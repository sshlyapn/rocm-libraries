// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <hip/hip_runtime.h>
#include <rccl/rccl.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <hip/hip_runtime_api.h>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
#include <mpi.h>
#endif

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

inline void hip_check(hipError_t e, const char* what, bool exit = true)
{
    if(e != hipSuccess)
    {
        std::cerr << "HIP error (" << what << "): " << hipGetErrorString(e) << "\n";
        if (exit) {
            std::exit(1);
        }
    }
}

// Forward decl (used by test helpers below).
inline void nccl_check(ncclResult_t r, const char* what);

// Create two streams with disjoint CU masks:
// - comm_stream runs on `reserve_comm_cus` CUs
// - compute_stream runs on all remaining CUs
//
// This is useful for the non-signal-memory fallback path where waiting is implemented as a GPU
// spin-kernel (otherwise the wait kernel may not get scheduled until the main GEMM finishes).
inline bool create_streams_with_reserved_cus(int reserve_comm_cus,
                                            hipStream_t* compute_stream,
                                            hipStream_t* comm_stream)
{
    if(reserve_comm_cus <= 0)
        return false;

    constexpr uint32_t kMaskDwords = 64; // supports up to 2048 CUs
    std::array<uint32_t, kMaskDwords> base_mask{};
    hipStream_t tmp{};
    if(hipStreamCreateWithFlags(&tmp, hipStreamNonBlocking) != hipSuccess)
        return false;
    if(hipExtStreamGetCUMask(tmp, kMaskDwords, base_mask.data()) != hipSuccess)
    {
        hip_check(hipStreamDestroy(tmp), "hipStreamDestroy(tmp)", false);
        return false;
    }
    hip_check(hipStreamDestroy(tmp), "hipStreamDestroy(tmp)", false);

    // Enumerate set bits (available CUs).
    std::array<int, static_cast<int>(kMaskDwords * 32)> cu_ids{};
    int num_cus = 0;
    for(uint32_t d = 0; d < kMaskDwords; ++d)
    {
        uint32_t v = base_mask[d];
        while(v)
        {
            const uint32_t lsb = v & (~v + 1u);
            const int bit      = __builtin_ctz(v);
            cu_ids[num_cus++]  = static_cast<int>(d * 32 + bit);
            v ^= lsb;
        }
    }

    if(num_cus <= reserve_comm_cus)
        return false;

    // Pick the last `reserve_comm_cus` available CUs for comm_stream.
    std::array<uint32_t, kMaskDwords> comm_mask{};
    std::array<uint32_t, kMaskDwords> compute_mask = base_mask;

    for(int i = num_cus - reserve_comm_cus; i < num_cus; ++i)
    {
        const int cu = cu_ids[i];
        const int d  = cu / 32;
        const int b  = cu % 32;
        comm_mask[static_cast<std::size_t>(d)] |= (1u << static_cast<uint32_t>(b));
        compute_mask[static_cast<std::size_t>(d)] &= ~(1u << static_cast<uint32_t>(b));
    }

    hip_check(hipExtStreamCreateWithCUMask(comm_stream, kMaskDwords, comm_mask.data()),
              "hipExtStreamCreateWithCUMask(comm_stream)");
    hip_check(hipExtStreamCreateWithCUMask(compute_stream, kMaskDwords, compute_mask.data()),
              "hipExtStreamCreateWithCUMask(compute_stream)");
    return true;
}

inline bool get_default_cu_mask(std::array<uint32_t, 64>& base_mask)
{
    hipStream_t tmp{};
    if(hipStreamCreateWithFlags(&tmp, hipStreamNonBlocking) != hipSuccess)
        return false;
    const hipError_t err = hipExtStreamGetCUMask(tmp, /*cuMaskSize=*/64, base_mask.data());
    hip_check(hipStreamDestroy(tmp), "hipStreamDestroy(tmp)", false);
    return err == hipSuccess;
}

inline std::vector<int> get_enabled_cu_pair_even_ids(const std::array<uint32_t, 64>& base_mask)
{
    // Many ROCr queue affinity paths require enabling CUs in (even, odd) contiguous pairs.
    // We build a list of even CU ids where both CU and CU+1 are enabled in the base mask.
    std::vector<int> pairs;
    pairs.reserve(1024);

    auto test_bit = [&](int cu) -> bool {
        const int d = cu / 32;
        const int b = cu % 32;
        if(d < 0 || d >= 64)
            return false;
        return (base_mask[static_cast<std::size_t>(d)] & (1u << static_cast<uint32_t>(b))) != 0;
    };

    for(int cu = 0; cu + 1 < 64 * 32; cu += 2)
    {
        if(test_bit(cu) && test_bit(cu + 1))
            pairs.push_back(cu);
    }
    return pairs;
}

inline bool create_stream_with_first_cu_pairs(int num_pairs,
                                              const std::vector<int>& pair_even_ids,
                                              hipStream_t* stream)
{
    if(num_pairs <= 0)
        return false;
    if(static_cast<std::size_t>(num_pairs) > pair_even_ids.size())
        return false;

    std::array<uint32_t, 64> mask{};
    int max_cu = -1;
    for(int i = 0; i < num_pairs; ++i)
    {
        const int cu0 = pair_even_ids[static_cast<std::size_t>(i)];
        const int cu1 = cu0 + 1;
        for(int cu : {cu0, cu1})
        {
            const int d = cu / 32;
            const int b = cu % 32;
            mask[static_cast<std::size_t>(d)] |= (1u << static_cast<uint32_t>(b));
            if(cu > max_cu)
                max_cu = cu;
        }
    }

    // Pass the minimal required cuMaskSize (number of uint32_t words), instead of always 64.
    // Some ROCr/HIP stacks are sensitive to oversized masks (even when trailing words are 0).
    const uint32_t mask_size = (max_cu >= 0) ? (static_cast<uint32_t>(max_cu / 32) + 1u) : 1u;
    return hipExtStreamCreateWithCUMask(stream, mask_size, mask.data()) == hipSuccess;
}

