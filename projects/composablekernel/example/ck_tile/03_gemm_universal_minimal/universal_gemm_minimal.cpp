// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <hip/hip_runtime.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include "ck_tile/host/kernel_launch.hpp"
#include "ck_tile/host/stream_config.hpp"
#include "ck_tile/ops/epilogue/cshuffle_epilogue.hpp"
#include "ck_tile/ops/gemm/kernel/gemm_tile_partitioner.hpp"
#include "ck_tile/ops/gemm/kernel/universal_gemm_kernel.hpp"
#include "ck_tile/ops/gemm/pipeline/gemm_pipeline_ag_bg_cr_comp_v3.hpp"
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

} // namespace

int main(int argc, char** argv)
{
    using ADataType   = ck_tile::fp16_t;
    using BDataType   = ck_tile::fp16_t;
    using AccDataType = float;
    using EDataType   = ck_tile::fp16_t;

    // Standard row-major GEMM: C[M,N] = A[M,K] * B[K,N]
    using ALayout = ck_tile::tensor_layout::gemm::RowMajor;
    using BLayout = ck_tile::tensor_layout::gemm::RowMajor;
    using ELayout = ck_tile::tensor_layout::gemm::RowMajor;

    ck_tile::index_t M = 4096;
    ck_tile::index_t N = 4096;
    ck_tile::index_t K = 4096;
    int warmup         = 50;
    int repeat         = 100;
    bool verify        = true;
    bool flush_cache   = false;

    // Minimal CLI (no ck_tile ArgParser dependency):
    //   --m <int> --n <int> --k <int> --warmup <int> --repeat <int>
    //   --verify <0|1>
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
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage:\n"
                      << "  " << argv[0] << " [--m M] [--n N] [--k K] [--warmup W] [--repeat R]\n"
                      << "    [--verify 0|1]\n";
            return 0;
        }
    }

    const ck_tile::index_t stride_A = K; // row-major A[M,K]
    const ck_tile::index_t stride_B = N; // row-major B[K,N]
    const ck_tile::index_t stride_E = N; // row-major E[M,N]

    // These values are compile-time parameters of the kernel.
    constexpr ck_tile::index_t M_Tile = 128;
    constexpr ck_tile::index_t N_Tile = 128;
    constexpr ck_tile::index_t K_Tile = 64; // must divide K when kPadK=false

    constexpr ck_tile::index_t M_Wave = 2;
    constexpr ck_tile::index_t N_Wave = 2;
    constexpr ck_tile::index_t K_Wave = 1;

    constexpr ck_tile::index_t M_PerXdl = 16;
    constexpr ck_tile::index_t N_PerXdl = 16;
    constexpr ck_tile::index_t K_PerXdl = ck_tile::get_k_warp_tile<ADataType, M_PerXdl>();

    using GemmShape = ck_tile::TileGemmShape<ck_tile::sequence<M_Tile, N_Tile, K_Tile>,
                                             ck_tile::sequence<M_Wave, N_Wave, K_Wave>,
                                             ck_tile::sequence<M_PerXdl, N_PerXdl, K_PerXdl>,
                                             false,
                                             false>;

    // TODO: check:
    // 1) Preshuffle
    // 2) Persistent
    // 3) Weights column/row majority
    // Traits describing layouts + optional padding.
    using Traits = ck_tile::TileGemmUniversalTraits</*kPadM=*/false,
                                                    /*kPadN=*/false,
                                                    /*kPadK=*/false,
                                                    /*DoubleSmemBuffer=*/false,
                                                    ALayout,
                                                    BLayout,
                                                    ELayout,
                                                    /*TransposeC=*/false,
                                                    /*UseStructuredSparsity=*/false,
                                                    /*UsePersistentKernel=*/false,
                                                    /*NumWaveGroups=*/1,
                                                    /*Preshuffle=*/false>;

    using Problem =
        ck_tile::UniversalGemmPipelineProblem<ADataType, BDataType, AccDataType, GemmShape, Traits>;

    using Pipeline = ck_tile::GemmPipelineAgBgCrCompV3<Problem>;

    // using TilePartitioner = ck_tile::GemmSpatiallyLocalTilePartitioner<GemmShape, /*GroupNum=*/8,
    // /*M01=*/4>;
    using TilePartitioner = ck_tile::GemmTile1DPartitioner<GemmShape>;

    using DsDataType      = ck_tile::tuple<>;
    using DsLayout        = ck_tile::tuple<>;
    using EpilogueProblem = ck_tile::CShuffleEpilogueProblem<ADataType,
                                                             BDataType,
                                                             DsDataType,
                                                             AccDataType,
                                                             EDataType,
                                                             DsLayout,
                                                             ELayout,
                                                             ck_tile::element_wise::PassThrough,
                                                             TilePartitioner::MPerBlock,
                                                             TilePartitioner::NPerBlock,
                                                             M_Wave,
                                                             N_Wave,
                                                             M_PerXdl,
                                                             N_PerXdl,
                                                             K_PerXdl,
                                                             Problem::TransposeC,
                                                             Traits::NumWaveGroups,
                                                             /*FixedVectorSize=*/false,
                                                             /*VectorSizeC=*/1,
                                                             /*TiledMMAPermuteN=*/false,
                                                             /*BlockedXDLN_PerWarp=*/1,
                                                             /*DoubleSmemBuffer=*/false>;

    using Epilogue = ck_tile::CShuffleEpilogue<EpilogueProblem>;
    using Kernel   = ck_tile::UniversalGemmKernel<TilePartitioner, Pipeline, Epilogue>;

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
    hip_check(hipMalloc(&dA, sizeof(ADataType) * static_cast<std::size_t>(M) * K), "hipMalloc(A)");
    hip_check(hipMalloc(&dB, sizeof(BDataType) * static_cast<std::size_t>(K) * N), "hipMalloc(B)");
    hip_check(hipMalloc(&dE, sizeof(EDataType) * static_cast<std::size_t>(M) * N), "hipMalloc(E)");

    hip_check(hipMemcpy(dA, hA.data(), sizeof(ADataType) * size_a, hipMemcpyHostToDevice),
              "hipMemcpy(A)");
    hip_check(hipMemcpy(dB, hB.data(), sizeof(BDataType) * size_b, hipMemcpyHostToDevice),
              "hipMemcpy(B)");
    hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E)");

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

    const auto kargs = Kernel::MakeKernelArgs(host_args);

    if(!Kernel::IsSupportedArgument(kargs))
    {
        std::cerr << "Kernel does not support the provided arguments (alignment/padding/shape).\n";
        std::cerr
            << "Try setting M,N,K to multiples of the tile sizes, or enable padding in Traits.\n";
        return 1;
    }

    const dim3 grids  = Kernel::GridSize(M, N, k_batch);
    const dim3 blocks = Kernel::BlockSize();

    // Time kernel execution (avg time in ms).
    ck_tile::stream_config s{};
    s.time_kernel_  = true;
    s.cold_niters_  = warmup;
    s.nrepeat_      = repeat;
    s.is_gpu_timer_ = true;
    s.flush_cache_  = flush_cache;

    const float ave_ms =
        ck_tile::launch_kernel(s, ck_tile::make_kernel<1>(Kernel{}, grids, blocks, 0, kargs));

    // GEMM FLOPs: 2*M*N*K
    const double flops =
        2.0 * static_cast<double>(M) * static_cast<double>(N) * static_cast<double>(K);
    const double tflops = flops / (static_cast<double>(ave_ms) * 1.0e-3) / 1.0e12;

    float max_abs_err = 0.0f;
    if(verify)
    {
        // Spot-check E[0,0] by computing a dot-product on CPU using A[0,:] and B[:,0].
        EDataType h0{};
        hip_check(hipMemcpy(&h0, dE, sizeof(EDataType), hipMemcpyDeviceToHost), "hipMemcpy(E[0])");

        float exp = 0.0f;
        for(ck_tile::index_t k = 0; k < K; ++k)
        {
            const float a = to_float(hA[static_cast<std::size_t>(k)]); // A[0,k]
            const float b =
                to_float(hB[static_cast<std::size_t>(k) * static_cast<std::size_t>(N)]); // B[k,0]
            exp += a * b;
        }
        const float got = to_float(h0);
        max_abs_err     = std::abs(got - exp);
    }

    std::cout << "UniversalGemm minimal example\n";
    std::cout << "M=" << M << " N=" << N << " K=" << K << "\n";
    std::cout << "warmup=" << warmup << " repeat=" << repeat << "\n";
    std::cout << "avg_ms=" << ave_ms << " tflops=" << tflops << "\n";
    if(verify)
        std::cout << "spotcheck_abs_err=" << max_abs_err << "\n";

    hip_check(hipFree(dA), "hipFree(A)");
    hip_check(hipFree(dB), "hipFree(B)");
    hip_check(hipFree(dE), "hipFree(E)");

    return 0;
}
