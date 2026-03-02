// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT
//
// Standalone persistent-kernel GEMM benchmark.
// Based on universal_gemm_chuncked.cpp but stripped of all overlap / RCCL / chunking logic.
// The kernel uses UniversalGemmKernel with UsePersistentKernel=true and is launched with
// MaxOccupancyGridSize() so that workgroups loop over tiles persistently.

#include <hip/hip_runtime.h>

#include <rocshmem/rocshmem.hpp>
using namespace rocshmem;

// #if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
// #include <mpi.h>
// #endif

#include <array>
#include <cmath>
#include <fstream>
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

// Mixin: conditionally adds a `timing_buf` field to KernelArgs.
// When Enable=false the struct is empty and EBO eliminates any size overhead.
// When Enable=true  the struct contains a `uint64_t* timing_buf` member,
// accessible directly on the derived KernelArgs (e.g. args.timing_buf).
template <bool Enable>
struct TimingBufField {};

template <>
struct TimingBufField<true>
{
    uint64_t* timing_buf;   // [num_post_processing_wgs × num_chunks × 2]
                            // timing data (nullptr = disabled).
                            // Metrics per (pp_wg, chunk):
                            //   0: wait   1: reduce
};

template <typename BaseKernel_, bool TimeProfiling_ = false>
struct AllReduceGemmChunkedKernel
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
    static constexpr bool TimeProfiling = TimeProfiling_;

    // Keep KernelArgs trivially copyable (HIP kernel-parameter friendly):
    // do NOT add user-defined constructors/destructors here.
    // TimingBufField<TimeProfiling> adds `uint64_t* timing_buf` only when TimeProfiling=true.
    
    struct KernelArgs : TimingBufField<TimeProfiling>
    {
        BaseArgs base;
        uint32_t* chunk_barriers;
        uint32_t num_chunks;
        uint32_t tiles_n_per_chunk;
        uint32_t num_compute_wgs;         // WGs that do GEMM work
        uint32_t num_post_processing_wgs; // WGs that do reduction / other work
        const int32_t* token_map;         // [num_post_work × warp_size] row-index map
                                          // -1 = sentinel (no more rows in this group)
        uint32_t num_post_work;           // number of reduction groups (= work items for
                                          // post-processing warps)
        EDataType** ag_remote_ptrs;       // [num_pes] device array of pointers — one per PE —
                                          // to that PE's AllGather symmetric buffer.
                                          // Each PE's buffer has num_post_work × N elements.
                                          // Obtained via rocshmem_ptr() on the host.
        int num_pes;                      // number of PEs (GPUs) in the team
        int my_pe;                        // this PE's index
    };

    static constexpr ck_tile::index_t kBlockSize = BaseKernel::kBlockSize;

    CK_TILE_HOST static KernelArgs
    MakeKernelArgs(const BaseArgs& base,
                   uint32_t* chunk_barriers,
                   uint32_t num_chunks,
                   uint32_t num_compute_wgs,
                   uint32_t num_post_processing_wgs,
                   const int32_t* token_map,
                   uint32_t num_post_work)
    {
        const uint32_t tiles_n = ceil_div<uint32_t>(static_cast<uint32_t>(base.N),
                                                    static_cast<uint32_t>(TilePartitioner::NPerBlock));
        if (tiles_n % num_chunks != 0) {
            std::cerr << "Total number of N chunks must be divisible by num_chunks (tiles_n="
                      << tiles_n << ", num_chunks=" << num_chunks << ")\n";
            std::exit(1);
        }

        const uint32_t tiles_n_per_chunk = ceil_div<uint32_t>(tiles_n, num_chunks);
        return KernelArgs{{},   // TimingBufField base
                          base, chunk_barriers, num_chunks, tiles_n_per_chunk,
                          num_compute_wgs, num_post_processing_wgs,
                          token_map, num_post_work,
                          /*ag_remote_ptrs=*/nullptr};
    }

    CK_TILE_HOST_DEVICE static constexpr ck_tile::index_t GetSmemSize()
    {
        return BaseKernel::GetSmemSize();
    }

    CK_TILE_HOST static auto BlockSize() { return BaseKernel::BlockSize(); }

    // Forwarding: simple MakeKernelArgs for benchmark compatibility
    // (creates args with all WGs as compute, no post-processing, 1 chunk)
    template <ck_tile::index_t NA, ck_tile::index_t NB, ck_tile::index_t ND>
    CK_TILE_HOST static KernelArgs
    MakeKernelArgs(const ck_tile::UniversalGemmHostArgs<NA, NB, ND>& hostArgs)
    {
        const auto base_args = BaseKernel::MakeKernelArgs(hostArgs);
        // Default: 1 chunk, 0 post-processing WGs — will be patched by caller
        // num_compute_wgs is set to 0 here; caller must set it before launch.
        return KernelArgs{{},         // TimingBufField base (timing_buf=nullptr when enabled, no-op when disabled)
                          base_args,
                          nullptr,    // chunk_barriers
                          1,          // num_chunks
                          1,          // tiles_n_per_chunk
                          0,          // num_compute_wgs
                          0,          // num_post_processing_wgs
                          nullptr,    // token_map
                          0,          // num_post_work
                          nullptr,    // ag_remote_ptrs
                          1,          // num_pes
                          0};         // my_pe
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
        // Post-processing WGs: reduction / signalling (top of grid)
        // -----------------------------------------------------------------
        if(wg_id >= args.num_compute_wgs)
        {
            static constexpr int vec_size   = 16 / sizeof(EDataType); // 16 bytes are the widest global loads
            static constexpr int warp_size  = ck_tile::get_warp_size();
            static constexpr int vec_elements_per_load = warp_size * vec_size; // 512 fp16 (256 fp32) per warp-wide load
            
            const int lane_idx = ck_tile::get_lane_id();
            const uint32_t chunk_n_elems = args.tiles_n_per_chunk * TilePartitioner::NPerBlock;
            const uint32_t num_vec_iters = (chunk_n_elems + vec_elements_per_load - 1) / vec_elements_per_load;
            const uint32_t m_tiles = (args.base.M + TilePartitioner::MPerBlock - 1) / TilePartitioner::MPerBlock;
            const uint32_t num_tiles_per_chunk = args.tiles_n_per_chunk * m_tiles;
            const uint32_t warps_per_wg = blockDim.x / warp_size;
            const uint32_t warp_in_wg = threadIdx.x / warp_size;

            // Offset to this PE's slice in the symmetric buffer.
            // const uint64_t pe_offset_pp = static_cast<uint64_t>(args.my_pe)
            //                             * static_cast<uint64_t>(args.base.M)
            //                             * static_cast<uint64_t>(args.base.N);
            const uint64_t pe_offset_pp = 0;    

            // Global warp index across ALL PP WGs — used to distribute
            // reduction work evenly among all available warps.
            const uint32_t pp_wg_idx = wg_id - args.num_compute_wgs;
            const uint32_t global_warp = pp_wg_idx * warps_per_wg + warp_in_wg;
            const uint32_t total_pp_warps = args.num_post_processing_wgs * warps_per_wg;

            // Process chunks sequentially — all PP WGs collaborate on each chunk.
            for (uint32_t chunk_idx = 0; chunk_idx < args.num_chunks; chunk_idx++) {
                uint64_t t0 = 0, t1 = 0, t2 = 0;

                // --- Timing: before wait ---
                if constexpr (TimeProfiling) {
                    if (threadIdx.x == 0) {
                        t0 = __builtin_amdgcn_s_memrealtime();
                    }
                }

                if(threadIdx.x == 0)
                {
                    volatile uint32_t* flag = args.chunk_barriers + chunk_idx;
                    while(__atomic_load_n(flag, __ATOMIC_RELAXED) < num_tiles_per_chunk)
                    {
                        __builtin_amdgcn_s_sleep(1);
                    }
                }
                __syncthreads();

                // --- Timing: after wait ---
                if constexpr (TimeProfiling) {
                    if (threadIdx.x == 0) {
                        t1 = __builtin_amdgcn_s_memrealtime();
                    }
                }

                // Pointer to column-offset for this chunk within this PE's slice.
                EDataType* chunk_base = static_cast<EDataType*>(args.base.e_ptr) +
                                        pe_offset_pp + chunk_idx * chunk_n_elems;

                // const uint32_t row_bytes = chunk_n_elems * sizeof(EDataType);

                // =============================================================
                // Reduce + Put per warp (interleaved per work_id):
                //   Each warp reduces its assigned work_ids, fences once,
                //   then puts the reduced rows.  No cross-warp sync needed
                //   since each warp only puts data it reduced.
                // =============================================================
                auto work_id = global_warp;
                while (work_id < args.num_post_work) {
                    const int32_t lane_token_idx =
                        args.token_map[work_id * warp_size + lane_idx];

                    const int32_t dst_row = __builtin_amdgcn_readlane(lane_token_idx, 0);

                    for (uint32_t vi = 0; vi < num_vec_iters; vi++) {
                        const uint32_t base_offset =
                            vi * vec_elements_per_load + lane_idx * vec_size;

                        EDataType acc[vec_size];
                        for (int v = 0; v < vec_size; v++) {
                            acc[v] = static_cast<EDataType>(0);
                        }

                        for (int token_idx = 0; token_idx < warp_size; token_idx++) {
                            int32_t row = __builtin_amdgcn_readlane(lane_token_idx,
                                                                    token_idx);
                            if (row < 0) break;

                            const EDataType* row_ptr =
                                chunk_base + row * args.base.N;

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

                        // Store reduced result
                        if (base_offset + vec_size <= chunk_n_elems) {
                            ck_tile::fp16x8_t vec_out{};
                            for (int v = 0; v < vec_size; v++) {
                                vec_out[v] = acc[v];
                            }

                            for (int i = 0; i < args.num_pes; i++) {
                                const int pe = (args.my_pe + i) % args.num_pes;

                                auto* vec_ptr =
                                    reinterpret_cast<ck_tile::fp16x8_t*>(
                                        args.ag_remote_ptrs[pe] + args.my_pe * args.base.N * args.num_post_work + dst_row * args.base.N + chunk_idx * chunk_n_elems + base_offset);
                                *vec_ptr = vec_out;

                                // rocshmem_putmem_nbi_wave(
                                //     dst_ptr,
                                //     dst_ptr,
                                //     row_bytes,
                                //     pe);
                            }


                        } else if (base_offset < chunk_n_elems) {
                            for (int v = 0; v < vec_size; v++) {
                                for (int i = 0; i < args.num_pes; i++) {
                                    const int pe = (args.my_pe + i) % args.num_pes;

                                    auto* dst_ptr = args.ag_remote_ptrs[pe] + args.my_pe * args.base.N * args.num_post_work + dst_row * args.base.N + chunk_idx * chunk_n_elems;
                                    uint32_t idx = base_offset + v;
                                    if (idx < chunk_n_elems) {
                                        dst_ptr[idx] = acc[v];
                                    }
                                }
                            }
                        }
                    }

                    work_id += total_pp_warps;
                }

                // // --- Timing: after reduction (warp 0 perspective) ---
                if constexpr (TimeProfiling) {
                    if (threadIdx.x == 0) {
                        t2 = __builtin_amdgcn_s_memrealtime();
                    }
                }

                // --- Timing: store results ---
                if constexpr (TimeProfiling) {
                    if (threadIdx.x == 0) {
                        const uint32_t buf_idx =
                            (pp_wg_idx * args.num_chunks + chunk_idx) * 2;
                        args.timing_buf[buf_idx + 0] = t1 - t0; // wait
                        args.timing_buf[buf_idx + 1] = t2 - t1; // reduce & send
                    }
                }

                // // Fence this warp's stores before issuing puts
                // __threadfence();

                // // --- Timing: after threadfence (warp 0 perspective) ---
                // if (args.timing_buf && threadIdx.x == 0)
                //     t3 = __builtin_amdgcn_s_memrealtime();

                // // --- Per-row puts: each warp puts the rows it reduced ---
                // if (args.num_pes > 1) {
                //     work_id = global_warp;
                //     while (work_id < args.num_post_work) {
                //         const int32_t lane_token_idx =
                //             args.token_map[work_id * warp_size + lane_idx];
                //         const int32_t dst_row = __builtin_amdgcn_readlane(lane_token_idx, 0);
                //         EDataType* dst_ptr = chunk_base + dst_row * args.base.N;

                //         for (int i = 1; i < args.num_pes; i++) {
                //             const int pe = (args.my_pe + i) % args.num_pes;
                //             // try void *remote = rocshmem_ptr(dest, pe);
                //             rocshmem_putmem_nbi_wave(
                //                 dst_ptr,
                //                 dst_ptr,
                //                 row_bytes,
                //                 pe);
                //         }

                //         work_id += total_pp_warps;
                //     }
                // }

                // // --- Timing: after puts (thread 0 captures before final sync) ---
                // if (args.timing_buf && threadIdx.x == 0)
                //     t4 = __builtin_amdgcn_s_memrealtime();

                // // --- Timing: store results ---
                // if (args.timing_buf && threadIdx.x == 0) {
                //     const uint32_t buf_idx =
                //         (pp_wg_idx * args.num_chunks + chunk_idx) * 4;
                //     args.timing_buf[buf_idx + 0] = t1 - t0; // wait
                //     args.timing_buf[buf_idx + 1] = t2 - t1; // reduce
                //     args.timing_buf[buf_idx + 2] = t3 - t2; // threadfence
                //     args.timing_buf[buf_idx + 3] = t4 - t3; // put
                // }
            }

            return;
        }

        // -----------------------------------------------------------------
        // Compute WGs: custom persistent GEMM tile loop
        //
        // Key difference from UniversalGemmKernel::operator() persistent:
        //   stride = num_compute_wgs  (NOT get_grid_size() which includes
        //            post-processing WGs)
        //   block_id starts at wg_id (0 .. num_compute_wgs-1)
        // -----------------------------------------------------------------
        const auto num_compute = ck_tile::amd_wave_read_first_lane(args.num_compute_wgs);
        const auto num_tiles   = ck_tile::amd_wave_read_first_lane(
            static_cast<uint32_t>(TilePartitioner::GridSize(args.base.M, args.base.N)));
        const auto num_work = ck_tile::amd_wave_read_first_lane(
            num_tiles * static_cast<uint32_t>(args.base.k_batch));

        // allocate LDS once — reused across all tiles this WG processes
        __shared__ char smem_ptr[BaseKernel::GetSmemSize()];

        auto block_id = wg_id; // start at own WG index

        while(block_id < num_work)
        {
            // --- tile coordinates ---
            const auto tile_idx =
                ck_tile::amd_wave_read_first_lane(block_id % num_tiles);
            const auto [iM, iN] =
                TilePartitioner{args.base.M, args.base.N}.GetOutputTileIndex(tile_idx);
            const ck_tile::index_t i_m =
                ck_tile::amd_wave_read_first_lane(iM * TilePartitioner::MPerBlock);
            const ck_tile::index_t i_n =
                ck_tile::amd_wave_read_first_lane(iN * TilePartitioner::NPerBlock);

            // --- SplitK offset ---
            const auto k_batch_idx =
                ck_tile::amd_wave_read_first_lane(block_id / num_tiles);
            const typename BaseKernel::SplitKBatchOffset splitk_batch_offset(
                args.base, k_batch_idx);

            // --- build per-tile pointers ---
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

            // Offset e_ptr to this PE's dedicated slice within the symmetric buffer.
            // Layout: dE[pe0 slice | pe1 slice | ... | pe(n-1) slice], each M*N elements.
            // const uint64_t pe_offset = 0;
            EDataType* e_ptr = static_cast<EDataType*>(args.base.e_ptr) + 0;

            // --- run GEMM pipeline + epilogue for this tile ---
            BaseKernel::RunGemm(
                as_ptr, bs_ptr, args.base.ds_ptr, e_ptr, smem_ptr,
                args.base, splitk_batch_offset, i_m, i_n);

            // --- publish chunk completion (lane 0 only) ---
            // s_barrier only (default template args = max counters, so s_waitcnt
            // is a no-op).  Syncs all warps so no warp races ahead to the next
            // tile while others are still in the epilogue.
            // NOTE: this does NOT drain global store pipes — __threadfence()
            // below is still required for cross-CU visibility.
            ck_tile::s_waitcnt_barrier();

            if(threadIdx.x == 0)
            {
                __threadfence(); // make E stores globally visible before counter

                const uint32_t tile_n = static_cast<uint32_t>(i_n) / TilePartitioner::NPerBlock;
                uint32_t chunk        = tile_n / args.tiles_n_per_chunk;
                if(chunk >= args.num_chunks)
                    chunk = args.num_chunks - 1;

                atomicAdd(args.chunk_barriers + chunk, 1u);
            }

            // --- stride by compute WGs only (NOT total grid) ---
            block_id += num_compute;
        }
    }
};

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
          bool TimeProfiling,
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

    // --- Partitioner (always N-chunked for all-reduce) ---
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
    // Persistent (wrapped with AllReduceGemmChunkedKernel for custom stride)
    using PersistentKernelBase =
        ck_tile::UniversalGemmKernel<TilePartitioner, PersistentPipeline, PersistentEpilogue>;
    using PersistentKernel = AllReduceGemmChunkedKernel<PersistentKernelBase, TimeProfiling>;

    // Non-persistent reference (plain UniversalGemmKernel — no wrapper)
    using NonPersistentKernel =
        ck_tile::UniversalGemmKernel<TilePartitioner, NonPersistentPipeline, NonPersistentEpilogue>;
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
//                                  M    N    K    Mw Nw Kw  Mxdl Nxdl Time   DblBuf V4   PadM  PadN  PadK  BPC
using Cfg0 = KernelConfig<         128, 128, 64,   2, 2, 1,  16,  16,  false, false, false>;
using Cfg1 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false, false>;
using Cfg2 = KernelConfig<         256, 256, 128,  2, 2, 1,  32,  32,  false, false, false>;
using Cfg3 = KernelConfig<         128, 128, 128,  2, 2, 1,  16,  16,  false, false, false>;
// --- occupancy + padding ---
using Cfg4 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false, false, false, false, false, 2>;
using Cfg5 = KernelConfig<         128, 128, 64,   2, 2, 1,  16,  16,  false, false, false, false, false, false, 2>;
using Cfg6 = KernelConfig<         128, 128, 128,  2, 2, 1,  16,  16,  false, false, false, false, false, false, 2>;
using Cfg7 = KernelConfig<         256, 256, 64,   2, 2, 1,  32,  32,  false, false, false, true,  true,  true,  1>;
// --- warp layout ---
using Cfg8 = KernelConfig<         128, 128, 64,   4, 1, 1,  32,  32,  false, false, false>;
using Cfg9 = KernelConfig<         128, 128, 64,   1, 4, 1,  32,  32,  false, false, false>;
// --- V4 pipeline (double-buffered LDS, ARegBRegCReg block GEMM) ---
//                                  M    N    K    Mw Nw Kw  Mxdl Nxdl DblBuf V4
using Cfg10 = KernelConfig<        256, 256, 64,   2, 2, 1,  32,  32,  false, true,  true>;  // 384 VGPRs - SPILLS
using Cfg11 = KernelConfig<        128, 128, 64,   2, 2, 1,  32,  32,  false, true,  true>;  // 128 VGPRs - fits
using Cfg12 = KernelConfig<        128, 128, 32,   2, 2, 1,  32,  32,  false, true,  true>;  //  96 VGPRs - fits
using Cfg13 = KernelConfig<        128, 128, 64,   2, 2, 1,  16,  16,  false, true,  true>;  // 128 VGPRs - fits (16x16 XDL)
using Cfg14 = KernelConfig<         64,  64, 64,   1, 1, 1,  32,  32,  false, true,  true>;  // 128 VGPRs - fits (small tile)

