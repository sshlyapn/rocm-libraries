// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT
//
// Standalone persistent-kernel GEMM benchmark.
// Based on universal_gemm_chuncked.cpp but stripped of all overlap / RCCL / chunking logic.
// The kernel uses UniversalGemmKernel with UsePersistentKernel=true and is launched with
// MaxOccupancyGridSize() so that workgroups loop over tiles persistently.

#include <hip/hip_runtime.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <type_traits>
#include <vector>

#include "ck_tile/host/kernel_launch.hpp"
#include "ck_tile/host/stream_config.hpp"
#include "ck_tile/host/stream_utils.hpp"
#include "ck_tile/ops/epilogue/cshuffle_epilogue.hpp"
#include "ck_tile/ops/gemm/kernel/gemm_tile_partitioner.hpp"
#include "ck_tile/ops/gemm/kernel/universal_gemm_kernel.hpp"
#include "ck_tile/ops/gemm/pipeline/gemm_pipeline_ag_bg_cr_comp_v3.hpp"
#include "ck_tile/ops/gemm/pipeline/gemm_pipeline_ag_bg_cr_comp_v4.hpp"
#include "ck_tile/ops/gemm/pipeline/gemm_pipeline_problem.hpp"
#include "ck_tile/ops/gemm/pipeline/tile_gemm_shape.hpp"
#include "ck_tile/ops/gemm/pipeline/tile_gemm_traits.hpp"

namespace {

inline void hip_check(hipError_t e, const char* what)
{
    if(e != hipSuccess)
    {
        std::cerr << "HIP error (" << what << "): " << hipGetErrorString(e) << "\n";
        std::exit(1);
    }
}

template <typename T>
T from_float(float x)
{
    return ck_tile::type_convert<T>(x);
}

template <typename T>
float to_float(T x)
{
    return ck_tile::type_convert<float>(x);
}

inline long parse_long(const char* s, const char* what)
{
    char* end    = nullptr;
    const long v = std::strtol(s, &end, 10);
    if(end == s || (end && *end != '\0'))
    {
        std::cerr << "Failed to parse " << what << " from '" << s << "'\n";
        std::exit(2);
    }
    return v;
}

// -----------------------------------------------------------------------
// Data types and layouts (compile-time).
// -----------------------------------------------------------------------
using ADataType   = ck_tile::fp16_t;
using BDataType   = ck_tile::fp16_t;
using AccDataType = float;
using EDataType   = ck_tile::fp16_t;

using ALayout = ck_tile::tensor_layout::gemm::RowMajor;
using BLayout = ck_tile::tensor_layout::gemm::RowMajor;
using ELayout = ck_tile::tensor_layout::gemm::RowMajor;

// -----------------------------------------------------------------------
// Kernel configuration template — parameterised by tile sizes, pipeline, etc.
// -----------------------------------------------------------------------
// Template parameters:
//   M_Tile_, N_Tile_, K_Tile_  — block tile dimensions
//   M_Wave_, N_Wave_, K_Wave_  — warp grid within a block
//   M_XDL_, N_XDL_             — XDL (MFMA) tile dimensions
//   DblBuf_                    — use double-buffered shared memory
//   UseV4_                     — use COMPUTE_V4 pipeline (requires DblBuf_=true)
//   PadM_, PadN_, PadK_        — enable padding for non-aligned sizes
//   BlockPerCu_                — occupancy hint (__launch_bounds__ MinBlockPerCu)
template <ck_tile::index_t M_Tile_, ck_tile::index_t N_Tile_, ck_tile::index_t K_Tile_,
          ck_tile::index_t M_Wave_, ck_tile::index_t N_Wave_, ck_tile::index_t K_Wave_,
          ck_tile::index_t M_XDL_,  ck_tile::index_t N_XDL_,
          bool DblBuf_, bool UseV4_,
          bool PadM_ = false, bool PadN_ = false, bool PadK_ = false,
          int BlockPerCu_ = 1>
struct KernelConfig
{
    static constexpr ck_tile::index_t M_Tile = M_Tile_;
    static constexpr ck_tile::index_t N_Tile = N_Tile_;
    static constexpr ck_tile::index_t K_Tile = K_Tile_;
    static constexpr ck_tile::index_t M_Wave = M_Wave_;
    static constexpr ck_tile::index_t N_Wave = N_Wave_;
    static constexpr ck_tile::index_t K_Wave = K_Wave_;
    static constexpr ck_tile::index_t M_PerXdl = M_XDL_;
    static constexpr ck_tile::index_t N_PerXdl = N_XDL_;
    static constexpr ck_tile::index_t K_PerXdl =
        ck_tile::get_k_warp_tile<ADataType, M_PerXdl>();
    static constexpr bool DoubleSmemBuffer = DblBuf_;
    static constexpr bool PadM = PadM_;
    static constexpr bool PadN = PadN_;
    static constexpr bool PadK = PadK_;
    static constexpr int kBlockPerCu = BlockPerCu_;

