// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT
//
// Reference implementation for persistent-kernel GEMM + separate reduction + NCCL AllGather.
// Mirrors the structure of universal_gemm_persistent_all_reduce.cpp but replaces the
// fused in-kernel all-reduce + rocSHMEM broadcast with:
//   1. Simple persistent GEMM kernel (same dispatching, same tile partitioner)
//   2. Separate reduction kernel using the same per-row token_map
//   3. NCCL AllGather on the reduced outputs
//
// Used for performance comparison against the fused rocSHMEM version.

#include <hip/hip_runtime.h>
#include <rccl/rccl.h>

#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
#include <mpi.h>
#endif

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

#include "ck_tile/core/arch/arch.hpp"
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

inline void nccl_check(ncclResult_t r, const char* what)
{
    if(r != ncclSuccess)
    {
        std::cerr << "NCCL error (" << what << "): " << ncclGetErrorString(r) << "\n";
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


template <typename T>
constexpr T ceil_div(T a, T b)
{
    return (a + b - 1) / b;
}

// -----------------------------------------------------------------------
// PersistentGemmKernel — persistent GEMM (all WGs do compute).
//
// Unlike AllReduceGemmChunkedKernel in the fused version, this kernel does
// NOT perform in-kernel reduction or rocSHMEM broadcast.  It only runs
// GEMM tiles persistently.  The actual reduction and AllGather are done
// by separate kernels / NCCL collectives launched afterwards.
// -----------------------------------------------------------------------
template <typename BaseKernel_>
struct PersistentGemmKernel
{
    using BaseKernel      = BaseKernel_;
    using TilePartitioner = typename BaseKernel::TilePartitioner;
    using GemmPipeline    = typename BaseKernel::GemmPipeline;
    using BaseArgs        = typename BaseKernel::KernelArgs;
    using ADataType       = typename BaseKernel::ADataType;
    using BDataType       = typename BaseKernel::BDataType;
    using EDataType       = typename BaseKernel::EDataType;

    static constexpr ck_tile::index_t NumATensor = BaseKernel::NumATensor;
    static constexpr ck_tile::index_t NumBTensor = BaseKernel::NumBTensor;
    static constexpr ck_tile::index_t NumDTensor = BaseKernel::NumDTensor;

    // Keep KernelArgs trivially copyable (HIP kernel-parameter friendly).
    struct KernelArgs
    {
        BaseArgs base;
        uint32_t num_compute_wgs;         // WGs that do GEMM work (= grid size)
    };

    static constexpr ck_tile::index_t kBlockSize = BaseKernel::kBlockSize;

    CK_TILE_HOST_DEVICE static constexpr ck_tile::index_t GetSmemSize()
    {
        return BaseKernel::GetSmemSize();
    }

    CK_TILE_HOST static auto BlockSize() { return BaseKernel::BlockSize(); }

    // Forwarding: simple MakeKernelArgs for benchmark compatibility
    template <ck_tile::index_t NA, ck_tile::index_t NB, ck_tile::index_t ND>
    CK_TILE_HOST static KernelArgs
    MakeKernelArgs(const ck_tile::UniversalGemmHostArgs<NA, NB, ND>& hostArgs)
    {
        const auto base_args = BaseKernel::MakeKernelArgs(hostArgs);
        return KernelArgs{base_args, 0};
    }

    CK_TILE_HOST static bool IsSupportedArgument(const KernelArgs& args)
    {
        return BaseKernel::IsSupportedArgument(args.base);
    }

    CK_TILE_HOST static auto MaxOccupancyGridSize(const ck_tile::stream_config& s) -> dim3
    {
        return BaseKernel::MaxOccupancyGridSize(s);
    }

    CK_TILE_HOST static constexpr auto GridSize(ck_tile::index_t M, ck_tile::index_t N,
                                                ck_tile::index_t KBatch)
    {
        return BaseKernel::GridSize(M, N, KBatch);
    }

    CK_TILE_DEVICE void operator()(KernelArgs args) const
    {
        const auto wg_id = ck_tile::amd_wave_read_first_lane(static_cast<uint32_t>(blockIdx.x));

        // -----------------------------------------------------------------
        // All WGs: persistent GEMM tile loop
        // -----------------------------------------------------------------
        const auto num_compute = ck_tile::amd_wave_read_first_lane(args.num_compute_wgs);
        const auto num_tiles   = ck_tile::amd_wave_read_first_lane(
            static_cast<uint32_t>(TilePartitioner::GridSize(args.base.M, args.base.N)));
        const auto num_work = ck_tile::amd_wave_read_first_lane(
            num_tiles * static_cast<uint32_t>(args.base.k_batch));

        __shared__ char smem_ptr[BaseKernel::GetSmemSize()];

        auto block_id = wg_id;

        while(block_id < num_work)
        {
            ck_tile::s_waitcnt_barrier();

            const auto tile_idx =
                ck_tile::amd_wave_read_first_lane(block_id % num_tiles);
            const auto [iM, iN] =
                TilePartitioner{args.base.M, args.base.N}.GetOutputTileIndex(tile_idx);
            const ck_tile::index_t i_m =
                ck_tile::amd_wave_read_first_lane(iM * TilePartitioner::MPerBlock);
            const ck_tile::index_t i_n =
                ck_tile::amd_wave_read_first_lane(iN * TilePartitioner::NPerBlock);

            const auto k_batch_idx =
                ck_tile::amd_wave_read_first_lane(block_id / num_tiles);
            const typename BaseKernel::SplitKBatchOffset splitk_batch_offset(
                args.base, k_batch_idx);

            std::array<const ADataType*, NumATensor> as_ptr;
            ck_tile::static_for<0, NumATensor, 1>{}([&](auto i) {
                as_ptr[i] = static_cast<const ADataType*>(args.base.as_ptr[i]) +
                            splitk_batch_offset.as_k_split_offset[i];
            });

            std::array<const BDataType*, NumBTensor> bs_ptr;
            ck_tile::static_for<0, NumBTensor, 1>{}([&](auto i) {
                bs_ptr[i] = static_cast<const BDataType*>(args.base.bs_ptr[i]) +
                            splitk_batch_offset.bs_k_split_offset[i];
            });

            EDataType* e_ptr = static_cast<EDataType*>(args.base.e_ptr);

            BaseKernel::RunGemm(
                as_ptr, bs_ptr, args.base.ds_ptr, e_ptr, smem_ptr,
                args.base, splitk_batch_offset, i_m, i_n);

            block_id += num_compute;
        }
    }
};

// -----------------------------------------------------------------------
// Separate reduction kernel — launched after GEMM completes.
//
// Uses the same token_map layout as the fused version:
//   token_map[num_post_work × warp_size] of int32_t
//   Each group reduces `tokens_per_reduction` rows (first row = destination).
//
// Grid: enough warps to cover num_post_work groups.
// Block: 256 threads (4 warps per block).
// -----------------------------------------------------------------------
struct ReduceKernelArgs
{
    EDataType* e_ptr;          // GEMM output buffer (this PE's M × N matrix, read-only)
    EDataType* reduced_ptr;    // compact output [num_post_work × N], row-major, stride N
    const int32_t* token_map;  // [num_post_work × warp_size]
    uint32_t num_post_work;    // number of reduction groups
    uint32_t num_chunks;       // number of N-chunks
    uint32_t tiles_n_per_chunk;// N-tiles per chunk
    uint32_t N_per_tile;       // NPerBlock from tile partitioner
    ck_tile::index_t N;        // matrix N dimension (stride)
};

__global__ void reduce_kernel(ReduceKernelArgs args)
{
    static constexpr int vec_size  = 16 / sizeof(EDataType); // 8 fp16 per 16-byte load
    static constexpr int warp_size = 64; // AMD warp size
    static constexpr int vec_elements_per_load = warp_size * vec_size;

    const int lane_idx = threadIdx.x % warp_size;
    const int warp_in_block = threadIdx.x / warp_size;
    const int warps_per_block = blockDim.x / warp_size;
    const int global_warp_id = blockIdx.x * warps_per_block + warp_in_block;

    const uint32_t chunk_n_elems = args.tiles_n_per_chunk * args.N_per_tile;
    const uint32_t num_vec_iters = (chunk_n_elems + vec_elements_per_load - 1) / vec_elements_per_load;

    // Each warp handles one (chunk_idx, work_id) pair.
    // Total work items = num_chunks * num_post_work.
    const uint32_t total_work = args.num_chunks * args.num_post_work;

    for (uint32_t gw = static_cast<uint32_t>(global_warp_id); gw < total_work;
         gw += static_cast<uint32_t>(gridDim.x) * warps_per_block)
    {
        const uint32_t chunk_idx = gw / args.num_post_work;
        const uint32_t work_id   = gw % args.num_post_work;

        EDataType* chunk_base = args.e_ptr + chunk_idx * chunk_n_elems;

        const int32_t lane_token_idx =
            args.token_map[work_id * warp_size + lane_idx];

        for (uint32_t vi = 0; vi < num_vec_iters; vi++) {
            const uint32_t base_offset =
                vi * vec_elements_per_load + lane_idx * vec_size;

            EDataType acc[vec_size];
            for (int v = 0; v < vec_size; v++) {
                acc[v] = static_cast<EDataType>(0);
            }

            // Reduce over rows in this group (broadcast row index via readlane)
            for (int token_idx = 0; token_idx < warp_size; token_idx++) {
                int32_t row = __builtin_amdgcn_readlane(lane_token_idx, token_idx);
                if (row < 0) {
                    break; // sentinel: no more rows
                }

                const EDataType* row_ptr = chunk_base + row * args.N;

                if (base_offset + vec_size <= chunk_n_elems) {
                    const auto* vec_ptr =
                        reinterpret_cast<const ck_tile::fp16x8_t*>(
                            row_ptr + base_offset);
                    ck_tile::fp16x8_t vec = *vec_ptr;

                    for (int v = 0; v < vec_size; v++) {
                        acc[v] += vec[v];
                    }
                } else {
                    for (int v = 0; v < vec_size; v++) {
                        uint32_t idx = base_offset + v;
                        if (idx < chunk_n_elems) {
                            acc[v] += row_ptr[idx];
                        }
                    }
                }
            }

            // Store reduced result into compact buffer at row = work_id.
            // Layout: reduced_ptr[work_id * N + col], where
            //   col = chunk_idx * chunk_n_elems + base_offset
            EDataType* dst_ptr = args.reduced_ptr + work_id * args.N
                               + chunk_idx * chunk_n_elems;

            if (base_offset + vec_size <= chunk_n_elems) {
                ck_tile::fp16x8_t vec_out{};
                for (int v = 0; v < vec_size; v++) {
                    vec_out[v] = acc[v];
                }
                auto* vec_ptr =
                    reinterpret_cast<ck_tile::fp16x8_t*>(
                        dst_ptr + base_offset);
                *vec_ptr = vec_out;
            } else {
                for (int v = 0; v < vec_size; v++) {
                    uint32_t idx = base_offset + v;
                    if (idx < chunk_n_elems) {
                        dst_ptr[idx] = acc[v];
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// Kernel configuration template — identical to fused version.
// -----------------------------------------------------------------------
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

    // --- Partitioner (always N-chunked) ---
    using TilePartitioner = ck_tile::GemmTileNChunked1DPartitioner<GemmShape>;

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
    // Persistent (wrapped with PersistentGemmKernel for persistent tile loop)
    using PersistentKernelBase =
        ck_tile::UniversalGemmKernel<TilePartitioner, PersistentPipeline, PersistentEpilogue>;
    using PersistentKernel = PersistentGemmKernel<PersistentKernelBase>;

    // Non-persistent reference (plain UniversalGemmKernel — no wrapper)
    using NonPersistentKernel =
        ck_tile::UniversalGemmKernel<TilePartitioner, NonPersistentPipeline, NonPersistentEpilogue>;
};

// -----------------------------------------------------------------------
// Kernel configurations (identical to fused version).
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
// --- V4 pipeline ---
using Cfg10 = KernelConfig<        256, 256, 64,   2, 2, 1,  32,  32,  true,  true>;
using Cfg11 = KernelConfig<        128, 128, 64,   2, 2, 1,  32,  32,  true,  true>;
using Cfg12 = KernelConfig<        128, 128, 32,   2, 2, 1,  32,  32,  true,  true>;
using Cfg13 = KernelConfig<        128, 128, 64,   2, 2, 1,  16,  16,  true,  true>;
using Cfg14 = KernelConfig<         64,  64, 64,   1, 1, 1,  32,  32,  true,  true>;

} // namespace

int main(int argc, char** argv)
{
    // -----------------------------------------------------------------------
    // MPI + NCCL initialization (same pattern as universal_gemm_chuncked.cpp).
    // -----------------------------------------------------------------------
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    struct MpiGuard
    {
        MpiGuard(int* argc_, char*** argv_) { MPI_Init(argc_, argv_); }
        ~MpiGuard() { MPI_Finalize(); }
    } mpi_guard{&argc, &argv};
#endif

    int world_rank = 0;
    int world_size = 1;
    int local_rank = 0;

#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    {
        MPI_Comm local_comm{};
        MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &local_comm);
        MPI_Comm_rank(local_comm, &local_rank);
        MPI_Comm_free(&local_comm);
    }
#endif

    // Bind this rank to its GPU before any HIP / NCCL calls.
    int num_devices = 0;
    hip_check(hipGetDeviceCount(&num_devices), "hipGetDeviceCount");
    const int device_id = local_rank % num_devices;
    hip_check(hipSetDevice(device_id), "hipSetDevice");

    std::cout << "[rank " << world_rank << "/" << world_size
              << "] local_rank=" << local_rank
              << " -> GPU " << device_id << "/" << num_devices << "\n";

    // NCCL communicator
    ncclUniqueId uid{};
    if(world_rank == 0)
    {
        nccl_check(ncclGetUniqueId(&uid), "ncclGetUniqueId");
    }
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    MPI_Bcast(&uid, static_cast<int>(sizeof(uid)), MPI_BYTE, 0, MPI_COMM_WORLD);
#endif

    ncclComm_t nccl_comm{};
    nccl_check(ncclCommInitRank(&nccl_comm, /*nranks=*/world_size, uid, /*rank=*/world_rank),
               "ncclCommInitRank");

    std::cout << "[rank " << world_rank << "/" << world_size << "] NCCL initialized\n";

    ck_tile::index_t M = 4096;
    ck_tile::index_t N = 4096;
    ck_tile::index_t K = 4096;
    int warmup         = 50;
    int repeat         = 100;
    bool verify        = true;
    bool flush_cache   = false;
    bool compare_non_persistent = true;
    int config_id      = 0;
    uint32_t forced_num_compute_wgs  = 0;
    uint32_t tokens_per_reduction = 0;

    // Minimal CLI (same as fused version):
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
        else if(std::strcmp(key, "--forced_num_compute_wgs") == 0)
            forced_num_compute_wgs = static_cast<uint32_t>(parse_long(need_value("--forced_num_compute_wgs"), "--forced_num_compute_wgs"));
        else if(std::strcmp(key, "--tokens_per_reduction") == 0)
            tokens_per_reduction = static_cast<uint32_t>(parse_long(need_value("--tokens_per_reduction"), "--tokens_per_reduction"));
        else if(std::strcmp(key, "--config") == 0)
            config_id = static_cast<int>(parse_long(need_value("--config"), "--config"));
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage:\n"
                      << "  " << argv[0]
                      << " [--m M] [--n N] [--k K] [--warmup W] [--repeat R]\n"
                      << "    [--verify 0|1] [--flush_cache 0|1] [--compare 0|1]\n"
                      << "    [--forced_num_compute_wgs N]\n"
                      << "    [--tokens_per_reduction T] [--config 0-14]\n"
                      << "\n"
                      << "  Reference version: persistent GEMM + separate reduction + NCCL AllGather\n";
            return 0;
        }
    }

    const ck_tile::index_t stride_A = K;
    const ck_tile::index_t stride_B = N;
    const ck_tile::index_t stride_E = N;

    // -----------------------------------------------------------------------
    // Allocate host / device memory.
    // -----------------------------------------------------------------------
    const std::size_t size_a = static_cast<std::size_t>(M) * static_cast<std::size_t>(K);
    const std::size_t size_b = static_cast<std::size_t>(K) * static_cast<std::size_t>(N);
    const std::size_t size_e = static_cast<std::size_t>(M) * static_cast<std::size_t>(N);

    std::vector<ADataType> hA(size_a);
    std::vector<BDataType> hB(size_b);

    std::mt19937 rng(20260205u);
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);

    for(std::size_t i = 0; i < size_a; ++i)
        hA[i] = from_float<ADataType>(dist(rng));
    for(std::size_t i = 0; i < size_b; ++i)
        hB[i] = from_float<BDataType>(dist(rng));

    ADataType* dA = nullptr;
    BDataType* dB = nullptr;
    hip_check(hipMalloc(&dA, sizeof(ADataType) * size_a), "hipMalloc(A)");
    hip_check(hipMalloc(&dB, sizeof(BDataType) * size_b), "hipMalloc(B)");

    // Output buffer: each PE gets its own dE via hipMalloc.
    // After reduction, NCCL AllGather collects reduced rows into dE_all:
    //   dE     — this PE's local GEMM output [M × N]
    //   dE_all — gathered reduced output [n_pes × num_post_work × N]
    //            (falls back to [n_pes × M × N] when reduction is disabled)
    const int n_pes = world_size;
    // const int my_pe = device_id;

    EDataType* dE = nullptr;
    hip_check(hipMalloc(&dE, sizeof(EDataType) * size_e), "hipMalloc(E)");

    // dE_all is allocated later, after num_post_work is known (see below).
    EDataType* dE_all = nullptr;

    hip_check(
        hipMemcpy(dA, hA.data(), sizeof(ADataType) * size_a, hipMemcpyHostToDevice), "hipMemcpy(A)");
    hip_check(
        hipMemcpy(dB, hB.data(), sizeof(BDataType) * size_b, hipMemcpyHostToDevice), "hipMemcpy(B)");
    hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E)");

    // -----------------------------------------------------------------------
    // Build token_map for post-processing row-reduction (identical to fused version).
    // -----------------------------------------------------------------------
    static constexpr int kWarpSize = 64;
    int32_t* d_token_map           = nullptr;
    uint32_t num_post_work         = 0;

    if(tokens_per_reduction > 0)
    {
        if(tokens_per_reduction > static_cast<uint32_t>(kWarpSize))
        {
            std::cerr << "--tokens_per_reduction (" << tokens_per_reduction
                      << ") must be <= warp_size (" << kWarpSize << ")\n";
            return 2;
        }

        const uint32_t Mu    = static_cast<uint32_t>(M);
        const uint32_t stride = (Mu + tokens_per_reduction - 1) / tokens_per_reduction;
        num_post_work         = stride;

        const std::size_t map_elems = static_cast<std::size_t>(num_post_work) * kWarpSize;
        std::vector<int32_t> h_token_map(map_elems, -1);

        for(uint32_t g = 0; g < num_post_work; g++)
        {
            for(uint32_t t = 0; t < tokens_per_reduction; t++)
            {
                const uint32_t row = g + t * stride;
                if(row < Mu)
                {
                    h_token_map[static_cast<std::size_t>(g) * kWarpSize + t] =
                        static_cast<int32_t>(row);
                }
            }
        }

        hip_check(hipMalloc(&d_token_map, sizeof(int32_t) * map_elems),
                  "hipMalloc(token_map)");
        hip_check(hipMemcpy(d_token_map, h_token_map.data(),
                            sizeof(int32_t) * map_elems, hipMemcpyHostToDevice),
                  "hipMemcpy(token_map)");

        std::cout << "token_map: num_post_work=" << num_post_work
                  << " tokens_per_reduction=" << tokens_per_reduction
                  << " stride=" << stride
                  << " (warp_size-aligned groups of " << kWarpSize << ")\n";
    }

    // -----------------------------------------------------------------------
    // Allocate compact reduced-output buffer for AllGather.
    //
    // Layout: [num_post_work × N] — one row per reduction group, stride N.
    // NCCL AllGather sends this (not the full M×N) to all PEs.
    // The gathered output is [n_pes × num_post_work × N].
    // -----------------------------------------------------------------------
    EDataType* dE_reduced = nullptr;
    const std::size_t size_reduced =
        static_cast<std::size_t>(num_post_work) * static_cast<std::size_t>(N);

    if(num_post_work > 0)
    {
        hip_check(hipMalloc(&dE_reduced, sizeof(EDataType) * size_reduced),
                  "hipMalloc(E_reduced)");
        hip_check(hipMemset(dE_reduced, 0, sizeof(EDataType) * size_reduced),
                  "hipMemset(E_reduced)");

        std::cout << "Reduced buffer: " << (sizeof(EDataType) * size_reduced)
                  << " bytes (" << num_post_work << " × " << N
                  << " × " << sizeof(EDataType) << "B)\n";
    }

    // -----------------------------------------------------------------------
    // Allocate AllGather receive buffer (deferred — depends on num_post_work).
    //
    // When reduction is enabled:  [n_pes × num_post_work × N]  (compact)
    // When reduction is disabled: [n_pes × M × N]              (full GEMM)
    // -----------------------------------------------------------------------
    const std::size_t ag_send_count = (num_post_work > 0) ? size_reduced : size_e;
    const std::size_t size_ag_total = static_cast<std::size_t>(n_pes) * ag_send_count;
    hip_check(hipMalloc(&dE_all, sizeof(EDataType) * size_ag_total), "hipMalloc(E_all)");
    hip_check(hipMemset(dE_all, 0, sizeof(EDataType) * size_ag_total), "hipMemset(E_all)");

    // -----------------------------------------------------------------------
    // Build kernel arguments.
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
        static_cast<void*>(dE),  // this PE's local output buffer
        k_batch,
        M,
        N,
        K,
        stride_As,
        stride_Bs,
        stride_Ds,
        stride_E};

    // -----------------------------------------------------------------------
    // Stream configuration.
    // -----------------------------------------------------------------------
    ck_tile::stream_config s{};
    s.time_kernel_  = true;
    s.cold_niters_  = warmup;
    s.nrepeat_      = repeat;
    s.is_gpu_timer_ = true;
    s.flush_cache_  = flush_cache;

    const double flops =
        2.0 * static_cast<double>(M) * static_cast<double>(N) * static_cast<double>(K);


    // Start test
    // -----------------------------------------------------------------------
    // Generic run function.
    // -----------------------------------------------------------------------
    auto run_benchmark = [&](auto persistent_kernel_tag, auto non_persistent_kernel_tag,
                             auto block_per_cu_tag) -> int {
        using PersistKernel    = typename decltype(persistent_kernel_tag)::type;
        using NonPersistKernel = typename decltype(non_persistent_kernel_tag)::type;
        constexpr int kBlockPerCu = decltype(block_per_cu_tag)::value;

        // --- Persistent kernel args ---
        auto persistent_kargs = PersistKernel::MakeKernelArgs(host_args);

        if(!PersistKernel::IsSupportedArgument(persistent_kargs))
        {
            std::cerr << "Persistent kernel does not support the provided arguments "
                         "(alignment/padding/shape).\n";
            std::cerr
                << "Try setting M,N,K to multiples of the tile sizes, or enable padding.\n";
            return 1;
        }

        dim3 persistent_grids        = PersistKernel::MaxOccupancyGridSize(s);
        const dim3 persistent_blocks = PersistKernel::BlockSize();

        if(forced_num_compute_wgs > 0)
        {
            if (forced_num_compute_wgs < persistent_grids.x) {
                std::cerr << "WARNING: Using less CUs than the total number of CUs available. "
                          << "Using " << forced_num_compute_wgs << " CUs instead of "
                          << persistent_grids.x << " CUs.\n";
            }
            persistent_kargs.num_compute_wgs = forced_num_compute_wgs;
            persistent_grids.x               = forced_num_compute_wgs;
        }
        else
        {
            persistent_kargs.num_compute_wgs = persistent_grids.x;
        }

        if(tokens_per_reduction == 0)
        {
            std::cerr << "WARNING: tokens_per_reduction == 0, skipping reduction and AllGather. "
                      << "Benchmarking pure persistent GEMM only.\n";
        }

        // --- Reduction kernel args ---
        // The ref version always uses a single chunk (num_chunks=1) since
        // reduction runs after GEMM completes — no overlap needed.
        using TP = typename PersistKernel::TilePartitioner;
        const uint32_t tiles_n =
            (static_cast<uint32_t>(N) + TP::NPerBlock - 1) / TP::NPerBlock;

        ReduceKernelArgs reduce_kargs{};
        reduce_kargs.e_ptr              = dE;            // source: full M×N GEMM output
        reduce_kargs.reduced_ptr        = dE_reduced;    // destination: compact num_post_work×N
        reduce_kargs.token_map          = d_token_map;
        reduce_kargs.num_post_work      = num_post_work;
        reduce_kargs.num_chunks         = 1;
        reduce_kargs.tiles_n_per_chunk  = tiles_n;
        reduce_kargs.N_per_tile         = TP::NPerBlock;
        reduce_kargs.N                  = N;

        // Reduction kernel grid: enough warps to cover all work items
        const uint32_t reduce_total_work = num_post_work;
        const int reduce_block_size = 256; // 4 warps per block
        const int warps_per_reduce_block = reduce_block_size / kWarpSize;
        const int reduce_grid_size = (reduce_total_work > 0)
            ? static_cast<int>((reduce_total_work + warps_per_reduce_block - 1) / warps_per_reduce_block)
            : 0;

        std::cout << "Persistent kernel (n_chunked, ref)"
                  << " grid_size=" << persistent_grids.x
                  << " block_size=" << persistent_blocks.x
                  << " num_compute_wgs=" << persistent_kargs.num_compute_wgs
                  << " num_post_work=" << num_post_work
                  << " tokens_per_reduction=" << tokens_per_reduction
                  << " kBlockPerCu=" << kBlockPerCu
                  << "\n";
        if(reduce_grid_size > 0)
        {
            std::cout << "Reduction kernel: grid_size=" << reduce_grid_size
                      << " block_size=" << reduce_block_size << "\n";
        }
        std::cout << "NCCL AllGather: n_pes=" << n_pes
                  << " sendcount=" << ag_send_count
                  << (num_post_work > 0 ? " (reduced rows only)" : " (full M×N)") << "\n";

        // --- Benchmark: GEMM + Reduce + AllGather ---
        const float persistent_ms = ck_tile::launch_kernel(
            s,
            ck_tile::make_kernel<kBlockPerCu>(
                PersistKernel{}, persistent_grids, persistent_blocks, 0, persistent_kargs),
            [&](const ck_tile::stream_config& sc) {
                // Separate reduction kernel (only if tokens_per_reduction > 0)
                if(reduce_grid_size > 0)
                {
                    reduce_kernel<<<reduce_grid_size, reduce_block_size, 0, sc.stream_id_>>>(
                        reduce_kargs);
                }
                // NCCL AllGather: send reduced rows (skip when no reduction)
                if(tokens_per_reduction > 0)
                {
                    const void* ag_send_ptr = (dE_reduced != nullptr)
                        ? static_cast<const void*>(dE_reduced)
                        : static_cast<const void*>(dE);
                    nccl_check(ncclAllGather(
                        ag_send_ptr,
                        static_cast<void*>(dE_all),
                        ag_send_count,
                        ncclFloat16,
                        nccl_comm,
                        sc.stream_id_),
                        "ncclAllGather");
                }
            });

        const double persistent_tflops =
            (persistent_ms > 0.0f)
                ? (flops / (static_cast<double>(persistent_ms) * 1.0e-3) / 1.0e12)
                : 0.0;

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

                std::cout << "Non-persistent reference (n_chunked)"
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
        std::cout << "UniversalGemm persistent kernel benchmark (REF: GEMM + Reduce + NCCL AllGather)\n";
        std::cout << "M=" << M << " N=" << N << " K=" << K
                  << " partitioner=n_chunked"
                  << " config=" << config_id << "\n";
        std::cout << "warmup=" << warmup << " repeat=" << repeat
                  << " kBlockPerCu=" << kBlockPerCu << "\n";
        std::cout << "\n";
        std::cout << "  [persistent+reduce+allgather] avg_ms=" << persistent_ms
                  << "  tflops=" << persistent_tflops << "\n";
        if(compare_non_persistent && non_persistent_ms > 0.0f)
        {
            std::cout << "  [non-persistent]              avg_ms=" << non_persistent_ms
                      << "  tflops=" << non_persistent_tflops << "\n";
            const float speedup = non_persistent_ms / persistent_ms;
            std::cout << "  speedup (persistent+ref / non-persistent) = " << speedup << "x\n";
        }
        // --- Full reference verification ---
        if(verify)
        {
            std::cout << "\n=== Verification (reference calculation) ===\n" << std::flush;

            // 1. Compute CPU reference GEMM (fp32 accumulation)
            std::cout << "Computing CPU reference GEMM ("
                      << M << " x " << N << " x " << K << ")..." << std::flush;
            std::vector<float> ref_E(size_e, 0.0f);
            for(ck_tile::index_t i = 0; i < M; i++)
            {
                for(ck_tile::index_t kk = 0; kk < K; kk++)
                {
                    const float a_val = to_float(hA[static_cast<std::size_t>(i) * K + kk]);
                    for(ck_tile::index_t j = 0; j < N; j++)
                    {
                        ref_E[static_cast<std::size_t>(i) * N + j] +=
                            a_val * to_float(hB[static_cast<std::size_t>(kk) * N + j]);
                    }
                }
            }
            std::cout << " done.\n";

            // 2. Zero all GPU output buffers
            hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e),
                      "hipMemset(E, verify)");
            if(dE_reduced)
                hip_check(hipMemset(dE_reduced, 0, sizeof(EDataType) * size_reduced),
                          "hipMemset(E_reduced, verify)");
            hip_check(hipMemset(dE_all, 0, sizeof(EDataType) * size_ag_total),
                      "hipMemset(E_all, verify)");

            // 3. Run persistent GEMM once (not timed)
            std::cout << "Running GEMM kernel..." << std::flush;
            {
                ck_tile::stream_config sv{};
                ck_tile::launch_kernel(
                    sv,
                    ck_tile::make_kernel<kBlockPerCu>(
                        PersistKernel{}, persistent_grids, persistent_blocks,
                        0, persistent_kargs));
            }

            // 4. Run reduction + AllGather (if tokens_per_reduction > 0)
            if(reduce_grid_size > 0)
            {
                std::cout << "Running reduction kernel..." << std::flush;
                reduce_kernel<<<reduce_grid_size, reduce_block_size>>>(reduce_kargs);
            }
            if(tokens_per_reduction > 0)
            {
                std::cout << "Running NCCL AllGather..." << std::flush;
                const void* ag_send_ptr = (dE_reduced != nullptr)
                    ? static_cast<const void*>(dE_reduced)
                    : static_cast<const void*>(dE);
                nccl_check(ncclAllGather(
                    ag_send_ptr, static_cast<void*>(dE_all),
                    ag_send_count, ncclFloat16, nccl_comm, nullptr),
                    "ncclAllGather(verify)");
                hip_check(hipDeviceSynchronize(), "sync(verify allgather)");
            }

            // 5. Validate GEMM output
            {
                std::vector<EDataType> hE_gpu(size_e);
                hip_check(hipMemcpy(hE_gpu.data(), dE,
                                    sizeof(EDataType) * size_e,
                                    hipMemcpyDeviceToHost), "hipMemcpy(E, verify)");

                double max_abs = 0.0, sum_sq = 0.0;
                double max_abs_gpu = 0.0, max_abs_ref = 0.0;
                double gpu_min = 1e30, gpu_max = -1e30;
                double ref_min = 1e30, ref_max = -1e30;
                for(std::size_t i = 0; i < size_e; i++)
                {
                    const double gv = static_cast<double>(to_float(hE_gpu[i]));
                    const double rv = ref_E[i];
                    const double err = std::abs(gv - rv);
                    if(err > max_abs) { max_abs = err; max_abs_gpu = gv; max_abs_ref = rv; }
                    sum_sq += err * err;
                    gpu_min = std::min(gpu_min, gv); gpu_max = std::max(gpu_max, gv);
                    ref_min = std::min(ref_min, rv); ref_max = std::max(ref_max, rv);
                }
                std::cout << "GEMM [" << M << " x " << N << "]:  "
                          << "max_abs_err=" << max_abs
                          << "  (gpu=" << max_abs_gpu << " ref=" << max_abs_ref << ")"
                          << "  rmse=" << std::sqrt(sum_sq / size_e)
                          << "  gpu_range=[" << gpu_min << ", " << gpu_max << "]"
                          << "  ref_range=[" << ref_min << ", " << ref_max << "]\n";
            }

            // 6. Validate reduction + AllGather output
            if(tokens_per_reduction > 0 && num_post_work > 0)
            {
                // CPU reduction reference (fp32 accum of fp16-rounded GEMM values)
                const uint32_t Mu     = static_cast<uint32_t>(M);
                const uint32_t stride = (Mu + tokens_per_reduction - 1) / tokens_per_reduction;

                std::vector<float> ref_reduced(size_reduced, 0.0f);
                for(uint32_t g = 0; g < num_post_work; g++)
                {
                    for(uint32_t t = 0; t < tokens_per_reduction; t++)
                    {
                        const uint32_t row = g + t * stride;
                        if(row >= Mu) break;
                        for(ck_tile::index_t j = 0; j < N; j++)
                        {
                            // Use fp16-rounded GEMM values (matches what GPU reduce reads)
                            ref_reduced[static_cast<std::size_t>(g) * N + j] +=
                                to_float(from_float<EDataType>(
                                    ref_E[static_cast<std::size_t>(row) * N + j]));
                        }
                    }
                }

                // Validate dE_reduced
                if(dE_reduced)
                {
                    std::vector<EDataType> hR_gpu(size_reduced);
                    hip_check(hipMemcpy(hR_gpu.data(), dE_reduced,
                                        sizeof(EDataType) * size_reduced,
                                        hipMemcpyDeviceToHost),
                              "hipMemcpy(E_reduced, verify)");

                    double max_abs = 0.0, sum_sq = 0.0;
                    double max_abs_gpu = 0.0, max_abs_ref = 0.0;
                    double gpu_min = 1e30, gpu_max = -1e30;
                    double ref_min = 1e30, ref_max = -1e30;
                    for(std::size_t i = 0; i < size_reduced; i++)
                    {
                        const double gv = static_cast<double>(to_float(hR_gpu[i]));
                        const double rv = ref_reduced[i];
                        const double err = std::abs(gv - rv);
                        if(err > max_abs) { max_abs = err; max_abs_gpu = gv; max_abs_ref = rv; }
                        sum_sq += err * err;
                        gpu_min = std::min(gpu_min, gv); gpu_max = std::max(gpu_max, gv);
                        ref_min = std::min(ref_min, rv); ref_max = std::max(ref_max, rv);
                    }
                    std::cout << "Reduce [" << num_post_work << " x " << N << "]:  "
                              << "max_abs_err=" << max_abs
                              << "  (gpu=" << max_abs_gpu << " ref=" << max_abs_ref << ")"
                              << "  rmse=" << std::sqrt(sum_sq / size_reduced)
                              << "  gpu_range=[" << gpu_min << ", " << gpu_max << "]"
                              << "  ref_range=[" << ref_min << ", " << ref_max << "]\n";
                }

                // Validate AllGather (each PE's slice should match ref_reduced)
                {
                    std::vector<EDataType> hAG_gpu(size_ag_total);
                    hip_check(hipMemcpy(hAG_gpu.data(), dE_all,
                                        sizeof(EDataType) * size_ag_total,
                                        hipMemcpyDeviceToHost),
                              "hipMemcpy(E_all, verify)");

                    double max_abs = 0.0, sum_sq = 0.0;
                    double max_abs_gpu = 0.0, max_abs_ref = 0.0;
                    double gpu_min = 1e30, gpu_max = -1e30;
                    double ref_min = 1e30, ref_max = -1e30;
                    for(int pe = 0; pe < n_pes; pe++)
                    {
                        for(std::size_t i = 0; i < size_reduced; i++)
                        {
                            // All PEs use same A,B -> same GEMM -> same reduction
                            const double gv = static_cast<double>(to_float(
                                hAG_gpu[static_cast<std::size_t>(pe) * size_reduced + i]));
                            const double rv = ref_reduced[i];
                            const double err = std::abs(gv - rv);
                            if(err > max_abs) { max_abs = err; max_abs_gpu = gv; max_abs_ref = rv; }
                            sum_sq += err * err;
                            gpu_min = std::min(gpu_min, gv); gpu_max = std::max(gpu_max, gv);
                            ref_min = std::min(ref_min, rv); ref_max = std::max(ref_max, rv);
                        }
                    }
                    std::cout << "AllGather [" << n_pes << " x " << num_post_work
                              << " x " << N << "]:  "
                              << "max_abs_err=" << max_abs
                              << "  (gpu=" << max_abs_gpu << " ref=" << max_abs_ref << ")"
                              << "  rmse=" << std::sqrt(sum_sq / size_ag_total)
                              << "  gpu_range=[" << gpu_min << ", " << gpu_max << "]"
                              << "  ref_range=[" << ref_min << ", " << ref_max << "]\n";
                }
            }

            std::cout << "=== Verification complete ===\n";
        }

        return 0;
    };

    // -----------------------------------------------------------------------
    // Dispatch based on config_id (always N-chunked partitioning).
    // -----------------------------------------------------------------------
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
        return run_benchmark(
            std::type_identity<typename Cfg::PersistentKernel>{},
            std::type_identity<typename Cfg::NonPersistentKernel>{},
            std::integral_constant<int, Cfg::kBlockPerCu>{});
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
    hip_check(hipFree(dE_all), "hipFree(E_all)");
    if(dE_reduced)
        hip_check(hipFree(dE_reduced), "hipFree(E_reduced)");
    if(d_token_map)
        hip_check(hipFree(d_token_map), "hipFree(token_map)");

    nccl_check(ncclCommDestroy(nccl_comm), "ncclCommDestroy");

    return rc;
}