using Cfg0_profiling = KernelConfig<128, 128, 64,   2, 2, 1,  16,  16, true, false, false>;
using Cfg1_profiling = KernelConfig<256, 256, 64,   2, 2, 1,  32,  32, true, false, false>;


} // namespace

// Bind this MPI rank to its local GPU BEFORE rocshmem_init().
// Uses OMPI_COMM_WORLD_LOCAL_RANK env var (set by mpiexec) so we don't need
// MPI_Init() to have been called yet — avoids conflicts with rocSHMEM's
// internal dlopen-based MPI management.
void bind_gpu_from_env() {
    int local_rank = 0;
    int world_rank = 0;
    int world_size = 1;

    // Open MPI sets these before the process starts — no MPI_Init needed.
    if (const char* v = std::getenv("OMPI_COMM_WORLD_LOCAL_RANK"))
        local_rank = std::atoi(v);
    if (const char* v = std::getenv("OMPI_COMM_WORLD_RANK"))
        world_rank = std::atoi(v);
    if (const char* v = std::getenv("OMPI_COMM_WORLD_SIZE"))
        world_size = std::atoi(v);

    int num_devices = 0;
    hip_check(hipGetDeviceCount(&num_devices), "hipGetDeviceCount");
    const int device_id = local_rank % num_devices;
    hip_check(hipSetDevice(device_id), "hipSetDevice");

    std::cout << "[rank " << world_rank << "/" << world_size
              << "] local_rank=" << local_rank
              << " -> GPU " << device_id << "/" << num_devices << "\n";
}