    using GemmShape =
        ck_tile::TileGemmShape<ck_tile::sequence<M_Tile, N_Tile, K_Tile>,
                               ck_tile::sequence<M_Wave, N_Wave, K_Wave>,
                               ck_tile::sequence<M_PerXdl, N_PerXdl, K_PerXdl>,
                               false, false>;

    // --- Persistent ---
    using PersistentTraits =
        ck_tile::TileGemmUniversalTraits<PadM, PadN, PadK,
                                         DoubleSmemBuffer,
                                         ALayout, BLayout, ELayout,
                                         /*TransposeC=*/false,
                                         /*UseStructuredSparsity=*/false,
                                         /*UsePersistentKernel=*/true,
                                         /*NumWaveGroups=*/1,
                                         /*Preshuffle=*/false>;

    using PersistentProblem =
        ck_tile::UniversalGemmPipelineProblem<ADataType, BDataType, AccDataType,
                                              GemmShape, PersistentTraits>;

    using PersistentPipeline =
        std::conditional_t<UseV4_,
                           ck_tile::GemmPipelineAgBgCrCompV4<PersistentProblem>,
                           ck_tile::GemmPipelineAgBgCrCompV3<PersistentProblem>>;

    // --- Non-persistent ---
    using NonPersistentTraits =
        ck_tile::TileGemmUniversalTraits<PadM, PadN, PadK,
                                         DoubleSmemBuffer,
                                         ALayout, BLayout, ELayout,
                                         /*TransposeC=*/false,
                                         /*UseStructuredSparsity=*/false,
                                         /*UsePersistentKernel=*/false,
                                         /*NumWaveGroups=*/1,
                                         /*Preshuffle=*/false>;

    using NonPersistentProblem =
        ck_tile::UniversalGemmPipelineProblem<ADataType, BDataType, AccDataType,
                                              GemmShape, NonPersistentTraits>;

    using NonPersistentPipeline =
        std::conditional_t<UseV4_,
                           ck_tile::GemmPipelineAgBgCrCompV4<NonPersistentProblem>,
                           ck_tile::GemmPipelineAgBgCrCompV3<NonPersistentProblem>>;

    // --- Partitioners ---
    using RowMajorPartitioner  = ck_tile::GemmTile1DPartitioner<GemmShape>;
    using NChunkedPartitioner  = ck_tile::GemmTileNChunked1DPartitioner<GemmShape>;

    using DsDataType = ck_tile::tuple<>;
    using DsLayout   = ck_tile::tuple<>;

    // --- Epilogue (persistent) ---
    using PersistentEpilogueProblem =
        ck_tile::CShuffleEpilogueProblem<ADataType, BDataType, DsDataType,
                                         AccDataType, EDataType, DsLayout, ELayout,
                                         ck_tile::element_wise::PassThrough,
                                         M_Tile, N_Tile,
                                         M_Wave, N_Wave, M_PerXdl, N_PerXdl, K_PerXdl,
                                         PersistentProblem::TransposeC,
                                         PersistentTraits::NumWaveGroups,
                                         /*FixedVectorSize=*/false, /*VectorSizeC=*/1,
                                         /*TiledMMAPermuteN=*/false,
                                         /*BlockedXDLN_PerWarp=*/1,
                                         DoubleSmemBuffer>;
    using PersistentEpilogue = ck_tile::CShuffleEpilogue<PersistentEpilogueProblem>;

