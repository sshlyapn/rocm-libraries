// Simplified reproducer for suspected RCCL bug.
//
// Mirrors the stream / synchronization structure of
// universal_gemm_persistent_all_reduce_ref.cpp but replaces the real
// GEMM and reduction kernels with trivial dummy kernels so that all
// CK-tile dependencies are removed.
//
// Build (example):
//   hipcc -O2 -std=c++17 -lrccl -lmpi -DCK_TILE_EXAMPLE_USE_MPI=1 \
//         rccl_issue_reproducer.cpp -o rccl_issue_reproducer
//
// Run (example, 8 GPUs):
//   mpirun -np 8 ./rccl_issue_reproducer --m 4096 --n 4096 \
//          --warmup 50 --repeat 100 --tokens_per_reduction 4

#include <hip/hip_runtime.h>
#include <rccl/rccl.h>

#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
#include <mpi.h>
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
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
// Dummy "GEMM" kernel — just fills the output buffer with a pattern so
// that subsequent reduction / AllGather operate on non-zero data.
// Grid / block can be anything; we mimic a persistent-style launch.
// -----------------------------------------------------------------------
__global__ void dummy_gemm_kernel(half* __restrict__ out,
                                  uint32_t num_elems,
                                  uint32_t total_wgs)
{
    const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t stride = total_wgs * blockDim.x;
    for(uint32_t i = gid; i < num_elems; i += stride)
    {
        // deterministic pattern
        out[i] = __float2half(static_cast<float>(i % 127) * 0.01f);
    }
}

// -----------------------------------------------------------------------
// Dummy "reduction" kernel — reads from e_ptr, writes to reduced_ptr.
// Mimics the memory-access pattern (row gather) without the real math.
// -----------------------------------------------------------------------
struct ReduceArgs
{
    half* e_ptr;
    half* reduced_ptr;
    const int32_t* token_map;
    uint32_t num_post_work;
    uint32_t N;
};