int main(int argc, char** argv)
{
    // Bind this rank to its local GPU BEFORE rocshmem_init().
    // rocshmem_init() does NOT call hipSetDevice() — it uses whatever GPU is active.
    // We read the local rank from Open MPI env vars (no MPI_Init needed).
    bind_gpu_from_env();

    // Let rocSHMEM handle MPI_Init internally via dlopen("libmpi.so").
    // Do NOT call MPI_Init() beforehand — rocSHMEM's internal library_init()
    // always calls mpilib_dl_init() which conflicts with statically-linked MPI.
    rocshmem_init();

    int my_pe = rocshmem_my_pe();
    int n_pes = rocshmem_n_pes();

    std::cout << "[PE " << my_pe << "/" << n_pes << "] rocSHMEM initialized\n";

    ck_tile::index_t M = 4096;
    ck_tile::index_t N = 4096;
    ck_tile::index_t K = 4096;
    int warmup         = 50;
    int repeat         = 100;
    bool verify        = true;
    bool flush_cache   = false;
    bool compare_non_persistent = true; // also run the non-persistent kernel for comparison
    int config_id      = 0;    // kernel configuration (0-14)
    uint32_t num_chunks       = 1; // number of N-chunks for all-reduce signalling
    uint32_t num_compute_wgs  = 0; // 0 = auto (all WGs are compute, from MaxOccupancyGridSize)
    uint32_t tokens_per_reduction = 0; // 0 = disabled; >0 = rows per reduction group
                                       // (num_post_work = M / tokens_per_reduction)
    bool enable_timing = false;        // --timing 1: capture per-PP-WG per-chunk timings

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
        else if(std::strcmp(key, "--num_chunks") == 0)
            num_chunks = static_cast<uint32_t>(parse_long(need_value("--num_chunks"), "--num_chunks"));
        else if(std::strcmp(key, "--num_compute_wgs") == 0)
            num_compute_wgs = static_cast<uint32_t>(parse_long(need_value("--num_compute_wgs"), "--num_compute_wgs"));
        else if(std::strcmp(key, "--tokens_per_reduction") == 0)
            tokens_per_reduction = static_cast<uint32_t>(parse_long(need_value("--tokens_per_reduction"), "--tokens_per_reduction"));
        else if(std::strcmp(key, "--timing") == 0)
            enable_timing = (parse_long(need_value("--timing"), "--timing") != 0);
        else if(std::strcmp(key, "--config") == 0)
            config_id = static_cast<int>(parse_long(need_value("--config"), "--config"));
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage:\n"
                      << "  " << argv[0]
                      << " [--m M] [--n N] [--k K] [--warmup W] [--repeat R]\n"
                      << "    [--verify 0|1] [--flush_cache 0|1] [--compare 0|1]\n"
                      << "    [--num_chunks N]\n"
                      << "    [--num_compute_wgs N] [--tokens_per_reduction T]\n"
                      << "    [--config 0-14]\n"
                      << "\n"
                      << "  --compare    1    (default) also run the non-persistent reference kernel\n"
                      << "  --num_chunks 1    (default) number of N-chunks for all-reduce signalling\n"
                      << "                    (tiles_n must be divisible by num_chunks)\n"
                      << "  --num_compute_wgs 0 (default) WGs dedicated to GEMM compute.\n"
                      << "                    0 = auto (all launched WGs are compute).\n"
                      << "                    When >0, total grid = num_compute_wgs + post-processing WGs.\n"
                      << "                    Remaining WGs (grid_size - num_compute_wgs) do post-processing.\n"
                      << "  --tokens_per_reduction 0 (default=disabled) rows per reduction group.\n"
                      << "                    Must be <= warp_size (64).\n"
                      << "                    Creates num_post_work = ceil(M / tokens_per_reduction) groups.\n"
                      << "                    Each group reduces up to `tokens_per_reduction` rows spaced\n"
                      << "                    stride apart; tail groups may have fewer rows (padded with -1).\n"
                      << "  --timing     0    (default) enable per-PP-WG per-chunk timing\n"
                      << "                    Prints wait/reduce/fence/put/sync durations.\n"
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
    hip_check(hipMalloc(&dA, sizeof(ADataType) * size_a), "hipMalloc(A)");
    hip_check(hipMalloc(&dB, sizeof(BDataType) * size_b), "hipMalloc(B)");

    // Allocate output buffer on the rocSHMEM symmetric heap.
    // Size is n_pes × size_e so that each PE writes to its own dedicated
    // slice at offset  my_pe * size_e  — no cross-PE write conflicts.
    const std::size_t size_e_total = static_cast<std::size_t>(n_pes) * size_e;
    // EDataType* dE = static_cast<EDataType*>(
    //     rocshmem_malloc(sizeof(EDataType) * size_e_total));
    EDataType* dE = nullptr;
    hip_check(hipMalloc(&dE, sizeof(EDataType) * size_e), "hipMalloc(E)");

    if(!dE)
    {
        std::cerr << "rocshmem_malloc failed for output buffer E ("
                  << (sizeof(EDataType) * size_e_total) << " bytes, "
                  << n_pes << " PEs × " << size_e << " elements)\n";
        std::exit(1);
    }

    hip_check(
        hipMemcpy(dA, hA.data(), sizeof(ADataType) * size_a, hipMemcpyHostToDevice), "hipMemcpy(A)");
    hip_check(
        hipMemcpy(dB, hB.data(), sizeof(BDataType) * size_b, hipMemcpyHostToDevice), "hipMemcpy(B)");
    hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E)");

    // Allocate chunk_barriers on device (one uint32_t counter per chunk, zeroed).
    uint32_t* d_chunk_barriers = nullptr;
    hip_check(hipMalloc(&d_chunk_barriers, sizeof(uint32_t) * num_chunks), "hipMalloc(chunk_barriers)");
    hip_check(hipMemset(d_chunk_barriers, 0, sizeof(uint32_t) * num_chunks), "hipMemset(chunk_barriers)");

    // -----------------------------------------------------------------------
    // Build token_map for post-processing row-reduction.
    //
    // Layout: [num_post_work × warp_size] of int32_t.
    //   num_post_work = M / tokens_per_reduction  (number of reduction groups)
    //   Each group has `tokens_per_reduction` valid row indices followed by
    //   (warp_size - tokens_per_reduction) sentinels (-1).
    //
    // Pattern (M=4096, tokens_per_reduction=32 → stride=128, num_post_work=128):
    //   group  0: [   0, 128, 256, …, 3968, -1, -1, …, -1]   (64 entries)
    //   group  1: [   1, 129, 257, …, 3969, -1, -1, …, -1]
    //   …
    //   group 127: [ 127, 255, 383, …, 4095, -1, -1, …, -1]
    // -----------------------------------------------------------------------
    static constexpr int kWarpSize = 64; // AMD warp size
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
        // stride = number of reduction groups = ceil(M / tokens_per_reduction)
        const uint32_t stride = (Mu + tokens_per_reduction - 1) / tokens_per_reduction;
        num_post_work         = stride; // one reduction group per stride offset

        const std::size_t map_elems = static_cast<std::size_t>(num_post_work) * kWarpSize;
        std::vector<int32_t> h_token_map(map_elems, -1); // fill with sentinel

        for(uint32_t g = 0; g < num_post_work; g++)
        {
            for(uint32_t t = 0; t < tokens_per_reduction; t++)
            {
                // row = g + t * stride  →  e.g. g=0: 0, 128, 256, …
                // Tail groups may have fewer valid rows when M % tokens_per_reduction != 0
                const uint32_t row = g + t * stride;
                if(row < Mu)
                {
                    h_token_map[static_cast<std::size_t>(g) * kWarpSize + t] =
                        static_cast<int32_t>(row);
                }
                // else: stays -1 (sentinel) — warp will simply reduce fewer rows
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
    // Allocate rocshmem symmetric buffer for AllGather.
    //
    // Each PE allocates  n_pes × num_post_work × N  elements on the symmetric
    // heap.  After reduction, PE `p` writes its reduced rows into slice
    //   [p * num_post_work * N .. (p+1) * num_post_work * N)
    // and then AllGather distributes every PE's slice to all others.
    //
    // We resolve GPU-visible pointers to every PE's buffer with rocshmem_ptr()
    // and pass the pointer array to the kernel so it can issue direct puts.
    // -----------------------------------------------------------------------
    EDataType*  d_ag_buf         = nullptr;   // local symmetric allocation
    EDataType** d_ag_remote_ptrs = nullptr;   // device array of per-PE pointers

    if(num_post_work > 0)
    {
        const std::size_t ag_elems_per_pe =
            static_cast<std::size_t>(num_post_work) * static_cast<std::size_t>(N);
        const std::size_t ag_total_elems  = static_cast<std::size_t>(n_pes) * ag_elems_per_pe;
        const std::size_t ag_total_bytes  = ag_total_elems * sizeof(EDataType);

        d_ag_buf = static_cast<EDataType*>(rocshmem_malloc(ag_total_bytes));
        if(!d_ag_buf)
        {
            std::cerr << "rocshmem_malloc failed for AllGather buffer ("
                      << ag_total_bytes << " bytes, "
                      << n_pes << " PEs × " << num_post_work << " × " << N << " elements)\n";
            std::exit(1);
        }
        hip_check(hipMemset(d_ag_buf, 0, ag_total_bytes), "hipMemset(ag_buf)");

        // Resolve GPU-visible pointers to each PE's symmetric buffer.
        std::vector<EDataType*> h_ag_remote_ptrs(n_pes);
        for(int pe = 0; pe < n_pes; pe++)
        {
            h_ag_remote_ptrs[pe] = static_cast<EDataType*>(rocshmem_ptr(d_ag_buf, pe));
            if(!h_ag_remote_ptrs[pe])
            {
                std::cerr << "rocshmem_ptr failed for PE " << pe << "\n";
                std::exit(1);
            }
        }

        hip_check(hipMalloc(&d_ag_remote_ptrs, sizeof(EDataType*) * n_pes),
                  "hipMalloc(ag_remote_ptrs)");
        hip_check(hipMemcpy(d_ag_remote_ptrs, h_ag_remote_ptrs.data(),
                            sizeof(EDataType*) * n_pes, hipMemcpyHostToDevice),
                  "hipMemcpy(ag_remote_ptrs)");

        std::cout << "AllGather buffer: " << ag_total_bytes << " bytes ("
                  << n_pes << " PEs × " << num_post_work << " × " << N
                  << " × " << sizeof(EDataType) << "B)\n";
    }

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
        static_cast<void*>(dE),  // start of symmetric-heap buffer (all PE slices)
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
                             auto block_per_cu_tag) -> int {
        using PersistKernel    = typename decltype(persistent_kernel_tag)::type;
        using NonPersistKernel = typename decltype(non_persistent_kernel_tag)::type;
        constexpr int kBlockPerCu = decltype(block_per_cu_tag)::value;

        // --- Persistent kernel launch ---
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

        // Patch all-reduce fields: chunk_barriers pointer, chunk count, compute WGs
        persistent_kargs.chunk_barriers  = d_chunk_barriers;
        persistent_kargs.num_chunks      = num_chunks;
        {
            // Recompute tiles_n_per_chunk to match the actual num_chunks
            using TP = typename PersistKernel::TilePartitioner;
            const uint32_t tiles_n = (static_cast<uint32_t>(N) + TP::NPerBlock - 1) / TP::NPerBlock;
            persistent_kargs.tiles_n_per_chunk = (tiles_n + num_chunks - 1) / num_chunks;
        }

        if (num_compute_wgs <= 0) {
            std::cerr << "ERROR: num_compute_wgs <= 0\n";
            return 1;
        }
        persistent_kargs.num_compute_wgs = num_compute_wgs;
        
        if (persistent_grids.x <= num_compute_wgs) {
            std::cerr << "ERROR: persistent_grids.x <= num_compute_wgs\n";
            return 1;
        }
        persistent_kargs.num_post_processing_wgs = persistent_grids.x - num_compute_wgs;

        if(tokens_per_reduction == 0)
        {
            std::cerr << "WARNING: tokens_per_reduction == 0, setting num_post_processing_wgs = 0\n";
            persistent_kargs.num_post_processing_wgs = 0;
            persistent_kargs.num_compute_wgs = persistent_grids.x;
        }

        // Patch token_map, num_post_work, and AllGather remote pointers
        persistent_kargs.token_map        = d_token_map;
        persistent_kargs.num_post_work    = num_post_work;
        persistent_kargs.ag_remote_ptrs   = d_ag_remote_ptrs;

        // Patch rocSHMEM team and PE info for cross-GPU broadcast
        // persistent_kargs.rocshmem_team = ROCSHMEM_TEAM_WORLD;
        persistent_kargs.num_pes       = n_pes;
        persistent_kargs.my_pe         = my_pe;

        // Allocate timing buffer if enabled
        uint64_t* d_timing_buf_local = nullptr;
        std::size_t timing_buf_elems = 0;
        if constexpr (PersistKernel::TimeProfiling) {
            timing_buf_elems = static_cast<std::size_t>(persistent_kargs.num_post_processing_wgs) * num_chunks * 2;
            hip_check(hipMalloc(&d_timing_buf_local,
                                sizeof(uint64_t) * timing_buf_elems),
                      "hipMalloc(timing_buf)");
            hip_check(hipMemset(d_timing_buf_local, 0,
                                sizeof(uint64_t) * timing_buf_elems),
                      "hipMemset(timing_buf)");
            persistent_kargs.timing_buf = d_timing_buf_local;
        }
        // persistent_kargs.timing_buf = d_timing_buf_local;

        // Warn about mismatched post-processing configuration
        if(persistent_kargs.num_post_processing_wgs > 0 && persistent_kargs.num_post_work == 0)
        {
            std::cerr << "WARNING: " << persistent_kargs.num_post_processing_wgs
                      << " post-processing WGs allocated but --tokens_per_reduction not set "
                         "(num_post_work=0). Post-processing WGs will idle.\n";
        }
        if(persistent_kargs.num_post_work > 0 && persistent_kargs.num_post_processing_wgs == 0)
        {
            std::cerr << "WARNING: --tokens_per_reduction set (num_post_work="
                      << persistent_kargs.num_post_work
                      << ") but no post-processing WGs allocated. "
                         "Reduction will not run. Set --num_compute_wgs < grid_size.\n";
        }

        std::cout << "Persistent kernel (n_chunked)"
                  << " grid_size=" << persistent_grids.x
                  << " block_size=" << persistent_blocks.x
                  << " num_compute_wgs=" << persistent_kargs.num_compute_wgs
                  << " num_post_processing_wgs=" << persistent_kargs.num_post_processing_wgs
                  << " num_chunks=" << num_chunks
                  << " num_post_work=" << num_post_work
                  << " tokens_per_reduction=" << tokens_per_reduction
                  << " kBlockPerCu=" << kBlockPerCu
                  << "\n";

        // Use launch_kernel_time_mask so that the preprocess callable resets
        // chunk_barriers before EACH timed iteration.  The preprocess time
        // is measured separately and subtracted from the reported kernel time.
        //
        // The post-callable enqueues rocshmem_barrier_all_on_stream() which
        // ensures all PEs have completed their puts AND all remote stores
        // targeting this PE have landed.  This makes the measurement
        // comparable to the reference (GEMM + reduce + NCCL AllGather)
        // where data is fully gathered on all devices when timing ends.
        //
        // rocshmem_quiet() inside the kernel drains this PE's outgoing puts;
        // the barrier_all_on_stream provides the cross-PE synchronization.
        const float persistent_ms = ck_tile::launch_kernel_time_mask(
            s,
            [&]() {
                (void)hipMemsetAsync(d_chunk_barriers, 0,
                                     sizeof(uint32_t) * num_chunks, s.stream_id_);
            },
            ck_tile::make_kernel<kBlockPerCu>(
                PersistKernel{}, persistent_grids, persistent_blocks, 0, persistent_kargs),
            [&](const ck_tile::stream_config& sc) {
                rocshmem_barrier_all_on_stream(sc.stream_id_);
            }
        );

        const double persistent_tflops =
            (persistent_ms > 0.0f)
                ? (flops / (static_cast<double>(persistent_ms) * 1.0e-3) / 1.0e12)
                : 0.0;

        // --- Save & print PP WG timing data (last iteration) ---
        if (d_timing_buf_local && timing_buf_elems > 0) {
            // Synchronize to ensure kernel + barrier have completed
            hip_check(hipStreamSynchronize(s.stream_id_), "hipStreamSync(timing)");

            std::vector<uint64_t> h_timing(timing_buf_elems);
            hip_check(hipMemcpy(h_timing.data(), d_timing_buf_local,
                                sizeof(uint64_t) * timing_buf_elems,
                                hipMemcpyDeviceToHost),
                      "hipMemcpy(timing)");

            // s_memrealtime on gfx9 runs at 100 MHz → 10 ns per tick
            constexpr double ns_per_tick = 10.0;

            const uint32_t n_pp = persistent_kargs.num_post_processing_wgs;

            // --- Save per-WG per-chunk detail to CSV file (one per rank) ---
            {
                const std::string fname =
                    "rank" + std::to_string(my_pe)
                    + "_m" + std::to_string(M)
                    + "_k" + std::to_string(K)
                    + "_n" + std::to_string(N)
                    + "_cwg" + std::to_string(persistent_kargs.num_compute_wgs)
                    + "_ppwg" + std::to_string(n_pp)
                    + "_chunk" + std::to_string(num_chunks)
                    + "_reduction" + std::to_string(tokens_per_reduction)
                    + "_perf.csv";
                std::ofstream ofs(fname);
                if (ofs.is_open()) {
                    ofs << "rank,pp_wg,chunk,wait_us,reduce_us,total_us\n";
                    for (uint32_t wg = 0; wg < n_pp; wg++) {
                        for (uint32_t c = 0; c < num_chunks; c++) {
                            const uint64_t* t =
                                h_timing.data() +
                                (static_cast<std::size_t>(wg) * num_chunks + c) * 2;
                            double wait_us   = t[0] * ns_per_tick / 1000.0;
                            double reduce_us = t[1] * ns_per_tick / 1000.0;
                            double total_us  = wait_us + reduce_us;
                            char buf[256];
                            std::snprintf(buf, sizeof(buf),
                                "%d,%u,%u,%.2f,%.2f,%.2f\n",
                                my_pe, wg, c, wait_us, reduce_us, total_us);
                            ofs << buf;
                        }
                    }
                    ofs.close();
                    std::cout << "[PE " << my_pe << "] timing saved to " << fname << "\n";
                } else {
                    std::cerr << "[PE " << my_pe << "] WARNING: could not open "
                              << fname << " for writing\n";
                }
            }

            // --- Per-chunk summary to stdout + CSV (all PEs) ---
            {
                const std::string summary_fname =
                    "rank" + std::to_string(my_pe)
                    + "_m" + std::to_string(M)
                    + "_k" + std::to_string(K)
                    + "_n" + std::to_string(N)
                    + "_cwg" + std::to_string(persistent_kargs.num_compute_wgs)
                    + "_ppwg" + std::to_string(n_pp)
                    + "_chunk" + std::to_string(num_chunks)
                    + "_reduction" + std::to_string(tokens_per_reduction)
                    + "_summary.csv";
                std::ofstream summary_ofs(summary_fname);
                if (summary_ofs.is_open()) {
                    summary_ofs << "rank,chunk,wait_avg_us,reduce_avg_us,wait_max_us,reduce_max_us,total_avg_us,total_max_us\n";
                }

                std::cout << "\n=== PP WG Timing Summary (PE " << my_pe << ") ===\n"
                          << "  chunk      wait_avg   reduce_avg "
                          << " wait_max   reduce_max  \n";
                for (uint32_t c = 0; c < num_chunks; c++) {
                    double sum[2] = {}, mx[2] = {};
                    for (uint32_t wg = 0; wg < n_pp; wg++) {
                        const uint64_t* t =
                            h_timing.data() + (static_cast<std::size_t>(wg) * num_chunks + c) * 2;
                        for (int m = 0; m < 2; m++) {
                            double us = t[m] * ns_per_tick / 1000.0;
                            sum[m] += us;
                            if (us > mx[m]) mx[m] = us;
                        }
                    }
                    double wait_avg   = sum[0] / n_pp;
                    double reduce_avg = sum[1] / n_pp;
                    double total_avg  = wait_avg + reduce_avg;
                    double total_max  = mx[0] + mx[1];

                    printf("  %5u  %10.2f  %10.2f "
                           "  %10.2f  %10.2f  \n",
                           c,
                           wait_avg, reduce_avg,
                           mx[0], mx[1]);

                    if (summary_ofs.is_open()) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "%d,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                            my_pe, c, wait_avg, reduce_avg,
                            mx[0], mx[1], total_avg, total_max);
                        summary_ofs << buf;
                    }
                }
                std::cout << "\n";

                if (summary_ofs.is_open()) {
                    summary_ofs.close();
                    std::cout << "[PE " << my_pe << "] timing summary saved to " << summary_fname << "\n";
                } else {
                    std::cerr << "[PE " << my_pe << "] WARNING: could not open "
                              << summary_fname << " for writing\n";
                }
            }

            hip_check(hipFree(d_timing_buf_local), "hipFree(timing_buf)");
            d_timing_buf_local = nullptr;
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
        std::cout << "UniversalGemm persistent kernel benchmark\n";
        std::cout << "M=" << M << " N=" << N << " K=" << K
                  << " partitioner=n_chunked"
                  << " config=" << config_id << "\n";
        std::cout << "warmup=" << warmup << " repeat=" << repeat
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
            hip_check(hipMemset(d_chunk_barriers, 0, sizeof(uint32_t) * num_chunks),
                      "hipMemset(barriers, verify)");
            if(d_ag_buf && num_post_work > 0)
            {
                const std::size_t ag_elems_per_pe =
                    static_cast<std::size_t>(num_post_work) * static_cast<std::size_t>(N);
                const std::size_t ag_total_bytes =
                    static_cast<std::size_t>(n_pes) * ag_elems_per_pe * sizeof(EDataType);
                hip_check(hipMemset(d_ag_buf, 0, ag_total_bytes),
                          "hipMemset(ag_buf, verify)");
            }

            // Sync all PEs before launching — ensures no PE's hipMemset(ag_buf)
            // clobbers another PE's remote writes from a previous/concurrent launch.
            rocshmem_barrier_all_on_stream(nullptr);
            hip_check(hipDeviceSynchronize(), "sync(verify pre-launch barrier)");

            // 3. Run fused kernel once (not timed) with barrier preprocess + post
            std::cout << "Running fused kernel..." << std::flush;
            {
                ck_tile::stream_config sv{};
                ck_tile::launch_kernel_time_mask(
                    sv,
                    [&]() {
                        (void)hipMemsetAsync(d_chunk_barriers, 0,
                                             sizeof(uint32_t) * num_chunks, sv.stream_id_);
                    },
                    ck_tile::make_kernel<kBlockPerCu>(
                        PersistKernel{}, persistent_grids, persistent_blocks,
                        0, persistent_kargs),
                    [&](const ck_tile::stream_config& sc) {
                        rocshmem_barrier_all_on_stream(sc.stream_id_);
                    });
            }
            hip_check(hipDeviceSynchronize(), "sync(verify)");
            std::cout << " done.\n";

            // 4. Validate GEMM output
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

            // 5. Validate AllGather output (in-kernel reduction + rocshmem puts)
            if(tokens_per_reduction > 0 && num_post_work > 0 && d_ag_buf)
            {
                // CPU reduction reference (fp32 accum of fp16-rounded GEMM values)
                const uint32_t Mu     = static_cast<uint32_t>(M);
                const uint32_t stride = (Mu + tokens_per_reduction - 1) / tokens_per_reduction;

                const std::size_t ag_elems_per_pe =
                    static_cast<std::size_t>(num_post_work) * static_cast<std::size_t>(N);

                std::vector<float> ref_reduced(ag_elems_per_pe, 0.0f);
                for(uint32_t g = 0; g < num_post_work; g++)
                {
                    for(uint32_t t = 0; t < tokens_per_reduction; t++)
                    {
                        const uint32_t row = g + t * stride;
                        if(row >= Mu) break;
                        for(ck_tile::index_t j = 0; j < N; j++)
                        {
                            ref_reduced[static_cast<std::size_t>(g) * N + j] +=
                                to_float(from_float<EDataType>(
                                    ref_E[static_cast<std::size_t>(row) * N + j]));
                        }
                    }
                }

                // Validate d_ag_buf: layout [n_pes × num_post_work × N]
                // Each PE's slice should match ref_reduced (all PEs use same A,B)
                const std::size_t ag_total_elems =
                    static_cast<std::size_t>(n_pes) * ag_elems_per_pe;
                std::vector<EDataType> hAG_gpu(ag_total_elems);
                hip_check(hipMemcpy(hAG_gpu.data(), d_ag_buf,
                                    sizeof(EDataType) * ag_total_elems,
                                    hipMemcpyDeviceToHost),
                          "hipMemcpy(ag_buf, verify)");

                double max_abs = 0.0, sum_sq = 0.0;
                double max_abs_gpu = 0.0, max_abs_ref = 0.0;
                double gpu_min = 1e30, gpu_max = -1e30;
                double ref_min = 1e30, ref_max = -1e30;
                for(int pe = 0; pe < n_pes; pe++)
                {
                    for(std::size_t i = 0; i < ag_elems_per_pe; i++)
                    {
                        const double gv = static_cast<double>(to_float(
                            hAG_gpu[static_cast<std::size_t>(pe) * ag_elems_per_pe + i]));
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
                          << "  rmse=" << std::sqrt(sum_sq / ag_total_elems)
                          << "  gpu_range=[" << gpu_min << ", " << gpu_max << "]"
                          << "  ref_range=[" << ref_min << ", " << ref_max << "]\n";
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
    if (enable_timing) {
        switch(config_id) {
        case 0:
            rc = dispatch_config(std::type_identity<Cfg0_profiling>{}, "0: 128x128x64, V3, 16x16 XDL + Timings");
            break;
        case 1:
            rc = dispatch_config(std::type_identity<Cfg1_profiling>{}, "1: 256x256x64, V3, 32x32 XDL + Timings");
            break;
        default:
            std::cerr << "Invalid --config for profiling" << config_id << " (valid: 0-1)\n";
            return 2;
        }
    } else {
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
    }

    // -----------------------------------------------------------------------
    // Cleanup.
    // -----------------------------------------------------------------------
    hip_check(hipFree(dA), "hipFree(A)");
    hip_check(hipFree(dB), "hipFree(B)");
    // dE was allocated on the rocSHMEM symmetric heap — use rocshmem_free
    hip_check(hipFree(dE), "hipFree(E)");
    // rocshmem_free(dE);
    hip_check(hipFree(d_chunk_barriers), "hipFree(chunk_barriers)");
    if(d_token_map)
        hip_check(hipFree(d_token_map), "hipFree(token_map)");
    if(d_ag_remote_ptrs)
        hip_check(hipFree(d_ag_remote_ptrs), "hipFree(ag_remote_ptrs)");
    if(d_ag_buf)
        rocshmem_free(d_ag_buf);

    // Finalize the rocSHMEM runtime
    rocshmem_finalize();

    return rc;
}