inline int rccl_cu_sweep_test(int world_rank,
                              int world_size,
                              int warmup,
                              int repeat,
                              void* d_buf,
                              std::size_t count_elems,
                              std::size_t bytes_per_elem)
{
    auto getenv_i = [](const char* key, int def) -> int {
        if(const char* v = std::getenv(key); v && *v)
        {
            char* end = nullptr;
            const long x = std::strtol(v, &end, 10);
            if(end != v && (!end || *end == '\0'))
                return static_cast<int>(x);
        }
        return def;
    };

    // Optional runtime controls (no CLI changes):
    // - CK_TILE_RCCL_TEST_MIN_CUS:  skip requests below this CU count (default: 16)
    // - CK_TILE_RCCL_TEST_MAX_CUS:  skip requests above this CU count (default: 0 = no cap)
    // - CK_TILE_RCCL_TIMEOUT_MS:    watchdog timeout for warmup/repeat completion (default: 0 = disabled)
    // - CK_TILE_CUMASK_SERIALIZE:   if 1, serialize hipExtStreamCreateWithCUMask across ranks (default: 0)
    // - CK_TILE_CUMASK_STAGGER_MS:  if >0, sleep rank*ms before creating CU-masked streams (default: 0)
    // - CK_TILE_RCCL_VERBOSE:       if 1, print progress from every rank (default: 0)
    // NOTE: very small CU masks (<= 8 CUs) can make RCCL collectives non-progressing on some stacks
    // because not all required CTAs/channels can be scheduled concurrently. Default to 16 for
    // repeatability; users can override to 1 if they explicitly want to explore tiny masks.
    const int min_req_cus = std::max(1, getenv_i("CK_TILE_RCCL_TEST_MIN_CUS", 16));
    const int max_req_cus = std::max(0, getenv_i("CK_TILE_RCCL_TEST_MAX_CUS", 0));
    const int timeout_ms  = std::max(0, getenv_i("CK_TILE_RCCL_TIMEOUT_MS", 0));
    const int serialize_cumask = getenv_i("CK_TILE_CUMASK_SERIALIZE", 0);
    const int stagger_ms       = std::max(0, getenv_i("CK_TILE_CUMASK_STAGGER_MS", 0));
    const int verbose_all_ranks = getenv_i("CK_TILE_RCCL_VERBOSE", 0);

    // Init communicator once.
    ncclUniqueId uid{};
    if(world_rank == 0)
    {
        nccl_check(ncclGetUniqueId(&uid), "ncclGetUniqueId");
    }
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    MPI_Bcast(&uid, static_cast<int>(sizeof(uid)), MPI_BYTE, 0, MPI_COMM_WORLD);
#endif

    ncclComm_t comm{};
    nccl_check(ncclCommInitRank(&comm, /*nranks=*/world_size, uid, /*rank=*/world_rank),
               "ncclCommInitRank");

    std::array<uint32_t, 64> base_mask{};
    if(!get_default_cu_mask(base_mask))
    {
        if(world_rank == 0)
            std::cerr << "Failed to query default CU mask via hipExtStreamGetCUMask\n";
        nccl_check(ncclCommDestroy(comm), "ncclCommDestroy");
        return 1;
    }

    const auto pair_even_ids = get_enabled_cu_pair_even_ids(base_mask);
    const int total_pairs    = static_cast<int>(pair_even_ids.size());

    // IMPORTANT: different ranks/devices can observe different default CU masks (e.g. partitioning,
    // scheduling constraints, or platform quirks). If ranks iterate different CU counts, they will
    // execute a different number/sequence of collectives and RCCL will hang.
    //
    // To prevent that, compute the global min/max available_pairs across ranks and only sweep up
    // to the global minimum so every rank executes the exact same loop.
    int min_pairs = total_pairs;
    int max_pairs = total_pairs;
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    MPI_Allreduce(MPI_IN_PLACE, &min_pairs, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &max_pairs, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
#endif
    const int sweep_pairs = min_pairs;
    const int sweep_cus   = sweep_pairs * 2;

    if(world_rank == 0)
    {
        std::cout << "[test_rccl] available_pairs(local_rank0)=" << total_pairs
                  << " available_pairs(min)=" << min_pairs
                  << " available_pairs(max)=" << max_pairs
                  << " sweep_cus=" << sweep_cus
                  << " count_elems=" << count_elems << " bytes=" << (count_elems * bytes_per_elem)
                  << " world_size=" << world_size << "\n";
        if(min_req_cus > 1)
            std::cout << "[test_rccl] CK_TILE_RCCL_TEST_MIN_CUS=" << min_req_cus << "\n";
        if(max_req_cus > 0)
            std::cout << "[test_rccl] CK_TILE_RCCL_TEST_MAX_CUS=" << max_req_cus << "\n";
        if(timeout_ms > 0)
            std::cout << "[test_rccl] CK_TILE_RCCL_TIMEOUT_MS=" << timeout_ms << "\n";
        if(serialize_cumask)
            std::cout << "[test_rccl] CK_TILE_CUMASK_SERIALIZE=1\n";
        if(stagger_ms > 0)
            std::cout << "[test_rccl] CK_TILE_CUMASK_STAGGER_MS=" << stagger_ms << "\n";
        if(verbose_all_ranks)
            std::cout << "[test_rccl] CK_TILE_RCCL_VERBOSE=1\n";
    }

    // Bootstrap one AllReduce on a normal stream to force communicator setup/progress engine init
    // outside of CU-masked streams. This helps diagnose/avoid hangs specific to CU masking.
    {
        hipStream_t boot{};
        hip_check(hipStreamCreateWithFlags(&boot, hipStreamNonBlocking), "hipStreamCreate(boot)");
        nccl_check(ncclAllReduce(d_buf,
                                 d_buf,
                                 count_elems,
                                 ncclHalf,
                                 ncclSum,
                                 comm,
                                 boot),
                   "ncclAllReduce(test_bootstrap)");
        hip_check(hipStreamSynchronize(boot), "hipStreamSynchronize(test_bootstrap)");
        hip_check(hipStreamDestroy(boot), "hipStreamDestroy(test_bootstrap)");
        if(world_rank == 0)
            std::cout << "[test_rccl] bootstrap AllReduce done\n";
    }

    // Sweep CU counts (requested). Actual enabled CUs will be rounded up to an even number
    // because we enable CU pairs.
    // NOTE: req list must be identical across ranks. Do NOT append rank-local total_cus.
    std::vector<int> req_cus = {1,  2,  4,  8,  16,  32,  64,  96,  128, 160, 192, 224, 256};
    req_cus.push_back(sweep_cus);
    std::sort(req_cus.begin(), req_cus.end());
    req_cus.erase(std::unique(req_cus.begin(), req_cus.end()), req_cus.end());

    const double busbw_factor = (world_size > 1) ? (2.0 * (static_cast<double>(world_size) - 1.0) /
                                                    static_cast<double>(world_size))
                                                 : 0.0;

    for(int req : req_cus)
    {
        if(req < min_req_cus)
            continue;
        if(max_req_cus > 0 && req > max_req_cus)
            continue;

        const int pairs_needed = (req + 1) / 2;
        const int actual_cus   = pairs_needed * 2;
        if(pairs_needed <= 0 || pairs_needed > sweep_pairs)
            continue;

        if(world_rank == 0)
        {
            std::cout << "[test_rccl] starting req_cus=" << req << " (pairs_needed=" << pairs_needed
                      << " actual_cus=" << actual_cus << ")\n"
                      << std::flush;
        }

        hipStream_t s{};

        // Some ROCr/HIP stacks show sporadic hangs when multiple processes create/destroy
        // CU-masked streams concurrently. Optionally serialize creation across ranks to
        // reduce contention (creation time is not part of the timed region anyway).
        bool local_ok = false;
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
        if(serialize_cumask)
        {
            // IMPORTANT: all ranks must enter the same number of barriers.
            // Each rank creates its CU-masked stream on its "turn" (rank order).
            for(int r = 0; r < world_size; ++r)
            {
                MPI_Barrier(MPI_COMM_WORLD);
                if(world_rank == r)
                {
                    if(stagger_ms > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms * world_rank));
                    if(world_rank == 0 || verbose_all_ranks)
                        std::cout << "[test_rccl] rank " << world_rank
                                  << " creating CU-masked stream...\n"
                                  << std::flush;
                    local_ok = create_stream_with_first_cu_pairs(pairs_needed, pair_even_ids, &s);
                }
                MPI_Barrier(MPI_COMM_WORLD);
            }
        }
        else
#endif
        {
            if(stagger_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms * world_rank));
            if(world_rank == 0 || verbose_all_ranks)
                std::cout << "[test_rccl] rank " << world_rank << " creating CU-masked stream...\n"
                          << std::flush;
            local_ok = create_stream_with_first_cu_pairs(pairs_needed, pair_even_ids, &s);
        }

        int ok = local_ok ? 1 : 0;
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
        MPI_Allreduce(MPI_IN_PLACE, &ok, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
#endif
        if(!ok)
        {
            if(local_ok)
                hip_check(hipStreamDestroy(s), "hipStreamDestroy(test_stream)", false);
            if(world_rank == 0)
                std::cerr << "[test_rccl] failed to create CU-masked stream for req_cus=" << req
                          << " (pairs_needed=" << pairs_needed << ")\n";
            break;
        }

        if(world_rank == 0 || verbose_all_ranks)
            std::cout << "[test_rccl] rank " << world_rank << " CU-masked stream created\n"
                      << std::flush;

        hipEvent_t start{}, stop{};
        hip_check(hipEventCreateWithFlags(&start, hipEventDefault), "hipEventCreate(test_start)");
        hip_check(hipEventCreateWithFlags(&stop, hipEventDefault), "hipEventCreate(test_stop)");

        // Warmup.
        if(world_rank == 0)
            std::cout << "[test_rccl] warmup enqueue...\n" << std::flush;
        for(int i = 0; i < warmup; ++i)
        {
            nccl_check(ncclAllReduce(d_buf,
                                     d_buf,
                                     count_elems,
                                     ncclHalf,
                                     ncclSum,
                                     comm,
                                     s),
                       "ncclAllReduce(test_warmup)");
        }
        if(timeout_ms > 0)
        {
            hipEvent_t warm_evt{};
            hip_check(hipEventCreateWithFlags(&warm_evt, hipEventDisableTiming),
                      "hipEventCreate(test_warm_evt)");
            hip_check(hipEventRecord(warm_evt, s), "hipEventRecord(test_warm_evt)");

            const auto t0 = std::chrono::steady_clock::now();
            auto next_report = t0;
            while(true)
            {
                const hipError_t q = hipEventQuery(warm_evt);
                if(q == hipSuccess)
                    break;
                if(q != hipErrorNotReady)
                    hip_check(q, "hipEventQuery(test_warm_evt)");

                const auto now = std::chrono::steady_clock::now();
                if(world_rank == 0 && now >= next_report)
                {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                    std::cout << "[test_rccl] warmup not done yet, waited " << ms << " ms\n" << std::flush;
                    next_report = now + std::chrono::seconds(1);
                }
                const auto waited_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                if(waited_ms >= timeout_ms)
                {
                    if(world_rank == 0)
                        std::cerr << "[test_rccl] TIMEOUT during warmup (req_cus=" << req
                                  << ", actual_cus=" << actual_cus << ")\n";
                    nccl_check(ncclCommAbort(comm), "ncclCommAbort(timeout)");
                    std::exit(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            hip_check(hipEventDestroy(warm_evt), "hipEventDestroy(test_warm_evt)");
        }
        else
        {
            hip_check(hipStreamSynchronize(s), "hipStreamSynchronize(test_warmup)");
        }
        if(world_rank == 0)
            std::cout << "[test_rccl] warmup done\n" << std::flush;

        // Timed repeats.
        if(world_rank == 0)
            std::cout << "[test_rccl] repeat enqueue...\n" << std::flush;
        hip_check(hipEventRecord(start, s), "hipEventRecord(test_start)");
        for(int i = 0; i < repeat; ++i)
        {
            nccl_check(ncclAllReduce(d_buf,
                                     d_buf,
                                     count_elems,
                                     ncclHalf,
                                     ncclSum,
                                     comm,
                                     s),
                       "ncclAllReduce(test_repeat)");
        }
        hip_check(hipEventRecord(stop, s), "hipEventRecord(test_stop)");
        if(timeout_ms > 0)
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto next_report = t0;
            while(true)
            {
                const hipError_t q = hipEventQuery(stop);
                if(q == hipSuccess)
                    break;
                if(q != hipErrorNotReady)
                    hip_check(q, "hipEventQuery(test_stop)");

                const auto now = std::chrono::steady_clock::now();
                if(world_rank == 0 && now >= next_report)
                {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                    std::cout << "[test_rccl] repeat not done yet, waited " << ms << " ms\n" << std::flush;
                    next_report = now + std::chrono::seconds(1);
                }
                const auto waited_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                if(waited_ms >= timeout_ms)
                {
                    if(world_rank == 0)
                        std::cerr << "[test_rccl] TIMEOUT during repeat (req_cus=" << req
                                  << ", actual_cus=" << actual_cus << ")\n";
                    nccl_check(ncclCommAbort(comm), "ncclCommAbort(timeout)");
                    std::exit(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else
        {
            hip_check(hipEventSynchronize(stop), "hipEventSynchronize(test_stop)");
        }
        if(world_rank == 0)
            std::cout << "[test_rccl] repeat done\n" << std::flush;

        float total_ms = 0.0f;
        hip_check(hipEventElapsedTime(&total_ms, start, stop), "hipEventElapsedTime(test)");
        const float avg_ms = (repeat > 0) ? (total_ms / static_cast<float>(repeat)) : 0.0f;

        const double bytes = static_cast<double>(count_elems) * static_cast<double>(bytes_per_elem);
        const double algbw = (avg_ms > 0.0f) ? (bytes / (static_cast<double>(avg_ms) * 1.0e-3) / 1.0e9)
                                             : 0.0;
        const double busbw = algbw * busbw_factor;

        if(world_rank == 0)
        {
            std::cout << "[test_rccl] req_cus=" << req << " actual_cus=" << actual_cus
                      << " avg_ms=" << avg_ms << " algbw_GBs=" << algbw << " busbw_GBs=" << busbw
                      << "\n";
        }

        hip_check(hipEventDestroy(stop), "hipEventDestroy(test_stop)");
        hip_check(hipEventDestroy(start), "hipEventDestroy(test_start)");
        hip_check(hipStreamDestroy(s), "hipStreamDestroy(test_stream)");
    }

    nccl_check(ncclCommDestroy(comm), "ncclCommDestroy");
    return 0;
}

inline void nccl_check(ncclResult_t r, const char* what)
{
    if(r != ncclSuccess)
    {
        std::cerr << "RCCL error (" << what << "): " << ncclGetErrorString(r) << "\n";
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
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if(end == s || (end && *end != '\0'))
    {
        std::cerr << "Failed to parse " << what << " from '" << s << "'\n";
        std::exit(2);
    }
    return v;
}

template <typename T>
constexpr T ceil_div(T a, T b)
{
    return (a + b - 1) / b;
}

template <typename BaseKernel_, typename TilePartitioner_>
struct UniversalGemmChunkedKernel
{
    using BaseKernel      = BaseKernel_;
    using TilePartitioner = TilePartitioner_;
    using BaseArgs        = typename BaseKernel::KernelArgs;

    // Keep KernelArgs trivially copyable (HIP kernel-parameter friendly):
    // do NOT add user-defined constructors/destructors here.
    struct KernelArgs
    {
        BaseArgs base;
        uint32_t* chunk_done;
        uint32_t num_chunks;
        uint32_t tiles_m_per_chunk;
    };

    static constexpr ck_tile::index_t kBlockSize = BaseKernel::kBlockSize;

    CK_TILE_HOST static KernelArgs
    MakeKernelArgs(const BaseArgs& base, uint32_t* chunk_done, uint32_t num_chunks)
    {
        const uint32_t tiles_m = ceil_div<uint32_t>(static_cast<uint32_t>(base.M),
                                                    static_cast<uint32_t>(TilePartitioner::MPerBlock));
        const uint32_t tiles_m_per_chunk = ceil_div<uint32_t>(tiles_m, num_chunks);
        return KernelArgs{base, chunk_done, num_chunks, tiles_m_per_chunk};
    }

    CK_TILE_DEVICE void operator()(KernelArgs args) const
    {
        // Run the original GEMM kernel (prolog + mainloop + epilogue).
        BaseKernel{}(args.base);

        // // After the epilogue stores are done, publish completion to the per-chunk counter.
        if(threadIdx.x == 0)
        {
            // Make sure global stores to E are visible before we publish the counter.
            __threadfence();

            const auto [i_m, i_n] = BaseKernel::GetTileCoordinates(args.base);
            (void)i_n;

            const uint32_t tile_m = static_cast<uint32_t>(i_m) / TilePartitioner::MPerBlock;
            uint32_t chunk        = tile_m / args.tiles_m_per_chunk;
            if(chunk >= args.num_chunks)
                chunk = args.num_chunks - 1;

            atomicAdd(args.chunk_done + chunk, 1u);
        }
    }
};

__global__ void wait_value_eq_kernel(uint32_t* ptr, uint32_t expected)
{
    if(threadIdx.x == 0)
    {
        // Use atomicAdd(0) as an atomic load (avoids cache-stale reads / compiler hoisting).
        // Wait for >= (not ==) to be robust if the counter ever overshoots due to bugs/changes.
        while(atomicAdd(ptr, 0u) < expected)
        {
            // Reduce memory-system pressure while spinning.
#if defined(__AMDGCN__)
            __builtin_amdgcn_s_sleep(1);
#endif
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
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

    using ADataType   = ck_tile::fp16_t;
    using BDataType   = ck_tile::fp16_t;
    using AccDataType = float;
    using EDataType   = ck_tile::fp16_t;

    // Standard row-major GEMM: C[M,N] = A[M,K] * B[K,N]
    using ALayout = ck_tile::tensor_layout::gemm::RowMajor;
    using BLayout = ck_tile::tensor_layout::gemm::RowMajor;
    using ELayout = ck_tile::tensor_layout::gemm::RowMajor;

    /* DeepSeekV3 shapes:
       Per GPU input: [T, 16384 / tp]
       Per GPU weights: [16384 / tp, 7168]
       Per GPU output: [T, 7168]
       Then ReduceAll sum
    */

    /* Qwen3 shapes:
       Per GPU input: [T, 8192 / tp]
       Per GPU weights: [8192 / tp, 4096]
       Per GPU output: [T, 4096]
       Then ReduceAll sum
     */

    // ck_tile::index_t T = 32768;
    // ck_tile::index_t in_features = 16384 / 8;
    // ck_tile::index_t out_features = 7168;

    ck_tile::index_t M = 4096;
    ck_tile::index_t N = 4096;
    ck_tile::index_t K = 4096;
    int warmup         = 50;
    int repeat         = 100;
    bool verify        = true;
    bool flush_cache   = false;
    uint32_t num_chunks = 8;
    bool rccl_per_chunk = false;
    int reserve_comm_cus = 0;
    bool run_both_stream_modes = false;
    bool test_rccl = false;

    // Minimal CLI (no ck_tile ArgParser dependency):
    //   --m <int> --n <int> --k <int> --warmup <int> --repeat <int>
    //   --verify <0|1> --chunks <4|8|16|32> --rccl_per_chunk <0|1> --reserve_comm_cus <0..>
    //   --run_both_stream_modes <0|1> (run default+current stream modes back-to-back)
    //   --test_rccl <0|1> (AllReduce-only CU sweep test; reuses M,N for buffer size)
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
        else if(std::strcmp(key, "--chunks") == 0)
            num_chunks = static_cast<uint32_t>(parse_long(need_value("--chunks"), "--chunks"));
        else if(std::strcmp(key, "--rccl_per_chunk") == 0)
            rccl_per_chunk = (parse_long(need_value("--rccl_per_chunk"), "--rccl_per_chunk") != 0);
        else if(std::strcmp(key, "--reserve_comm_cus") == 0)
            reserve_comm_cus = static_cast<int>(parse_long(need_value("--reserve_comm_cus"), "--reserve_comm_cus"));
        else if(std::strcmp(key, "--run_both_stream_modes") == 0)
            run_both_stream_modes =
                (parse_long(need_value("--run_both_stream_modes"), "--run_both_stream_modes") != 0);
        else if(std::strcmp(key, "--test_rccl") == 0)
            test_rccl = (parse_long(need_value("--test_rccl"), "--test_rccl") != 0);
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage:\n"
                      << "  " << argv[0]
                      << " [--m M] [--n N] [--k K] [--warmup W] [--repeat R]\n"
                      << "    [--verify 0|1] [--chunks 4|8] [--rccl_per_chunk 0|1]\n"
                      << "    [--reserve_comm_cus N]   (non-signal-memory fallback only; try N=2)\n"
                      << "    [--run_both_stream_modes 0|1] (run default+current stream modes)\n"
                      << "    [--test_rccl 0|1]        (AllReduce-only CU sweep)\n";
            return 0;
        }
    }

    if(!(num_chunks == 4 || num_chunks == 8 || num_chunks == 16 || num_chunks == 32))
    {
        std::cerr << "--chunks must be 4 or 8 or 16 or 32\n";
        return 2;
    }

    // Bind each process to a GPU on this node.
    // (For multi-node you typically also set NCCL_SOCKET_IFNAME / NCCL_IB_* etc, not shown here.)
    int num_devices = 0;
    hip_check(hipGetDeviceCount(&num_devices), "hipGetDeviceCount");
    if(num_devices <= 0)
    {
        std::cerr << "No HIP devices found\n";
        return 3;
    }
    const int device_id = local_rank % num_devices;
    hip_check(hipSetDevice(device_id), "hipSetDevice");
    if(world_rank == 0)
    {
        std::cout << "MPI world_size=" << world_size << " hip_devices=" << num_devices << "\n";
    }
    std::cout << "[rank " << world_rank << "] local_rank=" << local_rank << " device_id=" << device_id
              << "\n";

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

    using Epilogue  = ck_tile::CShuffleEpilogue<EpilogueProblem>;
    using BaseKernel = ck_tile::UniversalGemmKernel<TilePartitioner, Pipeline, Epilogue>;
    using ChunkedKernel = UniversalGemmChunkedKernel<BaseKernel, TilePartitioner>;

    const std::size_t size_a = static_cast<std::size_t>(M) * static_cast<std::size_t>(K);
    const std::size_t size_b = static_cast<std::size_t>(K) * static_cast<std::size_t>(N);
    const std::size_t size_e = static_cast<std::size_t>(M) * static_cast<std::size_t>(N);

    ADataType* dA = nullptr;
    BDataType* dB = nullptr;
    EDataType* dE = nullptr;
    hip_check(hipMalloc(&dE, sizeof(EDataType) * size_e), "hipMalloc(E)");

    hip_check(hipMemset(dE, 0, sizeof(EDataType) * size_e), "hipMemset(E)");

    if(test_rccl)
    {
        // Run AllReduce-only CU sweep test and exit.
        const int rc = rccl_cu_sweep_test(world_rank,
                                          world_size,
                                          warmup,
                                          repeat,
                                          static_cast<void*>(dE),
                                          /*count_elems=*/size_e,
                                          /*bytes_per_elem=*/sizeof(EDataType));
        hip_check(hipFree(dE), "hipFree(E)");
        return rc;
    }

    std::vector<ADataType> hA(size_a);
    std::vector<BDataType> hB(size_b);

    std::mt19937 rng(20260205u); // deterministic seed for reproducibility
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);

    for(std::size_t i = 0; i < size_a; ++i)
        hA[i] = from_float<ADataType>(dist(rng));
    for(std::size_t i = 0; i < size_b; ++i)
        hB[i] = from_float<BDataType>(dist(rng));

    hip_check(hipMalloc(&dA, sizeof(ADataType) * size_a), "hipMalloc(A)");
    hip_check(hipMalloc(&dB, sizeof(BDataType) * size_b), "hipMalloc(B)");

    hip_check(hipMemcpy(dA, hA.data(), sizeof(ADataType) * size_a, hipMemcpyHostToDevice),
              "hipMemcpy(A)");
    hip_check(hipMemcpy(dB, hB.data(), sizeof(BDataType) * size_b, hipMemcpyHostToDevice),
              "hipMemcpy(B)");

    // Per-chunk completion counters (signal memory enables hipStreamWaitValue32 on AMD).
    uint32_t* d_chunk_done = nullptr;
    const std::size_t chunk_done_bytes = sizeof(uint32_t) * static_cast<std::size_t>(num_chunks);
    bool chunk_done_is_signal_memory   = false;
    hipError_t alloc_err =
        hipExtMallocWithFlags(reinterpret_cast<void**>(&d_chunk_done), chunk_done_bytes, hipMallocSignalMemory);
    if(alloc_err != hipSuccess)
    {
        // hipExtMallocWithFlags() can fail on platforms without signal-memory support, and may
        // leave a sticky "last error" that would otherwise trip ck_tile's hipPeekAtLastError()
        // checks in launch_and_check(). Clear it explicitly since we handle the fallback.
        hip_check(hipGetLastError(), "hipGetLastError()", false);

        // Fallback: still works for polling, but hipStreamWaitValue32 may reject non-signal memory.
        hip_check(hipMalloc(reinterpret_cast<void**>(&d_chunk_done), chunk_done_bytes),
                  "hipMalloc(chunk_done)");
    }
    else
    {
        chunk_done_is_signal_memory = true;
    }
    hip_check(hipMemset(d_chunk_done, 0, chunk_done_bytes), "hipMemset(chunk_done)");

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

    const auto base_kargs = BaseKernel::MakeKernelArgs(host_args);

    if(!BaseKernel::IsSupportedArgument(base_kargs))
    {
        std::cerr << "Kernel does not support the provided arguments (alignment/padding/shape).\n";
        std::cerr << "Try setting M,N,K to multiples of the tile sizes, or enable padding in Traits.\n";
        hip_check(hipFree(d_chunk_done), "hipFree(chunk_done)");
        hip_check(hipFree(dA), "hipFree(A)");
        hip_check(hipFree(dB), "hipFree(B)");
        hip_check(hipFree(dE), "hipFree(E)");
        return 1;
    }

    const auto chunked_kargs = ChunkedKernel::MakeKernelArgs(base_kargs, d_chunk_done, num_chunks);

    const dim3 grids  = BaseKernel::GridSize(M, N, k_batch);
    const dim3 blocks = BaseKernel::BlockSize();

    // Time kernel execution (avg time in ms).
    ck_tile::stream_config s{};
    s.time_kernel_  = true;
    s.cold_niters_  = warmup;
    s.nrepeat_      = repeat;
    s.is_gpu_timer_ = true;
    s.flush_cache_  = flush_cache;

    float ave_ms = 0.0f;

    if(!rccl_per_chunk)
    {
        // Reset counters before every launch (warmup + repeat) so the counters represent a single
        // GEMM launch's completion state.
        ave_ms = ck_tile::launch_kernel(
            s,
            [&](const ck_tile::stream_config& sc) {
                hip_check(hipMemsetAsync(d_chunk_done, 0, chunk_done_bytes, sc.stream_id_),
                          "hipMemsetAsync(chunk_done)");
            },
            ck_tile::make_kernel<1>(ChunkedKernel{}, grids, blocks, 0, chunked_kargs));
    }
    else
    {
        // Communicator init (once). MPI case: rank 0 generates ncclUniqueId and broadcasts it.
        ncclUniqueId uid{};
        if(world_rank == 0)
        {
            nccl_check(ncclGetUniqueId(&uid), "ncclGetUniqueId");
        }
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
        MPI_Bcast(&uid, static_cast<int>(sizeof(uid)), MPI_BYTE, 0, MPI_COMM_WORLD);
#endif
        ncclComm_t comm{};
        nccl_check(ncclCommInitRank(&comm, /*nranks=*/world_size, uid, /*rank=*/world_rank),
                   "ncclCommInitRank");

        std::cout << "[rank " << world_rank << "] ncclCommInitRank done" << std::endl;

        const uint32_t tiles_m = ceil_div<uint32_t>(static_cast<uint32_t>(M), TilePartitioner::MPerBlock);
        const uint32_t tiles_n = ceil_div<uint32_t>(static_cast<uint32_t>(N), TilePartitioner::NPerBlock);
        const uint32_t tiles_m_per_chunk = ceil_div<uint32_t>(tiles_m, num_chunks);

        struct SuiteResult
        {
            float ave_ms_chunked      = 0.0f;
            float ave_ms_whole_buffer = 0.0f;
            float ave_ms_chunk_launch = 0.0f;
        };

        auto run_suite = [&](bool use_default_streams_this, const char* mode_tag) -> SuiteResult {
            hipStream_t compute_stream = nullptr;
            hipStream_t comm_stream    = nullptr;
            bool used_cu_masks         = false;

            if(use_default_streams_this)
            {
                // Two independent streams with HIP defaults.
                hip_check(hipStreamCreate(&compute_stream), "hipStreamCreate(compute_stream)");
                hip_check(hipStreamCreate(&comm_stream), "hipStreamCreate(comm_stream)");
            }
            else
            {
                if(!chunk_done_is_signal_memory && reserve_comm_cus > 0)
                {
                    used_cu_masks = create_streams_with_reserved_cus(reserve_comm_cus,
                                                                    &compute_stream,
                                                                    &comm_stream);
                    if(!used_cu_masks && world_rank == 0)
                    {
                        std::cerr << "Warning: failed to create CU-masked streams; falling back to normal streams\n";
                    }
                }
                if(!used_cu_masks)
                {
                    hip_check(hipStreamCreateWithFlags(&compute_stream, hipStreamNonBlocking),
                              "hipStreamCreateWithFlags(compute_stream)");
                    hip_check(hipStreamCreateWithFlags(&comm_stream, hipStreamNonBlocking),
                              "hipStreamCreateWithFlags(comm_stream)");
                }
            }

            hipEvent_t reset_evt{};
            hipEvent_t comm_done_evt{};
            hip_check(hipEventCreateWithFlags(&reset_evt, hipEventDisableTiming),
                      "hipEventCreate(reset_evt)");
            hip_check(hipEventCreateWithFlags(&comm_done_evt, hipEventDisableTiming),
                      "hipEventCreate(comm_done_evt)");

            // Timing for the full pipeline (compute + per-chunk waits + per-chunk allreduce).
            // We reuse streams/events across all iterations.
            const int total_iters = warmup + repeat;
            std::vector<hipEvent_t> start_evts;
            std::vector<hipEvent_t> stop_evts;
            if(repeat > 0)
            {
                start_evts.resize(static_cast<std::size_t>(repeat));
                stop_evts.resize(static_cast<std::size_t>(repeat));
                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventCreateWithFlags(&start_evts[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(start)");
                    hip_check(hipEventCreateWithFlags(&stop_evts[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(stop)");
                }
            }

            ck_tile::stream_config compute_sc{};
            compute_sc.stream_id_ = compute_stream;

            for(int iter = 0; iter < total_iters; ++iter)
            {
                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(start_evts[static_cast<std::size_t>(ti)], compute_stream),
                              "hipEventRecord(start)");
                }

                // Reset signal on comm_stream, then make compute_stream wait for it.
                // This avoids the "stale value already satisfies wait" hazard across iterations.
                hip_check(hipMemsetAsync(d_chunk_done, 0, chunk_done_bytes, comm_stream),
                          "hipMemsetAsync(chunk_done, comm_stream)");
                hip_check(hipEventRecord(reset_evt, comm_stream), "hipEventRecord(reset_evt)");
                hip_check(hipStreamWaitEvent(compute_stream, reset_evt, 0),
                          "hipStreamWaitEvent(reset_evt)");

                // Launch one chunked GEMM on compute_stream (and time the whole pipeline).
                ck_tile::launch_and_check(
                    compute_sc, ck_tile::make_kernel<1>(ChunkedKernel{}, grids, blocks, 0, chunked_kargs));

                // For each chunk:
                //  - wait until all blocks for that chunk have published completion
                //  - launch an RCCL op on the corresponding output region
                for(uint32_t c = 0; c < num_chunks; ++c)
                {
                    const uint32_t m0 = c * tiles_m_per_chunk;
                    const uint32_t m1 = std::min<uint32_t>(tiles_m, (c + 1) * tiles_m_per_chunk);
                    const uint32_t chunk_tile_rows = (m1 > m0) ? (m1 - m0) : 0;
                    const uint32_t expected_blocks = chunk_tile_rows * tiles_n;

                    if(chunk_done_is_signal_memory)
                    {
                        hip_check(hipStreamWaitValue32(comm_stream,
                                                       d_chunk_done + c,
                                                       expected_blocks,
                                                       hipStreamWaitValueGte),
                                  "hipStreamWaitValue32(chunk_done)");
                    }
                    else
                    {
                        wait_value_eq_kernel<<<dim3(1, 1, 1), dim3(1, 1, 1), 0, comm_stream>>>(
                            d_chunk_done + c, expected_blocks);
                        hip_check(hipGetLastError(), "wait_value_eq_kernel launch");
                    }

                    // Chunk region in E in element space: rows [m0*MPerBlock, m1*MPerBlock).
                    const uint32_t row0 = m0 * static_cast<uint32_t>(TilePartitioner::MPerBlock);
                    const uint32_t row1 = std::min<uint32_t>(
                        static_cast<uint32_t>(M),
                        m1 * static_cast<uint32_t>(TilePartitioner::MPerBlock));
                    const uint32_t chunk_rows = (row1 > row0) ? (row1 - row0) : 0;
                    const std::size_t chunk_elems =
                        static_cast<std::size_t>(chunk_rows) * static_cast<std::size_t>(N);

                    if(chunk_elems > 0)
                    {
                        void* chunk_ptr = static_cast<void*>(
                            dE + static_cast<std::size_t>(row0) * static_cast<std::size_t>(stride_E));

                        nccl_check(ncclAllReduce(/*sendbuff=*/chunk_ptr,
                                                 /*recvbuff=*/chunk_ptr,
                                                 /*count=*/chunk_elems,
                                                 /*datatype=*/ncclHalf,
                                                 /*op=*/ncclSum,
                                                 /*comm=*/comm,
                                                 /*stream=*/comm_stream),
                                   "ncclAllReduce(ncclSum, chunk)");
                    }
                }

                hip_check(hipEventRecord(comm_done_evt, comm_stream), "hipEventRecord(comm_done_evt)");
                hip_check(hipStreamWaitEvent(compute_stream, comm_done_evt, 0),
                          "hipStreamWaitEvent(comm_done_evt)");

                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(stop_evts[static_cast<std::size_t>(ti)], compute_stream),
                              "hipEventRecord(stop)");
                }
            }

            hip_check(hipStreamSynchronize(compute_stream),
                      "hipStreamSynchronize(compute_stream)");

            float ave_ms_chunked = 0.0f;
            if(repeat > 0)
            {
                hip_check(hipEventSynchronize(stop_evts.back()),
                          "hipEventSynchronize(stop_last)");
                float sum_ms = 0.0f;
                for(int i = 0; i < repeat; ++i)
                {
                    float ms = 0.0f;
                    hip_check(hipEventElapsedTime(&ms,
                                                  start_evts[static_cast<std::size_t>(i)],
                                                  stop_evts[static_cast<std::size_t>(i)]),
                              "hipEventElapsedTime(iter)");
                    sum_ms += ms;
                }
                ave_ms_chunked = sum_ms / static_cast<float>(repeat);

                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventDestroy(start_evts[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(start)");
                    hip_check(hipEventDestroy(stop_evts[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(stop)");
                }
            }

            // --------------------------------------------
            // Comparable workload 1: non-chunked GEMM + one AllReduce on full output.
            // If we used CU-masked streams for the chunked test, run this reference on an unmasked stream.
            // --------------------------------------------
            hipStream_t compute_stream_ref = compute_stream;
            if(used_cu_masks)
            {
                hip_check(hipStreamCreateWithFlags(&compute_stream_ref, hipStreamNonBlocking),
                          "hipStreamCreateWithFlags(compute_stream_ref)");
            }

            std::vector<hipEvent_t> start_evts2;
            std::vector<hipEvent_t> stop_evts2;
            if(repeat > 0)
            {
                start_evts2.resize(static_cast<std::size_t>(repeat));
                stop_evts2.resize(static_cast<std::size_t>(repeat));
                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventCreateWithFlags(&start_evts2[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(start2)");
                    hip_check(hipEventCreateWithFlags(&stop_evts2[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(stop2)");
                }
            }

            ck_tile::stream_config compute_sc_ref{};
            compute_sc_ref.stream_id_ = compute_stream_ref;

            for(int iter = 0; iter < total_iters; ++iter)
            {
                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(start_evts2[static_cast<std::size_t>(ti)],
                                             compute_stream_ref),
                              "hipEventRecord(start2)");
                }

                ck_tile::launch_and_check(
                    compute_sc_ref, ck_tile::make_kernel<1>(BaseKernel{}, grids, blocks, 0, base_kargs));

                nccl_check(ncclAllReduce(/*sendbuff=*/static_cast<const void*>(dE),
                                         /*recvbuff=*/static_cast<void*>(dE),
                                         /*count=*/size_e,
                                         /*datatype=*/ncclHalf,
                                         /*op=*/ncclSum,
                                         /*comm=*/comm,
                                         /*stream=*/compute_stream_ref),
                           "ncclAllReduce(ncclSum, whole_E)");

                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(stop_evts2[static_cast<std::size_t>(ti)],
                                             compute_stream_ref),
                              "hipEventRecord(stop2)");
                }
            }

            hip_check(hipStreamSynchronize(compute_stream_ref),
                      "hipStreamSynchronize(compute_stream_ref)");

            float ave_ms_whole = 0.0f;
            if(repeat > 0)
            {
                hip_check(hipEventSynchronize(stop_evts2.back()),
                          "hipEventSynchronize(stop2_last)");
                float sum_ms2 = 0.0f;
                for(int i = 0; i < repeat; ++i)
                {
                    float ms = 0.0f;
                    hip_check(hipEventElapsedTime(&ms,
                                                  start_evts2[static_cast<std::size_t>(i)],
                                                  stop_evts2[static_cast<std::size_t>(i)]),
                              "hipEventElapsedTime(iter2)");
                    sum_ms2 += ms;
                }
                ave_ms_whole = sum_ms2 / static_cast<float>(repeat);

                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventDestroy(start_evts2[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(start2)");
                    hip_check(hipEventDestroy(stop_evts2[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(stop2)");
                }
            }

            if(used_cu_masks)
            {
                hip_check(hipStreamDestroy(compute_stream_ref),
                          "hipStreamDestroy(compute_stream_ref)");
            }

            // --------------------------------------------
            // Comparable workload 2: per-chunk GEMM launches + per-chunk AllReduce, overlapped via events.
            // --------------------------------------------
            struct ChunkLaunch
            {
                typename BaseKernel::KernelArgs kargs;
                dim3 grid;
                uint32_t row0;
                uint32_t row1;
                std::size_t elems;
            };

            std::vector<ChunkLaunch> chunk_launches;
            chunk_launches.reserve(num_chunks);

            for(uint32_t c = 0; c < num_chunks; ++c)
            {
                const uint32_t m0 = c * tiles_m_per_chunk;
                const uint32_t m1 = std::min<uint32_t>(tiles_m, (c + 1) * tiles_m_per_chunk);

                const uint32_t row0 = m0 * static_cast<uint32_t>(TilePartitioner::MPerBlock);
                const uint32_t row1 = std::min<uint32_t>(
                    static_cast<uint32_t>(M),
                    m1 * static_cast<uint32_t>(TilePartitioner::MPerBlock));
                const uint32_t m_chunk = (row1 > row0) ? (row1 - row0) : 0;
                const std::size_t elems = static_cast<std::size_t>(m_chunk) * static_cast<std::size_t>(N);

                if(m_chunk == 0)
                {
                    chunk_launches.push_back(ChunkLaunch{base_kargs, dim3(0, 0, 0), row0, row1, 0});
                    continue;
                }

                const auto* dA_chunk =
                    dA + static_cast<std::size_t>(row0) * static_cast<std::size_t>(stride_A);
                auto* dE_chunk =
                    dE + static_cast<std::size_t>(row0) * static_cast<std::size_t>(stride_E);

                const std::array<const void*, NumATensor> as_ptr_chunk =
                    {static_cast<const void*>(dA_chunk)};
                const std::array<const void*, NumBTensor> bs_ptr_chunk =
                    {static_cast<const void*>(dB)};
                const std::array<const void*, NumDTensor> ds_ptr_chunk = {};

                const ck_tile::UniversalGemmHostArgs<NumATensor, NumBTensor, NumDTensor> host_args_chunk{
                    as_ptr_chunk,
                    bs_ptr_chunk,
                    ds_ptr_chunk,
                    static_cast<void*>(dE_chunk),
                    k_batch,
                    static_cast<ck_tile::index_t>(m_chunk),
                    N,
                    K,
                    stride_As,
                    stride_Bs,
                    stride_Ds,
                    stride_E};

                const auto kargs_chunk = BaseKernel::MakeKernelArgs(host_args_chunk);
                if(!BaseKernel::IsSupportedArgument(kargs_chunk))
                {
                    std::cerr << "BaseKernel does not support chunk arguments (chunk " << c << ").\n";
                    std::exit(1);
                }

                const dim3 grids_chunk =
                    BaseKernel::GridSize(static_cast<ck_tile::index_t>(m_chunk), N, k_batch);
                chunk_launches.push_back(ChunkLaunch{kargs_chunk, grids_chunk, row0, row1, elems});
            }

            std::vector<hipEvent_t> chunk_done_evts(num_chunks);
            for(uint32_t c = 0; c < num_chunks; ++c)
            {
                hip_check(hipEventCreateWithFlags(&chunk_done_evts[c], hipEventDisableTiming),
                          "hipEventCreate(chunk_done)");
            }

            std::vector<hipEvent_t> start_evts3;
            std::vector<hipEvent_t> stop_evts3;
            if(repeat > 0)
            {
                start_evts3.resize(static_cast<std::size_t>(repeat));
                stop_evts3.resize(static_cast<std::size_t>(repeat));
                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventCreateWithFlags(&start_evts3[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(start3)");
                    hip_check(hipEventCreateWithFlags(&stop_evts3[static_cast<std::size_t>(i)],
                                                     hipEventDefault),
                              "hipEventCreate(stop3)");
                }
            }

            for(int iter = 0; iter < total_iters; ++iter)
            {
                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(start_evts3[static_cast<std::size_t>(ti)], compute_stream),
                              "hipEventRecord(start3)");
                }

                for(uint32_t c = 0; c < num_chunks; ++c)
                {
                    const auto& ch = chunk_launches[c];
                    if(ch.elems == 0)
                        continue;

                    ck_tile::launch_and_check(
                        compute_sc,
                        ck_tile::make_kernel<1>(BaseKernel{}, ch.grid, blocks, 0, ch.kargs));

                    hip_check(hipEventRecord(chunk_done_evts[c], compute_stream),
                              "hipEventRecord(chunk_done)");
                    hip_check(hipStreamWaitEvent(comm_stream, chunk_done_evts[c], 0),
                              "hipStreamWaitEvent(chunk_done)");

                    void* chunk_ptr = static_cast<void*>(
                        dE + static_cast<std::size_t>(ch.row0) * static_cast<std::size_t>(stride_E));

                    nccl_check(ncclAllReduce(/*sendbuff=*/chunk_ptr,
                                             /*recvbuff=*/chunk_ptr,
                                             /*count=*/ch.elems,
                                             /*datatype=*/ncclHalf,
                                             /*op=*/ncclSum,
                                             /*comm=*/comm,
                                             /*stream=*/comm_stream),
                               "ncclAllReduce(ncclSum, chunk_launch)");
                }

                hip_check(hipEventRecord(comm_done_evt, comm_stream),
                          "hipEventRecord(comm_done_evt3)");
                hip_check(hipStreamWaitEvent(compute_stream, comm_done_evt, 0),
                          "hipStreamWaitEvent(comm_done_evt3)");

                if(iter >= warmup && repeat > 0)
                {
                    const int ti = iter - warmup;
                    hip_check(hipEventRecord(stop_evts3[static_cast<std::size_t>(ti)], compute_stream),
                              "hipEventRecord(stop3)");
                }
            }

            hip_check(hipStreamSynchronize(compute_stream),
                      "hipStreamSynchronize(compute_stream3)");

            float ave_ms_chunk_launch = 0.0f;
            if(repeat > 0)
            {
                hip_check(hipEventSynchronize(stop_evts3.back()),
                          "hipEventSynchronize(stop3_last)");
                float sum_ms3 = 0.0f;
                for(int i = 0; i < repeat; ++i)
                {
                    float ms = 0.0f;
                    hip_check(hipEventElapsedTime(&ms,
                                                  start_evts3[static_cast<std::size_t>(i)],
                                                  stop_evts3[static_cast<std::size_t>(i)]),
                              "hipEventElapsedTime(iter3)");
                    sum_ms3 += ms;
                }
                ave_ms_chunk_launch = sum_ms3 / static_cast<float>(repeat);

                for(int i = 0; i < repeat; ++i)
                {
                    hip_check(hipEventDestroy(start_evts3[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(start3)");
                    hip_check(hipEventDestroy(stop_evts3[static_cast<std::size_t>(i)]),
                              "hipEventDestroy(stop3)");
                }
            }

            for(uint32_t c = 0; c < num_chunks; ++c)
            {
                hip_check(hipEventDestroy(chunk_done_evts[c]), "hipEventDestroy(chunk_done)");
            }

            std::cout << "[rank " << world_rank << "] stream_mode=" << mode_tag
                      << " chunked_avg_ms=" << ave_ms_chunked
                      << " whole_avg_ms=" << ave_ms_whole
                      << " per_chunk_launch_avg_ms=" << ave_ms_chunk_launch << "\n";

            hip_check(hipEventDestroy(comm_done_evt), "hipEventDestroy(comm_done_evt)");
            hip_check(hipEventDestroy(reset_evt), "hipEventDestroy(reset_evt)");
            hip_check(hipStreamDestroy(comm_stream), "hipStreamDestroy(comm_stream)");
            hip_check(hipStreamDestroy(compute_stream), "hipStreamDestroy(compute_stream)");

            return SuiteResult{ave_ms_chunked, ave_ms_whole, ave_ms_chunk_launch};
        };

        SuiteResult r_chosen{};

        if(run_both_stream_modes)
        {
            (void)run_suite(/*use_default_streams_this=*/true, "default");
            r_chosen = run_suite(/*use_default_streams_this=*/false, "current");
            ave_ms   = r_chosen.ave_ms_chunked;
        }
        else
        {
            r_chosen = run_suite(/*use_default_streams_this=*/false, "current");
            ave_ms             = r_chosen.ave_ms_chunked;
        }

        nccl_check(ncclCommDestroy(comm), "ncclCommDestroy");
    }

    // GEMM FLOPs: 2*M*N*K
    const double flops  = 2.0 * static_cast<double>(M) * static_cast<double>(N) *
                         static_cast<double>(K);
    const double tflops =
        (ave_ms > 0.0f) ? (flops / (static_cast<double>(ave_ms) * 1.0e-3) / 1.0e12) : 0.0;

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
        const float expected = rccl_per_chunk ? (exp * static_cast<float>(world_size)) : exp;
        std::cout << "spotcheck_abs_err=" << std::abs(got - expected) << "\n";
    }

    std::vector<uint32_t> h_chunk_done(num_chunks);
    hip_check(hipMemcpy(h_chunk_done.data(),
                        d_chunk_done,
                        sizeof(uint32_t) * static_cast<std::size_t>(num_chunks),
                        hipMemcpyDeviceToHost),
              "hipMemcpy(chunk_done)");

    const uint32_t tiles_m = ceil_div<uint32_t>(static_cast<uint32_t>(M), TilePartitioner::MPerBlock);
    const uint32_t tiles_n = ceil_div<uint32_t>(static_cast<uint32_t>(N), TilePartitioner::NPerBlock);
    const uint32_t tiles_m_per_chunk = ceil_div<uint32_t>(tiles_m, num_chunks);
    (void)warmup;
    (void)repeat;

    std::cout << "UniversalGemm chuncked example\n";
    std::cout << "M=" << M << " N=" << N << " K=" << K << " chunks=" << num_chunks << "\n";
    std::cout << "warmup=" << warmup << " repeat=" << repeat << "\n";
    const char* selected_stream_mode = "current";
    std::cout << "[rank " << world_rank << ", dev_id=" << device_id << "] " << "avg_ms=" << ave_ms << " tflops=" << tflops
              << " rccl_per_chunk=" << (rccl_per_chunk ? 1 : 0)
              << " selected_stream_mode=" << selected_stream_mode
              << " run_both_stream_modes=" << (run_both_stream_modes ? 1 : 0)
              << " reserve_comm_cus=" << reserve_comm_cus << "\n";
    std::cout << "chunk_done (tiles completed per chunk in the last launch):\n";

    for(uint32_t c = 0; c < num_chunks; ++c)
    {
        const uint32_t m0 = c * tiles_m_per_chunk;
        const uint32_t m1 = std::min<uint32_t>(tiles_m, (c + 1) * tiles_m_per_chunk);
        const uint32_t chunk_tile_rows = (m1 > m0) ? (m1 - m0) : 0;
        const uint32_t expected_per_launch = chunk_tile_rows * tiles_n;
        std::cout << "  chunk " << c << ": got=" << h_chunk_done[c]
                  << " expected=" << expected_per_launch << "\n";
    }

    hip_check(hipFree(d_chunk_done), "hipFree(chunk_done)");
    hip_check(hipFree(dA), "hipFree(A)");
    hip_check(hipFree(dB), "hipFree(B)");
    hip_check(hipFree(dE), "hipFree(E)");

    return 0;
}


