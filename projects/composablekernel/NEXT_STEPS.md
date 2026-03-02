## Context (what we have now)

You currently have an MPI-capable test in:

- `example/ck_tile/03_gemm_universal_minimal/universal_gemm_chuncked.cpp`

It can run (tp=8) on a single node (8× MI350X) and measures three end-to-end patterns:

- **A. “Chunked signals inside a single GEMM” + per-chunk RCCL**
  - Single GEMM kernel publishes per-chunk completion via `__threadfence()` + `atomicAdd()`
  - Consumer side waits (signal memory via `hipStreamWaitValue32`, or fallback wait kernel)
  - Per-chunk `ncclAllReduce(ncclSum)` enqueued after each chunk is ready
- **B. “Whole-buffer AllReduce”**
  - Non-chunked GEMM (BaseKernel) produces full `E[M,N]`
  - One `ncclAllReduce(ncclSum)` on the full output buffer (`count = M*N` elements)
- **C. “Per-chunk GEMM launches” + per-chunk RCCL**
  - GEMM is launched as multiple kernels, one per M-chunk
  - After each chunk GEMM finishes, enqueue per-chunk `ncclAllReduce(ncclSum)` on `comm_stream`
  - This enables overlap: `GEMM(chunk c+1)` can run while `AllReduce(chunk c)` runs

You also added **CU masking** support for the non-signal-memory wait-kernel case:

- `comm_stream` can be restricted to N CUs (e.g. 2 or 64)
- `compute_stream` can be restricted to the remaining CUs
- For the “whole-buffer AllReduce” reference, compute is intentionally **unmasked** for fairness

## Summary of observed results (your measurements)

You reported (tp=8, xGMI):

1. **Per-chunk GEMM launches + per-chunk RCCL (C) is best**
2. **Non-chunked GEMM + whole-buffer AllReduce (B) is second**
3. **Single GEMM with internal per-tile/chunk signaling (A) is worst**

You also observed that “best CU split” for overlap cases often occurs around **~50%/50% compute/comm**.

## Interpretation (reliable causes that match profiling)

### Why (A) can be worst even with good overlap

If profiling shows RCCL starts early (e.g. ~1/8 into GEMM), then the performance loss is not due to “no overlap” but due to **producer overhead**:

- `__threadfence()` and `atomicAdd()` per workgroup are real overhead.
- They add global memory traffic and ordering/serialization pressure in the hottest kernel.
- This overhead scales with the number of workgroups and can perturb GEMM’s cache/HBM behavior.

This is consistent with “A overlaps well but is slower”: overlap is not enough to compensate for the extra work inserted into GEMM.

### Why (B) is second

- Minimal kernel overhead (one GEMM + one AllReduce)
- But little/no overlap, so wall time trends toward:

\[
T \approx T_{\mathrm{GEMM}} + T_{\mathrm{AllReduce}}
\]

### Why (C) can be best

- Chunk readiness is deterministic (kernel boundary), so pipelining is predictable.
- Removes the per-tile atomic/fence overhead from GEMM.
- Enables true overlap:

\[
\mathrm{GEMM}(c+1)\ \parallel\ \mathrm{AllReduce}(c)
\]

### Why “50/50 CU split” can be optimal

RCCL collectives run as GPU kernels and need CU resources to reach high effective throughput. A balanced split often indicates you are near:

\[
T_{\mathrm{GEMM\_chunk}} \approx T_{\mathrm{AllReduce\_chunk}}
\]

So neither compute nor comm dominates the pipeline tail.

## Important clarification: what “whole-buffer AllReduce” means

“Whole-buffer AllReduce” == **one** `ncclAllReduce(ncclSum)` on the **entire output `E`** after GEMM finishes:

- `sendbuff = dE`, `recvbuff = dE`
- `count = M*N` elements
- `datatype = ncclHalf` (for fp16 output)

No chunking, no per-chunk overlap.

## Measurement directions: separate link vs HBM vs CU limits

You asked: “How can I measure link vs HBM vs CU?” The most reliable workflow is:

### 1) CU-saturation curve for AllReduce (no counters required)

Goal: find “how many CUs are needed to saturate AllReduce bandwidth” for your payload size.

- Run an AllReduce-only microbenchmark (or your “whole-buffer AllReduce” path with GEMM replaced by a memset/init).
- Sweep comm CU mask: 1, 2, 4, 8, 16, 32, 64, …
- Measure achieved bandwidth (GB/s).
- Look for the knee where bandwidth plateaus.

Interpretation:

- If bandwidth increases with comm CUs until plateau → **CU/algorithm/protocol limited** before plateau.
- If bandwidth barely changes with comm CUs → likely **link-limited** or **HBM-limited**.

### 2) Interference test: does AllReduce steal GEMM performance?

Goal: determine whether the bottleneck is link-only or shared with memory hierarchy.