__global__ void dummy_reduce_kernel(ReduceArgs args)
{
    static constexpr int warp_size = 64;

    const int lane   = threadIdx.x % warp_size;
    const int warp   = (blockIdx.x * blockDim.x + threadIdx.x) / warp_size;
    const int nwarps = (gridDim.x * blockDim.x) / warp_size;

    for(uint32_t work_id = static_cast<uint32_t>(warp);
        work_id < args.num_post_work;
        work_id += static_cast<uint32_t>(nwarps))
    {
        const int32_t row0 = args.token_map[work_id * warp_size + lane];

        // Just copy the first valid row into the reduced buffer (dummy).
        int32_t first_row = __builtin_amdgcn_readlane(row0, 0);
        if(first_row < 0)
            continue;

        // Each lane copies a few columns
        for(uint32_t col = static_cast<uint32_t>(lane); col < args.N;
            col += static_cast<uint32_t>(warp_size))
        {
            args.reduced_ptr[work_id * args.N + col] =
                args.e_ptr[static_cast<uint32_t>(first_row) * args.N + col];
        }
    }
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(int argc, char** argv)
{
    // -----------------------------------------------------------------
    // MPI + NCCL initialization (same as the real benchmark)
    // -----------------------------------------------------------------
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
        MPI_Comm_split_type(
            MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &local_comm);
        MPI_Comm_rank(local_comm, &local_rank);
        MPI_Comm_free(&local_comm);
    }
#endif

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
    nccl_check(ncclCommInitRank(&nccl_comm, world_size, uid, world_rank),
               "ncclCommInitRank");

    std::cout << "[rank " << world_rank << "] NCCL initialized\n";

    // -----------------------------------------------------------------
    // CLI arguments (stripped down)
    // -----------------------------------------------------------------
    uint32_t M       = 4096;
    uint32_t N       = 4096;
    int warmup       = 50;
    int repeat       = 100;
    uint32_t tokens_per_reduction = 0;
    uint32_t forced_num_compute_wgs = 0;

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
            M = static_cast<uint32_t>(parse_long(need_value("--m"), "--m"));
        else if(std::strcmp(key, "--n") == 0)
            N = static_cast<uint32_t>(parse_long(need_value("--n"), "--n"));
        else if(std::strcmp(key, "--warmup") == 0)
            warmup = static_cast<int>(parse_long(need_value("--warmup"), "--warmup"));
        else if(std::strcmp(key, "--repeat") == 0)
            repeat = static_cast<int>(parse_long(need_value("--repeat"), "--repeat"));
        else if(std::strcmp(key, "--tokens_per_reduction") == 0)
            tokens_per_reduction = static_cast<uint32_t>(
                parse_long(need_value("--tokens_per_reduction"), "--tokens_per_reduction"));
        else if(std::strcmp(key, "--forced_num_compute_wgs") == 0)
            forced_num_compute_wgs = static_cast<uint32_t>(
                parse_long(need_value("--forced_num_compute_wgs"), "--forced_num_compute_wgs"));
        else if(std::strcmp(key, "--help") == 0 || std::strcmp(key, "-h") == 0)
        {
            std::cout << "Usage: " << argv[0]
                      << " [--m M] [--n N] [--warmup W] [--repeat R]\n"
                      << "    [--tokens_per_reduction T] [--forced_num_compute_wgs G]\n";
            return 0;
        }
    }

    const int n_pes = world_size;

    // -----------------------------------------------------------------
    // Allocate device memory
    // -----------------------------------------------------------------
    const std::size_t size_e = static_cast<std::size_t>(M) * N;

    half* dE = nullptr;
    hip_check(hipMalloc(&dE, sizeof(half) * size_e), "hipMalloc(E)");
    hip_check(hipMemset(dE, 0, sizeof(half) * size_e), "hipMemset(E)");

    // -----------------------------------------------------------------
    // Build token_map (same logic as the real benchmark)
    // -----------------------------------------------------------------
    static constexpr int kWarpSize = 64;
    int32_t* d_token_map           = nullptr;
    uint32_t num_post_work         = 0;

    if(tokens_per_reduction > 0)
    {
        if(tokens_per_reduction > static_cast<uint32_t>(kWarpSize))
        {
            std::cerr << "--tokens_per_reduction must be <= " << kWarpSize << "\n";
            return 2;
        }

        const uint32_t stride = (M + tokens_per_reduction - 1) / tokens_per_reduction;
        num_post_work         = stride;

        const std::size_t map_elems = static_cast<std::size_t>(num_post_work) * kWarpSize;
        std::vector<int32_t> h_token_map(map_elems, -1);

        for(uint32_t g = 0; g < num_post_work; g++)
        {
            for(uint32_t t = 0; t < tokens_per_reduction; t++)
            {
                const uint32_t row = g + t * stride;
                if(row < M)
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
                  << " tokens_per_reduction=" << tokens_per_reduction << "\n";
    }

    // -----------------------------------------------------------------
    // Reduced-output + AllGather buffers (same layout as real benchmark)
    // -----------------------------------------------------------------
    half* dE_reduced = nullptr;
    const std::size_t size_reduced =
        static_cast<std::size_t>(num_post_work) * N;

    if(num_post_work > 0)
    {
        hip_check(hipMalloc(&dE_reduced, sizeof(half) * size_reduced),
                  "hipMalloc(E_reduced)");
        hip_check(hipMemset(dE_reduced, 0, sizeof(half) * size_reduced),
                  "hipMemset(E_reduced)");
    }

    const std::size_t ag_send_count = (num_post_work > 0) ? size_reduced : size_e;
    const std::size_t size_ag_total = static_cast<std::size_t>(n_pes) * ag_send_count;

    half* dE_all = nullptr;
    hip_check(hipMalloc(&dE_all, sizeof(half) * size_ag_total), "hipMalloc(E_all)");
    hip_check(hipMemset(dE_all, 0, sizeof(half) * size_ag_total), "hipMemset(E_all)");

    // -----------------------------------------------------------------
    // Kernel launch parameters
    // -----------------------------------------------------------------
    // Dummy GEMM: mimic persistent launch (occupancy-based grid)
    int gemm_block_size = 256;
    int max_blocks_per_sm = 0;
    hip_check(hipOccupancyMaxActiveBlocksPerMultiprocessor(
                  &max_blocks_per_sm, dummy_gemm_kernel, gemm_block_size, 0),
              "hipOccupancy(gemm)");

    hipDeviceProp_t prop{};
    hip_check(hipGetDeviceProperties(&prop, device_id), "hipGetDeviceProperties");
    uint32_t gemm_grid = static_cast<uint32_t>(max_blocks_per_sm * prop.multiProcessorCount);
    if(forced_num_compute_wgs > 0)
        gemm_grid = forced_num_compute_wgs;

    // Dummy reduction kernel
    const int reduce_block_size = 256;
    const int warps_per_reduce_block = reduce_block_size / kWarpSize;
    const int reduce_grid_size = (num_post_work > 0)
        ? static_cast<int>(
              (num_post_work + warps_per_reduce_block - 1) / warps_per_reduce_block)
        : 0;

    ReduceArgs reduce_kargs{};
    reduce_kargs.e_ptr        = dE;
    reduce_kargs.reduced_ptr  = dE_reduced;
    reduce_kargs.token_map    = d_token_map;
    reduce_kargs.num_post_work = num_post_work;
    reduce_kargs.N            = N;

    const void* ag_send_ptr = (dE_reduced != nullptr)
        ? static_cast<const void*>(dE_reduced)
        : static_cast<const void*>(dE);

    std::cout << "[rank " << world_rank << "] gemm_grid=" << gemm_grid
              << " reduce_grid=" << reduce_grid_size
              << " ag_send_count=" << ag_send_count
              << " warmup=" << warmup << " repeat=" << repeat << "\n";

    // -----------------------------------------------------------------
    // Dedicated benchmark stream (same as the real benchmark)
    // -----------------------------------------------------------------
    hipStream_t bench_stream;
    hip_check(hipStreamCreate(&bench_stream), "hipStreamCreate(bench)");

    // One iteration: GEMM → reduce → AllGather, all on bench_stream
    auto run_one_iter = [&]() {
        dummy_gemm_kernel<<<gemm_grid, gemm_block_size, 0, bench_stream>>>(
            dE,
            static_cast<uint32_t>(size_e),
            gemm_grid);

        if(reduce_grid_size > 0)
        {
            dummy_reduce_kernel<<<reduce_grid_size, reduce_block_size, 0, bench_stream>>>(
                reduce_kargs);
        }

        if(tokens_per_reduction > 0)
        {
            nccl_check(ncclAllGather(
                           ag_send_ptr,
                           static_cast<void*>(dE_all),
                           ag_send_count,
                           ncclFloat16,
                           nccl_comm,
                           bench_stream),
                       "ncclAllGather");
        }
    };

    // -----------------------------------------------------------------
    // Synchronize all ranks before entering timing loop
    // -----------------------------------------------------------------
#if defined(CK_TILE_EXAMPLE_USE_MPI) && CK_TILE_EXAMPLE_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    // --- Warmup ---
    for(int i = 0; i < warmup; i++)
    {
        run_one_iter();
    }
    hip_check(hipStreamSynchronize(bench_stream), "hipStreamSynchronize(warmup)");

    // --- Timed phase ---
    hipEvent_t t_start, t_stop;
    hip_check(hipEventCreate(&t_start), "hipEventCreate(start)");
    hip_check(hipEventCreate(&t_stop), "hipEventCreate(stop)");

    hip_check(hipEventRecord(t_start, bench_stream), "hipEventRecord(start)");
    for(int i = 0; i < repeat; i++)
    {
        run_one_iter();
    }
    hip_check(hipEventRecord(t_stop, bench_stream), "hipEventRecord(stop)");
    hip_check(hipEventSynchronize(t_stop), "hipEventSynchronize(stop)");

    float elapsed_ms = 0.0f;
    hip_check(hipEventElapsedTime(&elapsed_ms, t_start, t_stop), "hipEventElapsedTime");
    elapsed_ms /= static_cast<float>(repeat);

    std::cout << "[rank " << world_rank << "] avg iteration time = "
              << elapsed_ms << " ms\n";

    // -----------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------
    hip_check(hipEventDestroy(t_start), "hipEventDestroy");
    hip_check(hipEventDestroy(t_stop), "hipEventDestroy");
    hip_check(hipStreamDestroy(bench_stream), "hipStreamDestroy");

    hip_check(hipFree(dE), "hipFree(E)");
    hip_check(hipFree(dE_all), "hipFree(E_all)");
    if(dE_reduced)
        hip_check(hipFree(dE_reduced), "hipFree(E_reduced)");
    if(d_token_map)
        hip_check(hipFree(d_token_map), "hipFree(token_map)");

    nccl_check(ncclCommDestroy(nccl_comm), "ncclCommDestroy");

    std::cout << "[rank " << world_rank << "] done.\n";
    return 0;
}