    // --- Epilogue (non-persistent) ---
    using NonPersistentEpilogueProblem =
        ck_tile::CShuffleEpilogueProblem<ADataType, BDataType, DsDataType,
                                         AccDataType, EDataType, DsLayout, ELayout,
                                         ck_tile::element_wise::PassThrough,
                                         M_Tile, N_Tile,
                                         M_Wave, N_Wave, M_PerXdl, N_PerXdl, K_PerXdl,
                                         NonPersistentProblem::TransposeC,
                                         NonPersistentTraits::NumWaveGroups,
                                         /*FixedVectorSize=*/false, /*VectorSizeC=*/1,
                                         /*TiledMMAPermuteN=*/false,
                                         /*BlockedXDLN_PerWarp=*/1,
                                         DoubleSmemBuffer>;
    using NonPersistentEpilogue = ck_tile::CShuffleEpilogue<NonPersistentEpilogueProblem>;

    // --- Kernel types ---
    using PersistentRowMajorKernel =
        ck_tile::UniversalGemmKernel<RowMajorPartitioner, PersistentPipeline, PersistentEpilogue>;
    using PersistentNChunkedKernel =
        ck_tile::UniversalGemmKernel<NChunkedPartitioner, PersistentPipeline, PersistentEpilogue>;

    using NonPersistentRowMajorKernel =
        ck_tile::UniversalGemmKernel<RowMajorPartitioner, NonPersistentPipeline, NonPersistentEpilogue>;
    using NonPersistentNChunkedKernel =
        ck_tile::UniversalGemmKernel<NChunkedPartitioner, NonPersistentPipeline, NonPersistentEpilogue>;
};

// -----------------------------------------------------------------------
// Kernel configurations (all V3 — V4 has constexpr issues in this CK version).
//
// Configs 0-3: original set
//   0: 128×128×64,  2×2 warp, 16×16 XDL  (baseline)
//   1: 256×256×64,  2×2 warp, 32×32 XDL  (larger tiles, bigger MFMA) — best so far ~823 TFLOPS
//   2: 256×256×128, 2×2 warp, 32×32 XDL  (larger tiles + deeper K unroll)
//   3: 128×128×128, 2×2 warp, 16×16 XDL  (baseline tiles + deeper K unroll)
//
// Configs 4-7: occupancy + padding experiments
//   4: 256×256×64,  2×2 warp, 32×32 XDL, kBlockPerCu=2  (2 blocks/CU occupancy)
//   5: 128×128×64,  2×2 warp, 16×16 XDL, kBlockPerCu=2  (smaller tile, 2 blocks/CU)
//   6: 128×128×128, 2×2 warp, 16×16 XDL, kBlockPerCu=2  (deep K, 2 blocks/CU)
//   7: 256×256×64,  2×2 warp, 32×32 XDL, padding=true    (padded for non-aligned sizes)
//
// Configs 8-9: warp layout experiments
//   8: 128×128×64,  4×1 warp, 32×32 XDL  (4 warps in M, 1 in N — better coalescing)
//   9: 128×128×64,  1×4 warp, 32×32 XDL  (1 warp in M, 4 in N)
// -----------------------------------------------------------------------
//                                  M    N    K    Mw Nw Kw  Mxdl Nxdl DblBuf V4   PadM  PadN  PadK  BPC
using Cfg0 = KernelConfig<         128, 128, 64,   2, 2, 1,  16,  16,  false, false>;
using Cfg1 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false>;
using Cfg2 = KernelConfig<         256, 256, 128,  2, 2, 1,  32,  32,  false, false>;
using Cfg3 = KernelConfig<         128, 128, 128,  2, 2, 1,  16,  16,  false, false>;
// --- occupancy + padding ---
using Cfg4 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false, false, false, false, 2>;
using Cfg5 = KernelConfig<         128, 128, 64,   2, 2, 1,  16,  16,  false, false, false, false, false, 2>;
using Cfg6 = KernelConfig<         128, 128, 128,  2, 2, 1,  16,  16,  false, false, false, false, false, 2>;
using Cfg7 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false, true,  true,  true,  1>;
// --- warp layout ---
using Cfg8 = KernelConfig<         128, 128, 64,   4, 1, 1,  32,  32,  false, false>;
using Cfg9 = KernelConfig<         128, 128, 64,   1, 4, 1,  32,  32,  false, false>;
// --- V4 pipeline (double-buffered LDS, ARegBRegCReg block GEMM) ---
//                                  M    N    K    Mw Nw Kw  Mxdl Nxdl DblBuf V4
using Cfg10 = KernelConfig<        256, 256, 64,   2, 2, 1,  32,  32,  true,  true>;  // 384 VGPRs - SPILLS
using Cfg11 = KernelConfig<        128, 128, 64,   2, 2, 1,  32,  32,  true,  true>;  // 128 VGPRs - fits
using Cfg12 = KernelConfig<        128, 128, 32,   2, 2, 1,  32,  32,  true,  true>;  //  96 VGPRs - fits
using Cfg13 = KernelConfig<        128, 128, 64,   2, 2, 1,  16,  16,  true,  true>;  // 128 VGPRs - fits (16x16 XDL)
using Cfg14 = KernelConfig<         64,  64, 64,   1, 1, 1,  32,  32,  true,  true>;  // 128 VGPRs - fits (small tile)

} // namespace

