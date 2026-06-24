// HIP Graph inter-kernel gap test - the TRUE apples-to-apples analog of a
// recorded Vulkan command buffer.
//
// A Vulkan command buffer is recorded ONCE and submitted; there is no per-dispatch
// CPU launch cost during execution. The equivalent in HIP is a hipGraph: K kernel
// nodes in a linear dependency chain (mirroring the SHADER_WRITE->SHADER_READ
// barrier), instantiated once into a hipGraphExec, then launched. The CPU launch
// loop (hip_gap_test.cpp) instead pays per-kernel hipLaunchKernel cost, which can
// dominate and is NOT what Vulkan does.
//
// Gap is measured on the GPU timeline with the 100 MHz wall clock, same as before.
//
// Usage: hip_gap_graph [K] [spin] [n]
// Build: hipcc -O2 --offload-arch=gfx1100 hip_gap_graph.cpp -o hip_gap_graph

#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define HIPCHECK(x)                                                                       \
    do {                                                                                  \
        hipError_t _e = (x);                                                              \
        if (_e != hipSuccess) {                                                           \
            fprintf(stderr, "HIP error '%s' at %s:%d\n", hipGetErrorString(_e), __FILE__, __LINE__); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

__device__ __forceinline__ unsigned long long gpu_realtime() {
    return (unsigned long long)__builtin_readsteadycounter();
}

__global__ void gap_kernel(float * data, uint32_t spin, uint32_t n,
                           unsigned long long * tstart, unsigned long long * tend, uint32_t kidx) {
    if (threadIdx.x == 0) atomicMin(&tstart[kidx], gpu_realtime());
    uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid < n) {
        float v = data[gid];
        for (uint32_t i = 0; i < spin; ++i) v = v * 1.0000001f + 1.0f;
        data[gid] = v;
    }
    __syncthreads();
    if (threadIdx.x == 0) atomicMax(&tend[kidx], gpu_realtime());
}

int main(int argc, char ** argv) {
    int      K    = argc > 1 ? atoi(argv[1]) : 2000;
    uint32_t spin = argc > 2 ? (uint32_t)atoi(argv[2]) : 0;
    uint32_t n    = argc > 3 ? (uint32_t)atoi(argv[3]) : 4096;

    hipDeviceProp_t prop;
    HIPCHECK(hipGetDeviceProperties(&prop, 0));
    printf("device : %s (%s)  K=%d spin=%u n=%u  [HIP GRAPH, linear chain]\n", prop.name, prop.gcnArchName, K,
           spin, n);

    float * data;
    HIPCHECK(hipMalloc(&data, (size_t)std::max(n, 64u) * sizeof(float)));
    HIPCHECK(hipMemset(data, 0, (size_t)std::max(n, 64u) * sizeof(float)));
    unsigned long long *tstart, *tend;
    HIPCHECK(hipMalloc(&tstart, (size_t)K * sizeof(unsigned long long)));
    HIPCHECK(hipMalloc(&tend, (size_t)K * sizeof(unsigned long long)));

    uint32_t blocks = (n + 255) / 256, threads = 256;

    // Build a graph: K kernel nodes, node k depends on node k-1 (linear chain = RAW barrier).
    hipGraph_t graph;
    HIPCHECK(hipGraphCreate(&graph, 0));
    std::vector<hipGraphNode_t> nodes(K);
    std::vector<uint32_t>       kidx(K);
    for (int k = 0; k < K; k++) kidx[k] = (uint32_t)k;

    for (int k = 0; k < K; k++) {
        void * args[] = { &data, &spin, &n, &tstart, &tend, &kidx[k] };
        hipKernelNodeParams p = {};
        p.func           = (void *)gap_kernel;
        p.gridDim        = dim3(blocks, 1, 1);
        p.blockDim       = dim3(threads, 1, 1);
        p.sharedMemBytes = 0;
        p.kernelParams   = args;
        p.extra          = nullptr;
        hipGraphNode_t deps[1];
        size_t         ndeps = 0;
        if (k > 0) { deps[0] = nodes[k - 1]; ndeps = 1; }
        HIPCHECK(hipGraphAddKernelNode(&nodes[k], graph, ndeps ? deps : nullptr, ndeps, &p));
    }

    hipGraphExec_t exec;
    HIPCHECK(hipGraphInstantiate(&exec, graph, nullptr, nullptr, 0));

    hipStream_t stream;
    HIPCHECK(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));

    auto reset_ts = [&]() {
        HIPCHECK(hipMemset(tstart, 0xFF, (size_t)K * sizeof(unsigned long long)));
        HIPCHECK(hipMemset(tend, 0x00, (size_t)K * sizeof(unsigned long long)));
    };

    reset_ts();
    HIPCHECK(hipGraphLaunch(exec, stream));  // warmup
    HIPCHECK(hipStreamSynchronize(stream));

    reset_ts();
    hipEvent_t e0, e1;
    HIPCHECK(hipEventCreate(&e0));
    HIPCHECK(hipEventCreate(&e1));
    HIPCHECK(hipEventRecord(e0, stream));
    HIPCHECK(hipGraphLaunch(exec, stream));
    HIPCHECK(hipEventRecord(e1, stream));
    HIPCHECK(hipStreamSynchronize(stream));
    float wall_ms = 0;
    HIPCHECK(hipEventElapsedTime(&wall_ms, e0, e1));

    std::vector<unsigned long long> hs(K), he(K);
    HIPCHECK(hipMemcpy(hs.data(), tstart, (size_t)K * sizeof(unsigned long long), hipMemcpyDeviceToHost));
    HIPCHECK(hipMemcpy(he.data(), tend, (size_t)K * sizeof(unsigned long long), hipMemcpyDeviceToHost));

    unsigned long long span_ticks = he[K - 1] - hs[0];
    double ns_per_tick = (double)wall_ms * 1e6 / (double)span_ticks;
    auto to_us = [&](long long t) { return (double)t * ns_per_tick / 1000.0; };

    std::vector<double> kern, gap;
    for (int k = 0; k < K; k++) {
        kern.push_back(to_us((long long)(he[k] - hs[k])));
        if (k < K - 1) gap.push_back(to_us((long long)((long long)hs[k + 1] - (long long)he[k])));
    }
    int skip = std::min(50, K / 10);
    auto med = [&](std::vector<double> v) {
        std::vector<double> s(v.begin() + std::min((size_t)skip, v.size()), v.end());
        std::sort(s.begin(), s.end());
        return s.empty() ? 0.0 : s[s.size() / 2];
    };
    printf("  calib %.4f ns/tick | kernel busy median = %.3f us | GAP median = %.3f us | period(span/K) = %.3f us\n",
           ns_per_tick, med(kern), med(gap), to_us((long long)span_ticks) / K);
    // Directly measured end-to-end GPU wall time of the whole graph launch
    // (single start/stop event pair around hipGraphLaunch; no per-dispatch math).
    printf("  E2E_WALL_MS = %.4f  (K=%d dispatches)\n", wall_ms, K);
    return 0;
}