- Measure GEMM-only TFLOPs baseline.
- Measure GEMM+AllReduce overlapped pipeline TFLOPs.
- If GEMM TFLOPs drops materially under overlap, that indicates **HBM/L2 contention** (both kernels moving lots of data).

This is particularly relevant because AllReduce reads+writes `E` while GEMM epilogue is writing `E` for neighboring chunks.

### 3) Counters with rocprofv3 (to attribute the plateau)

You can collect performance counters with rocprofv3:

- List available counters:

```bash
rocprofv3 -L
```

Then search for counter groups related to:

- **xGMI / interconnect traffic** (often includes `XGMI` in the name)
- **HBM / memory interface** (often includes `TCC`-related events)

Collect a small set of PMCs during:

- AllReduce-only runs
- GEMM-only runs
- GEMM+AllReduce overlap runs

Interpretation goals:

- If **xGMI bytes/sec** plateaus near hardware limits but GEMM still has headroom → link-limited.
- If **HBM/TCC bytes/sec** is near peak and xGMI isn’t saturated → HBM-limited (can’t feed links).
- If neither reaches peak but comm CUs improve both → CU/algorithm/protocol limited.

Notes:

- Prefer small counter sets so rocprof doesn’t force many passes/replays.
- Use the same message sizes and iteration counts across sweeps.

## Profiling hygiene (so traces match reality)

### Understand why wait kernels “overlap” in traces

When using the non-signal fallback, you enqueue one wait kernel per chunk. Traces in runtime/dispatch modes often show submit→complete intervals, so multiple wait kernels can appear overlapped even if they execute serially on the same stream.

### Improve stream/queue visibility in rocprofv3

For Perfetto traces:

- Add `--group-by-queue` so kernels are grouped by HIP streams rather than only by HSA queues.
- Keep `--kernel-trace` enabled (explicitly, even though `-r` includes it).

Example (MPI per-rank):

```bash
rocprofv3 -r --kernel-trace --group-by-queue -f pftrace -d ./profiling -o rank${OMPI_COMM_WORLD_RANK}_... -- ./app ...
```

## Pipeline improvement directions (within your constraints)

You noted:

- tp=8 is fixed
- model architecture requires an AllReduce(sum) of the output

So “reduce-scatter instead of allreduce” may be off the table. Within that constraint, focus on:

### 1) Make approach (C) cheaper: reduce launch overhead and jitter

Approach (C) increases launch count (num_chunks GEMMs + num_chunks collectives per iter).

Research direction:

- Consider HIP Graph capture for the steady-state iteration (same chunk shapes each time).
- Aim: reduce CPU overhead and launch latency jitter.

### 2) Continue chunk-size research, but sweep message size too

You tried num_chunks ∈ {4,8,16,32} and found 8 best.

Next step:

- Keep num_chunks=8 and sweep **M (tokens T)** to see if the optimum shifts with payload size.
- If optimum shifts with payload size, you’re in a mixed regime (latency/overhead vs bandwidth).

### 3) RCCL algorithm/protocol/channel exploration (must verify with logs)

RCCL/NCCL-style stacks can select different algorithms/protocols depending on size/topology.

Research direction:

- Enable init logs to see what it picks:

```bash
export NCCL_DEBUG=INFO
export NCCL_DEBUG_SUBSYS=INIT
```

- Verify selected algorithm/protocol/channels for your message sizes and topology.
- If you change environment knobs, always re-check logs to confirm they took effect.

### 4) CU budgeting methodology (make it systematic)

Instead of manually trying 50/50 splits:

- Build a 2D sweep:
  - comm CUs: 8, 16, 32, 64, …
  - chunk count: 4, 8, 16
- Collect:
  - end-to-end time
  - GEMM TFLOPs
  - AllReduce GB/s (or total AllReduce time)

Then pick the knee points rather than a single “best split” for one case.

## Practical “answers” to key questions

### Is communication still a bottleneck if you overlap with tensor-core GEMM?

Given your ranking (C best, B second) and sensitivity to CU split, AllReduce is **not negligible**. Overlap helps, but the comm portion remains part of the critical path whenever:

- the AllReduce time is comparable to the GEMM chunk time, or
- comm steals enough HBM bandwidth to reduce GEMM throughput under overlap.

### How much compute is needed for AllReduce(sum)?

The arithmetic (adds) is typically small compared to GEMM FLOPs for large K, but AllReduce requires CUs to drive:

- HBM reads+writes of the buffer
- inter-GPU transfer scheduling
- reduction and protocol logic

So practical “compute need” is best answered empirically via the CU-saturation curve described above.

### How many CUs are needed to saturate xGMI AllReduce?

There is no universal constant. For your platform (8× MI350X xGMI), measure:

- AllReduce-only GB/s vs comm CU mask
- xGMI bytes/sec (via counters) vs comm CU mask

The smallest CU mask that yields plateau bandwidth is the “CUs needed to saturate comm” for that payload.