int main(int argc, char** argv)
{
    ck_tile::index_t M = 4096;
    ck_tile::index_t N = 4096;
    ck_tile::index_t K = 4096;
    int warmup         = 50;
    int repeat         = 100;
    bool verify        = true;
    bool flush_cache   = false;
    bool compare_non_persistent = true; // also run the non-persistent kernel for comparison
    float cu_scale     = 1.0f; // scale factor for persistent kernel CU occupancy (0.0, ...)
    bool n_chunked     = false; // use N-chunked (column-major) tile partitioner
    int config_id      = 0;    // kernel configuration (0-14)

    // Minimal CLI:
    //   --m <int> --n <int> --k <int> --warmup <int> --repeat <int>
    //   --verify <0|1> --flush_cache <0|1> --compare <0|1>
    for(int i = 1; i < argc; ++i)
    {
        const char* key = argv[i];
        auto need_value = [&](const char* opt) {
            if(i + 1 >= argc)
            {
                std::cerr << "Missing value for " << opt << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if(std::strcmp(key, "--m") == 0)
            M = static_cast<ck_tile::index_t>(parse_long(need_value("--m"), "--m"));
        else if(std::strcmp(key, "--n") == 0)
            N = static_cast<ck_tile::index_t>(parse_long(need_value("--n"), "--n"));
        else if(std::strcmp(key, "--k") == 0)
            K = static_cast<ck_tile::index_t>(parse_long(need_value("--k"), "--k"));
        else if(std::strcmp(key, "--warmup") == 0)
            warmup = static_cast<int>(parse_long(need_value("--warmup"), "--warmup"));
        else if(std::strcmp(key, "--repeat") == 0)
            repeat = static_cast<int>(parse_long(need_value("--repeat"), "--repeat"));
        else if(std::strcmp(key, "--verify") == 0)
            verify = (parse_long(need_value("--verify"), "--verify") != 0);
        else if(std::strcmp(key, "--flush_cache") == 0)
            flush_cache = (parse_long(need_value("--flush_cache"), "--flush_cache") != 0);
        else if(std::strcmp(key, "--compare") == 0)
            compare_non_persistent =
                (parse_long(need_value("--compare"), "--compare") != 0);
        else if(std::strcmp(key, "--cu_scale") == 0)
        {
            const char* val = need_value("--cu_scale");
            char* end       = nullptr;
            cu_scale        = std::strtof(val, &end);
            if(end == val || (end && *end != '\0') || cu_scale <= 0.0f)
            {
                std::cerr << "Invalid --cu_scale value '" << val
                          << "' (must be a positive float)\n";
                return 2;
            }
        }
        else if(std::strcmp(key, "--n_chunked") == 0)
            n_chunked = (parse_long(need_value("--n_chunked"), "--n_chunked") != 0);
        else if(std::strcmp(key, "--config") == 0)
            config_id = static_cast<int>(parse_long(need_value("--config"), "--config"));
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage:\n"
                      << "  " << argv[0]
                      << " [--m M] [--n N] [--k K] [--warmup W] [--repeat R]\n"
                      << "    [--verify 0|1] [--flush_cache 0|1] [--compare 0|1]\n"
                      << "    [--cu_scale F] [--n_chunked 0|1] [--config 0-9]\n"
                      << "\n"
                      << "  --compare    1    (default) also run the non-persistent kernel for comparison\n"
                      << "  --cu_scale   1.0  (default) scale factor for persistent kernel grid size\n"
                      << "  --n_chunked  0    (default) use N-chunked (column-major) tile partitioner\n"
                      << "                    When 1, tiles are ordered so that all M-tiles for a given\n"
                      << "                    N-column are computed first, then the next N-column, etc.\n"
                      << "  --config     0    (default) kernel tile configuration:\n"
                      << "                    0: 128x128x64,  2x2 warp, 16x16 XDL (baseline)\n"
                      << "                    1: 256x256x64,  2x2 warp, 32x32 XDL (larger tiles)\n"
                      << "                    2: 256x256x128, 2x2 warp, 32x32 XDL (larger tiles + deep K)\n"
                      << "                    3: 128x128x128, 2x2 warp, 16x16 XDL (baseline + deep K)\n"
                      << "                    4: 256x256x64,  2x2 warp, 32x32 XDL, kBlockPerCu=2\n"
                      << "                    5: 128x128x64,  2x2 warp, 16x16 XDL, kBlockPerCu=2\n"
                      << "                    6: 128x128x128, 2x2 warp, 16x16 XDL, kBlockPerCu=2\n"
                      << "                    7: 256x256x64,  2x2 warp, 32x32 XDL, padding=true\n"
                      << "                    8: 128x128x64,  4x1 warp, 32x32 XDL\n"
                      << "                    9: 128x128x64,  1x4 warp, 32x32 XDL\n"
                      << "                   10: 256x256x64,  2x2 warp, 32x32 XDL, V4 (spills!)\n"
                      << "                   11: 128x128x64,  2x2 warp, 32x32 XDL, V4\n"
                      << "                   12: 128x128x32,  2x2 warp, 32x32 XDL, V4\n"
                      << "                   13: 128x128x64,  2x2 warp, 16x16 XDL, V4\n"
                      << "                   14: 64x64x64,    1x1 warp, 32x32 XDL, V4\n";
            return 0;
        }
    }

    const ck_tile::index_t stride_A = K; // row-major A[M,K]
    const ck_tile::index_t stride_B = N; // row-major B[K,N]
    const ck_tile::index_t stride_E = N; // row-major E[M,N]

    // -----------------------------------------------------------------------
    // Allocate host / device memory.
    // -----------------------------------------------------------------------
    const std::size_t size_a = static_cast<std::size_t>(M) * static_cast<std::size_t>(K);
    const std::size_t size_b = static_cast<std::size_t>(K) * static_cast<std::size_t>(N);
    const std::size_t size_e = static_cast<std::size_t>(M) * static_cast<std::size_t>(N);

    std::vector<ADataType> hA(size_a);
    std::vector<BDataType> hB(size_b);

    std::mt19937 rng(20260205u); // deterministic seed for reproducibility
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);

    for(std::size_t i = 0; i < size_a; ++i)
        hA[i] = from_float<ADataType>(dist(rng));
    for(std::size_t i = 0; i < size_b; ++i)
        hB[i] = from_float<BDataType>(dist(rng));

    ADataType* dA = nullptr;
    BDataType* dB = nullptr;
    EDataType* dE = nullptr;
    hip_check(hipMalloc(&dA, sizeof(ADataType) * size_a), "hipMalloc(A)");
    hip_check(hipMalloc(&dB, sizeof(BDataType) * size_b), "hipMalloc(B)");
    hip_check(hipMalloc(&dE, sizeof(EDataType) * size_e), "hipMalloc(E)");

    hip_check(
        hipMemcpy(dA, hA.data(), sizeof(ADataType) * size_a, hipMemcpyHostToDevice), "hipMemcpy(A)");
    hip_check(
        hipMemcpy(dB, hB.data(), sizeof(BDataType) * size_b, hipMemcpyHostToDevice), "hipMemcpy(B)");
    hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E)");

    // -----------------------------------------------------------------------
    // Build kernel arguments (shared between persistent and non-persistent).
    // -----------------------------------------------------------------------
    constexpr ck_tile::index_t NumATensor = 1;
    constexpr ck_tile::index_t NumBTensor = 1;
    constexpr ck_tile::index_t NumDTensor = 0;
    constexpr ck_tile::index_t k_batch    = 1;

    const std::array<const void*, NumATensor> as_ptr         = {static_cast<const void*>(dA)};
    const std::array<const void*, NumBTensor> bs_ptr         = {static_cast<const void*>(dB)};
    const std::array<const void*, NumDTensor> ds_ptr         = {};
    const std::array<ck_tile::index_t, NumATensor> stride_As = {stride_A};
    const std::array<ck_tile::index_t, NumBTensor> stride_Bs = {stride_B};
    const std::array<ck_tile::index_t, NumDTensor> stride_Ds = {};

    const ck_tile::UniversalGemmHostArgs<NumATensor, NumBTensor, NumDTensor> host_args{
        as_ptr,
        bs_ptr,
        ds_ptr,
        static_cast<void*>(dE),
        k_batch,
        M,
        N,
        K,
        stride_As,
        stride_Bs,
        stride_Ds,
        stride_E};

    // -----------------------------------------------------------------------
    // Stream configuration (shared by all kernel variants).
    // -----------------------------------------------------------------------
    ck_tile::stream_config s{};
    s.time_kernel_  = true;
    s.cold_niters_  = warmup;
    s.nrepeat_      = repeat;
    s.is_gpu_timer_ = true;
    s.flush_cache_  = flush_cache;

    const double flops =
        2.0 * static_cast<double>(M) * static_cast<double>(N) * static_cast<double>(K);

    // -----------------------------------------------------------------------
    // Generic run function — works for any PersistentKernel / NonPersistentKernel pair.
    // kBlockPerCu is a compile-time constant forwarded to make_kernel<kBlockPerCu>().
    // -----------------------------------------------------------------------
    auto run_benchmark = [&](auto persistent_kernel_tag, auto non_persistent_kernel_tag,
                             const char* partitioner_name,
                             auto block_per_cu_tag) -> int {
        using PersistKernel    = typename decltype(persistent_kernel_tag)::type;
        using NonPersistKernel = typename decltype(non_persistent_kernel_tag)::type;
        constexpr int kBlockPerCu = decltype(block_per_cu_tag)::value;

        // --- Persistent kernel launch ---
        const auto persistent_kargs = PersistKernel::MakeKernelArgs(host_args);

        if(!PersistKernel::IsSupportedArgument(persistent_kargs))
        {
            std::cerr << "Persistent kernel (" << partitioner_name
                      << ") does not support the provided arguments "
                         "(alignment/padding/shape).\n";
            std::cerr
                << "Try setting M,N,K to multiples of the tile sizes, or enable padding.\n";
            return 1;
        }

        dim3 persistent_grids        = PersistKernel::MaxOccupancyGridSize(s);
        const dim3 persistent_blocks = PersistKernel::BlockSize();

        if(cu_scale != 1.0f)
        {
            const auto scaled = static_cast<unsigned int>(
                std::max(1.0f, std::round(static_cast<float>(persistent_grids.x) * cu_scale)));
            persistent_grids.x = scaled;
        }

        std::cout << "Persistent kernel (" << partitioner_name << ")"
                  << " grid_size=" << persistent_grids.x
                  << " block_size=" << persistent_blocks.x
                  << " cu_scale=" << cu_scale
                  << " kBlockPerCu=" << kBlockPerCu
                  << "\n";

        const float persistent_ms = ck_tile::launch_kernel(
            s,
            ck_tile::make_kernel<kBlockPerCu>(
                PersistKernel{}, persistent_grids, persistent_blocks, 0, persistent_kargs));

        const double persistent_tflops =
            (persistent_ms > 0.0f)
                ? (flops / (static_cast<double>(persistent_ms) * 1.0e-3) / 1.0e12)
                : 0.0;

        // --- Verification (spot-check E[0,0]) ---
        float max_abs_err = 0.0f;
        if(verify)
        {
            EDataType h0{};
            hip_check(
                hipMemcpy(&h0, dE, sizeof(EDataType), hipMemcpyDeviceToHost), "hipMemcpy(E[0])");

            float exp = 0.0f;
            for(ck_tile::index_t k = 0; k < K; ++k)
            {
                const float a = to_float(hA[static_cast<std::size_t>(k)]);
                const float b =
                    to_float(hB[static_cast<std::size_t>(k) * static_cast<std::size_t>(N)]);
                exp += a * b;
            }
            const float got = to_float(h0);
            max_abs_err     = std::abs(got - exp);
        }

        // --- Optional: non-persistent kernel for comparison ---
        float non_persistent_ms      = 0.0f;
        double non_persistent_tflops = 0.0;

        if(compare_non_persistent)
        {
            hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E, compare)");

            const auto non_persistent_kargs = NonPersistKernel::MakeKernelArgs(host_args);

            if(!NonPersistKernel::IsSupportedArgument(non_persistent_kargs))
            {
                std::cerr << "Non-persistent kernel does not support the provided arguments.\n";
            }
            else
            {
                const dim3 np_grids  = NonPersistKernel::GridSize(M, N, k_batch);
                const dim3 np_blocks = NonPersistKernel::BlockSize();

                std::cout << "Non-persistent kernel (" << partitioner_name << ")"
                          << " grid_size=" << np_grids.x
                          << " block_size=" << np_blocks.x << "\n";

                non_persistent_ms = ck_tile::launch_kernel(
                    s,
                    ck_tile::make_kernel<kBlockPerCu>(
                        NonPersistKernel{}, np_grids, np_blocks, 0, non_persistent_kargs));

                non_persistent_tflops =
                    (non_persistent_ms > 0.0f)
                        ? (flops / (static_cast<double>(non_persistent_ms) * 1.0e-3) / 1.0e12)
                        : 0.0;
            }
        }

        // --- Summary ---
        std::cout << "\n";
        std::cout << "UniversalGemm persistent kernel benchmark\n";
        std::cout << "M=" << M << " N=" << N << " K=" << K
                  << " partitioner=" << partitioner_name
                  << " config=" << config_id << "\n";
        std::cout << "warmup=" << warmup << " repeat=" << repeat
                  << " cu_scale=" << cu_scale
                  << " kBlockPerCu=" << kBlockPerCu << "\n";
        std::cout << "\n";
        std::cout << "  [persistent]     avg_ms=" << persistent_ms
                  << "  tflops=" << persistent_tflops << "\n";
        if(compare_non_persistent && non_persistent_ms > 0.0f)
        {
            std::cout << "  [non-persistent] avg_ms=" << non_persistent_ms
                      << "  tflops=" << non_persistent_tflops << "\n";
            const float speedup = non_persistent_ms / persistent_ms;
            std::cout << "  speedup (persistent / non-persistent) = " << speedup << "x\n";
        }
        if(verify)
            std::cout << "spotcheck_abs_err=" << max_abs_err << "\n";

        return 0;
    };

    // -----------------------------------------------------------------------
    // Dispatch based on config_id and partitioner choice.
    // -----------------------------------------------------------------------
    // Helper: dispatch for a given KernelConfig type.
    auto dispatch_config = [&](auto cfg_tag, const char* cfg_name) -> int {
        using Cfg = typename decltype(cfg_tag)::type;
        std::cout << "Config: " << cfg_name
                  << "  tile=" << Cfg::M_Tile << "x" << Cfg::N_Tile << "x" << Cfg::K_Tile
                  << "  warp=" << Cfg::M_Wave << "x" << Cfg::N_Wave << "x" << Cfg::K_Wave
                  << "  xdl=" << Cfg::M_PerXdl << "x" << Cfg::N_PerXdl << "x" << Cfg::K_PerXdl
                  << "  pad=" << Cfg::PadM << "/" << Cfg::PadN << "/" << Cfg::PadK
                  << "  dbl_buf=" << Cfg::DoubleSmemBuffer
                  << "  kBlockPerCu=" << Cfg::kBlockPerCu
                  << "\n\n";
        if(n_chunked)
        {
            return run_benchmark(
                std::type_identity<typename Cfg::PersistentNChunkedKernel>{},
                std::type_identity<typename Cfg::NonPersistentNChunkedKernel>{},
                "n_chunked",
                std::integral_constant<int, Cfg::kBlockPerCu>{});
        }
        else
        {
            return run_benchmark(
                std::type_identity<typename Cfg::PersistentRowMajorKernel>{},
                std::type_identity<typename Cfg::NonPersistentRowMajorKernel>{},
                "row_major",
                std::integral_constant<int, Cfg::kBlockPerCu>{});
        }
    };

    int rc = 0;
    switch(config_id)
    {
    case 0:
        rc = dispatch_config(std::type_identity<Cfg0>{}, "0: 128x128x64, V3, 16x16 XDL");
        break;
    case 1:
        rc = dispatch_config(std::type_identity<Cfg1>{}, "1: 256x256x64, V3, 32x32 XDL");
        break;
    case 2:
        rc = dispatch_config(std::type_identity<Cfg2>{}, "2: 256x256x128, V3, 32x32 XDL");
        break;
    case 3:
        rc = dispatch_config(std::type_identity<Cfg3>{}, "3: 128x128x128, V3, 16x16 XDL");
        break;
    case 4:
        rc = dispatch_config(std::type_identity<Cfg4>{}, "4: 256x256x64, V3, 32x32 XDL, BPC=2");
        break;
    case 5:
        rc = dispatch_config(std::type_identity<Cfg5>{}, "5: 128x128x64, V3, 16x16 XDL, BPC=2");
        break;
    case 6:
        rc = dispatch_config(std::type_identity<Cfg6>{}, "6: 128x128x128, V3, 16x16 XDL, BPC=2");
        break;
    case 7:
        rc = dispatch_config(std::type_identity<Cfg7>{}, "7: 256x256x64, V3, 32x32 XDL, padded");
        break;
    case 8:
        rc = dispatch_config(std::type_identity<Cfg8>{}, "8: 128x128x64, V3, 4x1 warp, 32x32 XDL");
        break;
    case 9:
        rc = dispatch_config(std::type_identity<Cfg9>{}, "9: 128x128x64, V3, 1x4 warp, 32x32 XDL");
        break;
    case 10:
        rc = dispatch_config(std::type_identity<Cfg10>{}, "10: 256x256x64, V4, 32x32 XDL (spills!)");
        break;
    case 11:
        rc = dispatch_config(std::type_identity<Cfg11>{}, "11: 128x128x64, V4, 32x32 XDL");
        break;
    case 12:
        rc = dispatch_config(std::type_identity<Cfg12>{}, "12: 128x128x32, V4, 32x32 XDL");
        break;
    case 13:
        rc = dispatch_config(std::type_identity<Cfg13>{}, "13: 128x128x64, V4, 16x16 XDL");
        break;
    case 14:
        rc = dispatch_config(std::type_identity<Cfg14>{}, "14: 64x64x64, V4, 32x32 XDL");
        break;
    default:
        std::cerr << "Invalid --config " << config_id << " (valid: 0-14)\n";
        return 2;
    }

    // -----------------------------------------------------------------------
    // Cleanup.
    // -----------------------------------------------------------------------
    hip_check(hipFree(dA), "hipFree(A)");
    hip_check(hipFree(dB), "hipFree(B)");
    hip_check(hipFree(dE), "hipFree(E)");

    return rc;
}

