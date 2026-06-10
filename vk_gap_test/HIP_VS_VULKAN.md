# HIP vs Vulkan: Inter-Kernel Gap on AMD RDNA3 (W7900 / gfx1100)

Investigation into the per-dispatch "inter-kernel gap" (the GPU-idle time between
back-to-back compute kernels) on a single AMD Radeon PRO W7900 (gfx1100, GFX11,
RDNA3), comparing the HIP/ROCm runtime against the Vulkan (RADV) runtime on the
exact same silicon.

All measurements use the on-die fixed-frequency 100 MHz wall clock (10.000 ns/tick)
on both APIs, so reported gaps are real microseconds and are NOT affected by the
variable shader core clock.

--------------------------------------------------------------------------------

## 0. TL;DR (what is actually true)

- The inter-kernel gap is REAL on both APIs. It is wave-drain + cache invalidate +
  command-processor (CP) turnaround between dispatches.
- On the SAME W7900, HIP's per-dispatch period is consistently equal-or-higher
  than Vulkan/RADV's. So the difference is a RUNTIME/API effect, NOT a
  CDNA-vs-RDNA architecture effect.
- The earlier "0.5 us vs 3 us" headline was partly a measurement-reference
  artifact: Vulkan measures TOP_OF_PIPE -> BOTTOM_OF_PIPE (the post-dispatch cache
  flush lands INSIDE "kernel"), while the HIP wave-timeline method measures
  first-wave-start -> last-wave-end (the flush lands INSIDE "gap"). The fair,
  reference-invariant metric is the per-dispatch PERIOD.
- CACHE-FENCE WEIGHT IS RULED OUT as the cause of HIP being slower. Source proof:
  RADV does a FULL L2 writeback+invalidate on every compute SHADER_WRITE->SHADER_READ
  barrier, while default HIP (AGENT scope) does NOT touch L2 at all. HIP does LESS
  cache work yet is slower => the residual cost is in the dispatch/submission path,
  not coherence.

Two earlier hand-waved claims were WRONG and are corrected below:
1. "HIP defaults to system-scope acquire/release fences" -> FALSE. Default is AGENT.
2. "RADV uses a lighter device-scope cache op and keeps L2 resident" -> FALSE. RADV
   does a full GL2 writeback+invalidate on this barrier.

--------------------------------------------------------------------------------

## 1. Methodology

Microbenchmark: one command buffer / one stream records K back-to-back compute
dispatches. Each kernel reads+writes data[gid] (a RAW hazard on an SSBO/global
buffer), so consecutive kernels are genuinely dependent. A barrier (Vulkan) or the
default in-stream ordering (HIP) sits between every pair.

- Vulkan (`vk_gap_test`, `main.cpp`): `vkCmdWriteTimestamp(TOP_OF_PIPE)` before and
  `(BOTTOM_OF_PIPE)` after each dispatch; barrier =
  `VkMemoryBarrier{srcAccess=SHADER_WRITE, dstAccess=SHADER_READ}` with
  COMPUTE_SHADER src/dst stages (`main.cpp:228-256`).
- HIP (`hip_gap_test.cpp`): in-kernel `__builtin_readsteadycounter()` (lowers to
  `s_sendmsg_rtn(GET_REALTIME)` on gfx11) with `atomicMin`/`atomicMax` across all
  blocks to capture first-wave-start (tstart) and last-wave-end (tend) per dispatch.
  Self-calibrated against a `hipEvent` wall timer.

Metrics:
- kernel : tend - tstart (HIP wave busy) / TOP->BOT (Vulkan, includes post-flush).
- gap    : tstart[k+1] - tend[k] (HIP) / BOT->TOP (Vulkan).
- PERIOD : total_span / K  -- reference-point invariant, the fair comparison.

Calibration sanity check: both APIs lock to exactly 10.00 ns/tick = 100 MHz, the
same counter PAL's profiler reports. The wall-clock self-calibration agreeing to 4
digits is strong evidence the GPU-timeline measurement is sound.

--------------------------------------------------------------------------------

## 2. Source facts (audited, with file:line)

### 2.1 HIP/ROCm: default dispatch fence scope is AGENT, not SYSTEM

`AMD_OPT_FLUSH` defaults to 1:

    rocclr/utils/flags.hpp:169
      release(uint, AMD_OPT_FLUSH, 1,
        "Kernel flush option , 0x0 = Use system-scope fence operations."
        "0x1 = Use device-scope fence operations when possible.")

    rocclr/device/device.cpp:1366
      fenceScopeAgent_ = AMD_OPT_FLUSH;

Dispatch packet header is built with AGENT scope when fenceScopeAgent_==1
(GFX12 is the exception: acquire=SYSTEM, release=AGENT):

    rocclr/device/rocm/rocvirtual.cpp:1916-1922
      dispatchPacketHeader_ =
        (... | barrierHBits | (isGfx12 ? sysAcquireAgentReleaseHBits : agentScopeHBits));

What each scope does to caches (runtime's own comment):

    rocclr/device/rocm/rocvirtual.cpp:62-64
      // AGENT acquire  invalidates I, K and L1
      // SYSTEM release invalidates L1, L2 and flushes L2

So a default HIP dispatch invalidates I/K/L1 only and does NOT flush L2.

Escalation to SYSTEM happens only for: SVM prefetch (rocvirtual.cpp:2655),
stream wait/write-value atomics (rocvirtual.cpp:3545,3563), first hidden-heap init
(rocvirtual.cpp:4160), memory copies/blits (rocblit.cpp:397,443,...), HIP-graph
batch where an SDMA follows kernels (hip_graph_internal.cpp:2104), a launch whose
stop timing-event lacks hipEventDisableSystemFence (hip_module.cpp:543-547), or
globally AMD_OPT_FLUSH=0 (rocvirtual.cpp:1913-1919, the else branch).

A separate BARRIER_AND packet (used only at sync points like stream sync / event
wait) DOES default to SYSTEM/SYSTEM and performs the full L2 flush:

    rocclr/device/rocm/rocvirtual.cpp:68-72
      kBarrierPacketHeader = ... | (SYSTEM << ACQUIRE) | (SYSTEM << RELEASE)

No per-dispatch completion signal by default (attach_signal=false ->
completion_signal=0): rocvirtual.cpp:1294-1297, :582-591, hpp:470-471.

Cache op is performed by the CP firmware from the header scope bits, not a software
flush in rocclr/ROCr (rocvirtual.cpp:62-64; amd_aql_queue.cpp:1213-1219 rewrites the
header only to work around old firmware, confirming firmware acts on the bit).

Enum/bit values confirmed in hsa.h: NONE=0, AGENT=1, SYSTEM=2; acquire field bit 9,
release field bit 11 (rocr-runtime .../inc/hsa.h:2859-2932).

### 2.2 RADV (Mesa 25.0.7): the compute barrier does a FULL L2 flush

Dispatch packet on GFX11 is PKT3_DISPATCH_DIRECT:

    src/amd/vulkan/radv_cmd_buffer.c:12062
      radeon_emit(cs, PKT3(PKT3_DISPATCH_DIRECT, 3, predicating) | PKT3_SHADER_TYPE_S(1));

For SHADER_WRITE -> SHADER_READ on a buffer, INV_L2 is set unconditionally on the
write (src) side:

    src/amd/vulkan/radv_cmd_buffer.c:6617-6618
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
    (documented intent at radv_cmd_buffer.c:6577-6579)

dst (read) side adds vector + scalar L0/L1 invalidate:

    src/amd/vulkan/radv_cmd_buffer.c:6722-6726
      if (!pdev->use_llvm && !image) flush_bits |= RADV_CMD_FLAG_INV_SCACHE;
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;

INV_L2 lowers to a full GL2 writeback+invalidate (+ metadata) in one ACQUIRE_MEM:

    src/amd/vulkan/radv_cs.c:163-166
      if (flush_bits & RADV_CMD_FLAG_INV_L2) {
         // Writeback and invalidate everything in L2.
         gcr_cntl |= S_586_GL2_INV(1) | S_586_GL2_WB(1) | (gfx_level < GFX12 ? S_586_GLM_INV(1) | S_586_GLM_WB(1) : 0);

plus a CS_PARTIAL_FLUSH wave drain (radv_cmd_buffer.c:6528 -> radv_cs.c:232-237).
No per-dispatch EOP/signal between dispatches; the only EOP path (cb_db_event) is
not taken for a compute->compute SSBO barrier.

Net PM4 between two dispatches on GFX11:
  1. EVENT_WRITE CS_PARTIAL_FLUSH (drain writer waves)
  2. ACQUIRE_MEM with GCR_CNTL = GL2_WB|GL2_INV|GLM_WB|GLM_INV|GL1_INV|GLV_INV|GLK_INV
     (full L2 writeback+invalidate + L1/L0/scalar invalidate; CP waits for idle)
  3. PKT3_DISPATCH_DIRECT (reader)

--------------------------------------------------------------------------------

## 3. Hardware measurements (W7900, gfx1100, GPU 0)

Note on variance: GPU clock could not be pinned on this part in-container
(perf level reads "unknown"), so absolute us drift run-to-run. Within-session
deltas and the consistent ordering across iterations are the reliable signals.

### 3.1 Controlled scope toggle (same session, spin=0, n=4096)

| HIP scope                              | wave-timeline gap |
|----------------------------------------|-------------------|
| AGENT  (default, AMD_OPT_FLUSH=1)       | ~2.5 us           |
| SYSTEM (AMD_OPT_FLUSH=0, +L2 WB+INV)    | ~4.9 us           |

=> Forcing the L2 flush adds ~2.4 us. HIP's default avoids it. This directly
confirms the source finding (2.1).

### 3.2 Interleaved HIP vs Vulkan (same session, spin=0, n=4096, ordered/barrier)

| iter | Vulkan period (BOT->BOT) | HIP period (span/K) |
|------|--------------------------|----------------------|
| 1    | 3.28 us                  | 3.67 us              |
| 2    | 3.52 us                  | 5.60 us              |
| 3    | 3.44 us                  | 5.60 us              |

HIP per-dispatch period is equal-or-higher in every iteration, despite HIP doing
LESS cache work (no L2 flush) than RADV (full L2 flush).

### 3.3 Reference-point decomposition (illustrative single run, spin=0, n=4096)

    Vulkan (RADV):  kernel TOP->BOT 2.16 us ; gap BOT->TOP 0.64 us ; period 2.79 us
    HIP (wave):     kernel busy     1.35 us ; gap wave-end->start 3.08 us
    HIP hipEvent CP-boundary pass: DISCARDED -- hipEventRecord per kernel is a heavy
       system-scope barrier packet that ~tripled the period (artifact, not a clean
       analog of cheap Vulkan timestamps).

--------------------------------------------------------------------------------

## 4. Conclusion

1. The inter-kernel gap is real on both APIs (wave drain + cache invalidate + CP
   turnaround).
2. On identical RDNA3 silicon, HIP's per-dispatch period >= Vulkan/RADV's. The
   difference is a runtime/API property, not architecture.
3. Cache-coherence cost is NOT the explanation: RADV flushes L2 fully on every
   compute barrier while default HIP (AGENT) does not touch L2, yet HIP is slower.
4. The residual HIP overhead therefore lives in the dispatch/submission path
   (AQL kernel-dispatch packets via the CP HSA soft-queue + the ordered-stream
   AQL "barrier" bit forcing per-packet serialization) rather than cache fences.

OPEN / TO VERIFY (isolation test, see section 5): quantify and attribute the
dispatch-path cost by (a) launching across multiple independent streams and
(b) clearing the AQL barrier bit (any-order), to see if the HIP period collapses
toward Vulkan's.

--------------------------------------------------------------------------------

## 5. Isolation test (isolated container + HIP built from source)

Setup: a fresh container `hipvk-isolated-sshliapn` (same image/devices, separate
env). The rocm-systems monorepo was checked out to tag `rocm-7.2.1` to match the
installed runtime, and `libamdhip64.so` was built from source
(`clr/build-gap`, HIP 7.2.53211). It loads against the installed ROCr and
calibrates to 10.00 ns/tick - ABI compatible.

A one-line env-gated patch in `rocclr/device/rocm/rocvirtual.cpp` (after
`aqlHeader_ = dispatchPacketHeader_;`) allows toggling the dispatch packet header:
- `GAP_NOSCOPE=1`   -> strip acquire/release fence scope (NONE): no per-dispatch
                       cache acquire/release at all.
- `GAP_NOBARRIER=1` -> clear the AQL barrier bit.

### 5.1 CPU launch overhead is NOT the cause (HIP Graph == CPU launch loop)

A recorded Vulkan command buffer pays no per-dispatch CPU cost. The HIP analog is
a hipGraph (record once, replay). Measured (spin=0, n=4096):
  HIP graph period      ~= 5.5 us
  HIP CPU launch loop   ~= 5.6 us
Identical. So the gap is a real GPU-side per-dispatch cost, not host launch cost.

### 5.2 Concurrency: single HSA queue serializes; multiple queues overlap

Round-robin across N independent streams (each its own buffer), spin=0 n=4096:
  nstreams=1  -> period 3.97 us, concurrency 0.31x
  nstreams=2  -> period 2.84 us, concurrency 0.45x
  nstreams=4  -> period 2.39 us, concurrency 0.57x
  nstreams=8  -> period 2.38 us, concurrency 0.82x
  nstreams=16 -> period 2.41 us, concurrency 0.76x
The CP can overlap dispatches across queues down to a ~2.4 us floor. A single
queue cannot - it processes packets serially.

### 5.3 Same-session A/B (custom HIP), spin=0 n=4096 K=4000 - REPRODUCIBLE

| dispatch header variant                  | period (span/K) |
|------------------------------------------|-----------------|
| default (barrier bit + AGENT scope)       | 5.2 - 5.6 us    |
| GAP_NOSCOPE (no per-dispatch cache fence) | 3.94 us         |
| GAP_NOBARRIER (barrier bit cleared)       | 6.22 us         |
| BOTH                                      | 4.75 us         |

Conclusions (these are clock-robust: both arms are the same binary back-to-back):
- The per-dispatch AGENT cache fence (I/K/L1 invalidate + acquire/release) costs
  ~1.2-1.6 us. Removing it: 5.2 -> 3.94 us.
- The AQL barrier bit is NOT the serializer: clearing it does not reduce the
  period (it is slightly worse, and concurrency stays 0.21x = still no overlap).
  The CP serializes the single queue regardless of the bit.
- Floor with all cache work and the barrier bit removed: still ~3.9 us per
  dispatch in a single queue = pure CP AQL packet-processing + launch latency.

### 5.4 Cross-API, same session: Vulkan (full L2 flush) vs HIP-NOSCOPE (no cache)

| iter | Vulkan (full L2 flush) | HIP NOSCOPE (no cache work) |
|------|------------------------|------------------------------|
| 1    | 3.08 us                | 2.92 us                      |
| 2    | 2.80 us                | 4.02 us                      |
| 3    | 3.44 us                | 4.02 us                      |

Once HIP's per-dispatch cache fence is removed, HIP's single-queue dispatch floor
(~2.9-4.0 us) lands in the SAME range as Vulkan's full-L2-flush period
(~2.8-3.4 us). The two are comparable to within the measurement noise; in iter 1
HIP is actually lower. Because GPU clock management is broken in this environment
(perf level "unknown", driver reports clock tables EMPTY / low-power state),
absolute cross-API differences at this ~1 us scale are NOT reliably resolvable,
and this is stated as a limitation rather than papered over.

### 5.5 Did we try to "fix" the barrier bit in source? Yes - three ways, all null

The AQL barrier bit was attacked three independent ways:
1. hipExtLaunchKernel(hipExtAnyOrderLaunch) - the public API that clears the bit.
2. GAP_NOBARRIER source patch - clears the bit on dispatchPacketHeader_ directly.
3. Independent buffers per kernel (hip_gap_indep), so the memory-dependency
   tracker auto-selects the no-barrier "nosync" header (rocvirtual.cpp:764-767).

Single stream, stock HIP, spin=0 n=4096:
  buffers=1  (dependent, sync hdr, barrier bit) -> period 4.25 us, concurrency 0.31x
  buffers=2  (independent, nosync hdr, NO bit)  -> period 5.56 us, concurrency 0.24x
  buffers=64 (independent, nosync hdr, NO bit)  -> period 5.57 us, concurrency 0.24x

In ALL cases clearing/avoiding the barrier bit did NOT enable overlap and did NOT
reduce the period (it was equal or slightly worse). Root cause: a single HSA queue
is processed serially by the CP one dispatch at a time; the barrier bit only adds
"wait for prior to complete" on top. Overlap requires MULTIPLE queues/streams
(section 5.2: 2.4 us floor, concurrency 0.82x). So the barrier bit is not a bug to
fix - it is not the bottleneck. The real levers are (a) per-dispatch fence scope
(~1.4 us, section 5.3) and (b) number of streams (concurrency).

--------------------------------------------------------------------------------

## 6. Final conclusion (revised by the isolation data)

Reliable, same-session, reproducible findings:

1. Default HIP per-dispatch period (~5.2-5.6 us) decomposes into:
   - ~1.2-1.6 us  per-dispatch AGENT cache fence (I/K/L1 invalidate + acq/rel)
   - ~3.9 us      CP AQL packet-processing + launch floor (single queue)
2. The biggest HIP-specific, controllable per-dispatch cost is the AGENT cache
   fence (~1.4 us). Stripping it brings HIP into the same range as Vulkan.
3. The AQL barrier bit is NOT a serialization cost (clearing it does not help).
4. CPU launch overhead is NOT the cause (HIP graph == CPU launch loop).
5. A single HSA queue serializes dispatches; multiple queues overlap to a ~2.4 us
   floor - so the serial per-dispatch cost is largely hideable with concurrency.

Ruled out as explanations for "HIP gap > Vulkan gap":
- Architecture (CDNA vs RDNA): same W7900 silicon for both.
- Cache-fence weight being heavier in HIP: FALSE - Vulkan does a full L2
  writeback+invalidate per compute barrier (source 2.2), while default HIP (AGENT)
  does not touch L2 at all (source 2.1). HIP does LESS cache work.
- The AQL barrier bit: ruled out (5.3).
- CPU launch overhead: ruled out (5.1).

What remains, stated honestly:
- HIP's per-dispatch AGENT cache fence (~1.4 us) is a real, measured cost that
  Vulkan's path does not pay in the same per-dispatch manner. This is the largest
  reliably-attributable contributor to HIP's higher default period.
- Any residual dispatch-floor difference between HIP (AQL via the CP HSA queue)
  and Vulkan (prebuilt PM4 DISPATCH_DIRECT stream) is within this environment's
  clock noise (~1 us) and is NOT claimed as a definitive number.

--------------------------------------------------------------------------------

## 6b. DEFINITIVE results at PINNED clock (supersede noisy section 5.4)

The W7900 autosuspend was the noise source. Pinning the clock with
`gpu_pin_freq.sh` (profile_standard -> sclk 973 MHz, mclk 1124 MHz,
runtime_status=active) made every number reproducible to <1% across 5 runs.

Pinned 973 MHz, spin=0 n=4096, single stream, kernel busy = 2.06 us:

| configuration                                | period   | gap = period-busy |
|----------------------------------------------|----------|-------------------|
| Vulkan (RADV, full L2 writeback+invalidate)  | 4.96 us  | 2.90 us           |
| HIP default (barrier bit + AGENT fence)      | 7.94 us  | 5.88 us           |
| HIP GAP_NOSCOPE (no per-dispatch cache fence)| 5.59 us  | 3.53 us           |
| HIP GAP_NOBARRIER (barrier bit cleared)      | 7.52 us  | -                 |
| HIP BOTH (no fence + no barrier bit)         | 4.93 us  | 2.87 us           |

Multi-stream (pinned): 1->7.94, 2->4.03, 4->4.17, 8->3.35, 16->2.94 us
  (concurrency rises to 0.81x at 16 streams).
HIP graph 7.62 us ~= CPU launch loop 7.94 us (CPU launch overhead negligible).

Exact accounting of the HIP-vs-Vulkan per-dispatch difference:
  AGENT cache fence  = 7.94 - 5.59 = 2.35 us
  AQL barrier bit    = 5.59 - 4.93 = 0.66 us   (also default-nobarrier = 0.42 us)
  HIP stripped (4.93) ~= Vulkan (4.96)
  HIP default - Vulkan = 7.94 - 4.96 = 2.98 us = fence + barrier bit

CONCLUSION (now clock-clean, no noise hedge):
- The AQL dispatch engine is NOT inherently slower than PM4. With HIP's default
  per-dispatch AGENT fence and barrier bit removed, HIP single-queue dispatch
  (4.93 us) matches Vulkan's full period (4.96 us, which includes an L2 flush).
- The ENTIRE HIP > Vulkan per-dispatch gap (~3.0 us here) is HIP's default
  per-dispatch AGENT cache fence (~2.35 us) + AQL barrier-bit serialization (~0.66 us).
- Open puzzle (flagged, not hand-waved): Vulkan does MORE cache work (full L2 WB+INV)
  yet costs ~the same as HIP with NO cache work, while HIP's L1-only AGENT fence
  costs 2.35 us. This indicates the cost is the per-AQL-packet CP acquire/release
  serialization stall, not cache-traffic volume (working set is 16 KB). Precise
  isolation is firmware-level and not resolved here.

--------------------------------------------------------------------------------

## 6c. Is Vulkan actually flushing, or skipping it? (the "are you sure" check)

Challenge: maybe RADV is cheaper because it secretly SKIPS the flush (does less),
not because the mechanism is better. Checked three ways; the answer is NO, RADV
really flushes (and in fact does MORE cache work than HIP).

### 6c.1 Source: the L2 flush is unconditional on the producer side

For a global VkMemoryBarrier (image == NULL), image_is_coherent is hard-false, so
INV_L2 is always set on the write/src side:

    src/amd/vulkan/radv_cmd_buffer.c:6592   image_is_coherent = image ? ... : false;
    src/amd/vulkan/radv_cmd_buffer.c:6617-6618
        if (!image_is_coherent) flush_bits |= RADV_CMD_FLAG_INV_L2;

The can_skip_buffer_l2_flushes() optimization (radv_cmd_buffer.c:6546-6551) that
could suppress this is applied ONLY on the dst/read side (line 6684), NOT here. So
RADV emits the GL2 writeback+invalidate every time. It does not skip it.

### 6c.2 Empirical: the barrier does real, data-proportional work (pinned 973 MHz)

Barrier on vs off, spin=0 n=4096:

    VK barrier=1 : period 4.96 us
    VK barrier=0 : period ~1.0 us (median)

A no-op'd barrier would leave both at ~1 us. The barrier adds ~4 us of real
flush+drain. And the flush SCALES with the dirtied footprint (a skipped flush
could not):

| n (dirtied)     | VK period (b=1) | VK kernel TOP->BOT | HIP period | HIP shader busy |
|-----------------|-----------------|--------------------|------------|-----------------|
| 4096   (16 KB)  | 4.96 us         | 4.03 us            | 7.88 us    | 2.08 us         |
| 65536  (256 KB) | 5.10 us         | 4.19 us            | 9.35 us    | 3.54 us         |
| 262144 (1 MB)   | 7.98 us         | 7.06 us            | 17.99 us   | 12.19 us        |
| 1048576 (4 MB)  | 19.13 us        | 18.21 us           | 52.23 us   | 46.43 us        |

The decisive observation - per-dispatch OVERHEAD (period minus busy/window):
  HIP gap = period - shader_busy = 5.80, 5.80, 5.80, 5.81 us  -> FLAT (size-invariant)
  VK  gap = period - TOP->BOT     = 0.93, 0.91, 0.93, 0.92 us  -> FLAT (CP turnaround)

HIP's per-dispatch overhead is a FIXED ~5.8 us tax that does NOT scale with the
working set => it is a protocol/latency cost, not cache-traffic volume. Vulkan's
fixed tail is only ~0.92 us; its real L2 flush is folded INTO the dispatch window
(TOP->BOT) and scales there with dirty bytes. So Vulkan is doing the (heavier)
flush; it just overlaps it instead of paying a fixed serialized round-trip.

### 6c.3 ACQUIRE_MEM vs AGENT acquire/release - why the SAME logical op differs

Both must do the same three things between dependent kernels:
(A) drain the producer's waves, (B) make the producer's stores visible at the
device coherence point (GL2), (C) invalidate the consumer's L0/L1 so it re-reads.
The cost difference is HOW each runtime expresses that, not WHAT is required.

VULKAN / RADV = pre-built PM4 stream, single hardware cache-rinse:
  1. PKT3 DISPATCH_DIRECT (writer N)          radv_cmd_buffer.c:12062
  2. EVENT_WRITE CS_PARTIAL_FLUSH             radv_cs.c:232-237   (drain N's waves)
  3. PKT3 ACQUIRE_MEM with GCR_CNTL=          radv_cs.c:163-166,332-339
       GL2_WB|GL2_INV|GLM_WB|GLM_INV|GL1_INV|GLV_INV|GLK_INV
       "cache flush is executed in the ME, but the PFP waits for completion"
       (radv_cs.c:328-330) -- ONE inline Graphics Cache Rinse over only DIRTY
       lines; for 16 KB this is a few cache lines = near-instant.
  4. PKT3 DISPATCH_DIRECT (reader N+1)
  The CP front-end (PFP=PreFetch Parser) has already pre-parsed N+1's register/
  user-data packets while N ran; the only true stall is "wait for GCR idle".
  => overhead = wave drain + tiny GCR wait + ~0.9 us turnaround.

HIP / ROCm = interpreted AQL packets, per-packet firmware fence:
  1. host writes an AQL kernel_dispatch_packet into the user HSA queue ring,
     header = KERNEL_DISPATCH | barrier-bit | acquire=AGENT | release=AGENT
     (rocvirtual.cpp:1916-1922), rings doorbell.
  2. CP AQL packet processor (MEC firmware) reads packet N, decodes it, does the
     ACQUIRE fence at AGENT scope (invalidate I/K/L1, rocvirtual.cpp:62-64).
  3. MEC translates to an internal dispatch; SPI distributes N's workgroups.
  4. barrier-bit on N+1 => MEC must, in strict SERIAL order:
       4a. wait for N's grid to fully retire (wave drain)   -- same as Vulkan
       4b. perform N's RELEASE fence at AGENT scope (make stores visible at GL2)
       4c. advance read_index, fetch packet N+1 from the ring, decode it
       4d. perform N+1's ACQUIRE fence (invalidate I/K/L1)
       4e. dispatch N+1 to SPI
  Steps 4b-4e are a serialized microcode round-trip the CP does NOT overlap with
  the next packet's setup (the AQL model treats each packet boundary as a hard
  ordering point; the barrier bit forbids running ahead). That fixed round-trip
  is the measured ~2.35 us (fence) + ~0.66 us (barrier bit) on top of the same
  wave-drain floor.

WHY THE SAME WORK COSTS DIFFERENTLY:
- PM4 is pre-translated and prefetched: the next dispatch's setup overlaps the
  current dispatch, and the cache op is a single inline hardware GCR instruction.
  The flush latency is mostly HIDDEN.
- AQL is interpreted per packet: the CP firmware serially does decode -> acquire
  -> dispatch -> drain -> release for EVERY kernel, and cannot prefetch the next
  packet past the barrier. The fence is a firmware-orchestrated round-trip, not a
  cheap inline instruction, so it is EXPOSED as a fixed ~3 us tax.
- Proof it is mechanism, not cache volume: HIP with the fence stripped (4.93 us)
  == Vulkan WITH a full L2 flush (4.96 us); and HIP's gap is size-invariant
  (6c.2) while a 16 KB working set is far too small for 2.35 us of cache traffic.

CAVEAT (honesty): the MEC/AQL microcode is closed firmware. The per-step 4a-4e
attribution is a model-level explanation consistent with every measurement
(NOSCOPE removes exactly the 2.35 us acquire/release; NOBARRIER removes 0.66 us;
gap is size-invariant), but the exact firmware micro-ops are inferred, not traced.

IMPLICATION FOR LLM DECODE: each dependent kernel pays the fixed ~3 us HIP tax
regardless of tensor size. At decode (many small sequential dependent launches)
this dominates, so kernel FUSION (fewer dispatches) and multi-stream concurrency
(section 5.2/6b: ~2.4 us floor) pay off far more under HIP than under Vulkan.

### 6c.4 Granular per-step dispatch timelines (N -> N+1 transition)

Both paths must accomplish the SAME three things between two dependent kernels:
  (A) drain the writer's waves, (B) make the writer's stores visible at GL2 (the
  device coherence point), (C) invalidate the reader's L0/L1 so it re-reads.
Only the EXPRESSION differs. CP stages referenced: PFP = PreFetch Parser (front of
the graphics CP), ME = MicroEngine, MEC = MicroEngine Compute (runs the HSA/AQL
soft-queue), SPI = Shader Processor Input (workgroup distributor).

VULKAN / RADV -- a pre-compiled, prefetched PM4 stream + one hardware cache-rinse:

  1) Dispatch N  (PKT3_DISPATCH_DIRECT, radv_cmd_buffer.c:12062)
     1.0) PFP reads the packet; ME programs COMPUTE_DISPATCH registers
     1.1) SPI distributes N's workgroups to the WGPs
     1.2) waves run; stores flow L0 -> GL1 -> GL2
  2) Barrier  (two PM4 packets)
     2.0) EVENT_WRITE CS_PARTIAL_FLUSH (radv_cs.c:232-237): CP stops new work and
          waits until ALL of N's waves retire  <-- the unavoidable wave-drain, the
          bulk of the gap
     2.1) ACQUIRE_MEM, GCR_CNTL = GL2_WB|GL2_INV|GL1_INV|GLV_INV|GLK_INV
          (radv_cs.c:163-166, 332-339): ME fires ONE Graphics Cache Rinse, PFP
          waits for the idle signal ("cache flush executed in the ME, but the PFP
          waits for completion", radv_cs.c:328-330). The GCR walks only DIRTY
          lines -> for 16 KB this is a few cache lines, near-instant.
  3) Dispatch N+1  (PKT3_DISPATCH_DIRECT)
     3.0) the stream is flat and pre-built, so the PFP has ALREADY pre-parsed
          N+1's register/user-data packets while N ran; launch was only gated by
          "GCR idle"
     3.1) SPI distributes N+1; its waves read the now-coherent data from GL2
  => gap = wave-drain + tiny GCR wait + ~0.9 us turnaround. The L2 flush is mostly
     HIDDEN behind the already-prefetched next dispatch.

HIP / ROCm -- interpreted AQL packets + a per-packet firmware fence:

  1) Launch N
     1.0) host writes an AQL kernel_dispatch_packet into the user-mode HSA queue
          ring; header = KERNEL_DISPATCH | barrier-bit | acquire=AGENT |
          release=AGENT (rocvirtual.cpp:1916-1922); rings the doorbell
     1.1) MEC reads packet N from the ring, decodes it, reads the kernarg pointer
     1.2) MEC performs N's ACQUIRE fence at AGENT scope = invalidate I$/K$/L1
          (rocvirtual.cpp:62-64)
     1.3) MEC translates to an internal dispatch; SPI distributes N's workgroups;
          waves run; stores L0 -> GL1 -> GL2
  2) Transition N -> N+1  (N+1 has the barrier bit, so MEC does this STRICTLY
     SERIAL, one micro-step at a time, with no run-ahead):
     2.0) wait for N's grid to fully retire (wave-drain)  <-- same cost as Vulkan
     2.1) perform N's RELEASE fence at AGENT scope (push stores to GL2 + visibility
          barrier)  -- serialized firmware step
     2.2) advance read_index, fetch packet N+1 from the ring, DECODE it
     2.3) perform N+1's ACQUIRE fence (invalidate I$/K$/L1)
     2.4) only now dispatch N+1 to SPI
  => steps 2.1-2.4 are a serialized microcode round-trip the CP does NOT overlap
     with the next packet's setup. Measured: ~2.35 us (fence) + ~0.66 us (barrier
     bit) on top of the same wave-drain floor.

WHY THE SAME WORK COSTS DIFFERENTLY:
  PM4 is pre-translated and prefetched -> next dispatch's setup overlaps the
  current one, and the cache op is a single inline hardware GCR -> flush latency is
  HIDDEN. AQL is interpreted per packet -> the CP firmware serially does
  decode -> acquire -> dispatch -> drain -> release for EVERY kernel and cannot
  prefetch past the barrier -> the fence is EXPOSED as a fixed ~3 us tax.
  Proof it is mechanism, not cache volume: HIP fence-stripped (4.93 us) == Vulkan
  WITH a full L2 flush (4.96 us), and the HIP gap is size-invariant (6c.2).

--------------------------------------------------------------------------------

## 6d. PROOF: driving the SAME GPU via the PM4 front-end is ~2x faster than AQL

The preceding sections argued, from source + microbenchmarks, that HIP's extra
per-dispatch cost is the AQL packet/acquire-release MECHANISM, not the dispatch
engine or cache work. To prove it, we drove the SAME W7900 through a RAW KFD PM4
compute queue (HSA_QUEUE_COMPUTE) - the same PM4 front-end (PFP/ME) RADV uses -
bypassing the HSA/AQL soft-queue that HIP/ROCr dispatch through.

### 6d.1 What was built (artifact: pm4_gap/)

A standalone program (pm4_gap.cpp) that links libhsakmt directly and:
- selects the gfx1100 node, allocates GPU memory via hsaKmtAllocMemory,
- creates a PM4 compute queue (hsaKmtCreateQueue, HSA_QUEUE_COMPUTE),
- loads a tiny offline-assembled gfx1100 shader (gap_kernel.s: data[0]++ with
  L2-coherent glc flat access -> a true per-dispatch RAW dependency),
- builds ONE linear PM4 stream in the ring: one-time COMPUTE_* register setup,
  then K x [ DISPATCH_DIRECT (+ EVENT_WRITE CS_PARTIAL_FLUSH (+ ACQUIRE_MEM)) ],
  a final CS_PARTIAL_FLUSH and a WRITE_DATA sentinel,
- rings the 64-bit doorbell ONCE and busy-polls the sentinel; period = wall/K.
The PM4 sequence (DISPATCH_DIRECT + CS_PARTIAL_FLUSH + ACQUIRE_MEM) is exactly
RADV's compute-barrier recipe (section 2.2). data[0]==K after the run proves the
chain truly serialized (each dispatch saw the previous increment).

### 6d.2 Results (pinned 973 MHz, trivial 1-thread kernel so period ~= overhead)

| path / fence                                  | period   | chain OK | notes |
|-----------------------------------------------|----------|----------|-------|
| PM4, no fence (dispatch-only)                  | 0.33 us  | NO (393) | raw issue rate; dispatches overlap+race |
| PM4, CS_PARTIAL_FLUSH only (drain)             | 2.53 us  | yes      | wave-drain serialize, no cache flush |
| PM4, CS_PARTIAL_FLUSH + ACQUIRE_MEM (RADV-like)| 3.93 us  | yes      | full GL2 WB+INV + L1/L0/scalar inv |
| HIP AQL, default (trivial kernel n=256)        | 7.88 us  | yes      | kernel busy 1.92 us -> ~5.96 us overhead |

Apples-to-apples, SAME silicon, SAME trivial dependent-increment workload:
  HIP AQL per dispatch     = 7.88 us
  raw PM4 per dispatch     = 3.93 us   (and PM4 does MORE cache work: full L2 WB+INV)
  => ~2x reduction purely from changing the submission path AQL -> PM4.

PM4 overhead decomposition (clock-clean, stable to <1% over 3 runs):
  raw dispatch issue (overlapped) .......... 0.33 us
  + wave drain (CS_PARTIAL_FLUSH) .......... 2.53 us   (drain/serialize = +2.20)
  + full L2 cache flush (ACQUIRE_MEM) ...... 3.93 us   (cache flush     = +1.42)

### 6d.3 Conclusion

This is the direct confirmation of the whole investigation:
- The GPU dispatch engine and cache coherence are NOT the bottleneck. The PM4
  front-end on this exact W7900, doing a FULL L2 writeback+invalidate per
  dependent dispatch, costs ~3.93 us - LOWER than Vulkan's measured 4.96 us
  (Vulkan's includes a real ~2 us kernel) and ~2x LOWER than HIP's AQL 7.88 us.
- HIP's ~3-4 us extra per-dispatch tax is the AQL mechanism: per-packet
  decode + AGENT acquire/release enforced serially by the CP soft-queue
  microcode, which cannot be prefetched/pipelined the way a flat PM4 stream is
  (sections 6c.3 / 6c.4). Removing it from HIP (GAP_NOSCOPE+NOBARRIER, 4.93 us)
  and driving raw PM4 (3.93 us) both land in the PM4/Vulkan range.
- "Leverage the PM4 front-end from HIP" is therefore demonstrably worth it: a
  PM4 submission path retains full coherence yet halves the per-dispatch period,
  which for sequential LLM-decode kernels directly cuts the inter-kernel gap.

CAVEATS: the PM4 kernel is a 1-thread increment (near-zero compute) so its
period is almost pure overhead; the HIP AQL number includes ~1.9 us of real
kernel busy, so the engine-overhead gap (PM4 3.93 vs AQL ~5.96) is the precise
comparison. The PM4 queue runs on the MEC; RADV's universal-queue compute may
use the GFX PFP/ME, but both reach the same ~4 us range, confirming the result
is path-driven, not micro-engine-specific. This is a research prototype: it
bypasses ROCr scheduling/signals and is not a drop-in hipLaunchKernel.

--------------------------------------------------------------------------------

## 6e. PM4 dispatch of a REAL hipcc kernel (ABI replication validated)

The 6d prototype used a hand-assembled shader with the kfdtest USER_DATA ABI
(args poked straight into SGPRs). A real HIP kernel uses the full AMDHSA COV5
ABI: the kernel reads its arguments through a kernarg-segment pointer passed in
a USER_SGPR, and the CP sets up those USER_SGPRs from the 64-byte kernel
descriptor (KD). To drive a real decode through PM4 we must replicate exactly
what the CP's AQL-to-hardware translation does. pm4_real.cpp proves we can:

  1. load the ELF code object PT_LOAD segments into one GPU exec buffer at their
     virtual addresses (so KD -> kernel_code_entry_byte_offset stays valid),
  2. read the KD (kernel_descriptor_t, 64 B): kernarg_size, entry offset,
     compute_pgm_rsrc1/rsrc2/rsrc3, kernel_code_properties (enable bits),
  3. COMPUTE_PGM_LO/HI = (kdVA + entry_offset) >> 8,
  4. COMPUTE_PGM_RSRC1/RSRC2 copied VERBATIM from the KD -- never hand-decoded,
     so they are byte-identical to what the CP would load,
  5. USER_DATA SGPRs filled in kernel_code_properties enable-bit order. For the
     test kernel only ENABLE_SGPR_KERNARG_SEGMENT_PTR (bit 3) is set, so the
     kernarg pointer goes in s[0:1] (USER_DATA_0/1); no scratch, no dispatch_ptr,
     no queue_ptr.
  6. kernarg segment holds the explicit args (here one int* counter).

Test kernel KD (extern "C" __global__ void inc_kernel(int*): *c = *c + 1):
  kernarg=8  entry_off=0x10c0  rsrc1=0xe0af0000  rsrc2=0x0000009e
  rsrc3=0x00000010  props=0x0408 (KERNARG_SEGMENT_PTR | WAVEFRONT_SIZE32)

Result (W7900 / gfx1100, profile_standard ~ unpinned, K=5000 dependent chain):

  fence mode                         data       period (us)
  -------------------------------    --------   -----------
  0  dispatch-only (no fence)        2  (race)   0.171
  1  CS_PARTIAL_FLUSH only           8  (race)   0.654
  2  pf + ACQUIRE_MEM (RADV-like)    5000 (OK)   3.521

So a REAL ROCm kernel, PM4-dispatched with the RADV-like full-L2 flush, both
serializes correctly (counter == K) and lands at ~3.5 us/dispatch -- the same
PM4 range as the hand-asm shader (6d) and ~half of HIP's AQL ~7.9 us for the
same trivial dependent kernel. This retires the kernel-ABI risk for routing a
real decode through a PM4 path: copying rsrc1/rsrc2 verbatim and placing
USER_DATA by enable-bit order is sufficient for no-scratch kernels.

What is NOT yet solved (the hard parts of a drop-in HIP/CLR integration, which
are pure correctness plumbing and independent of the gap mechanism):
- completion-signal wakeup: PM4 RELEASE_MEM can write the hsa_signal value, but
  waking an interrupt-blocked HIP waiter (hsa_signal_wait fallback) needs the
  mailbox+interrupt EOP replicated, else streamSynchronize can stall/deadlock;
- cross-queue ordering: kernels on the PM4 queue vs copies/barriers/events still
  on the ROCr AQL queue need a shared-signal handshake at every crossing;
- scratch kernels (private_segment_fixed_size>0 / FLAT_SCRATCH_INIT / dynamic
  stack): need a per-queue scratch buffer + V# + flat_scratch_init; until then
  such kernels must fall back to the normal AQL path.

Artifact: pm4_gap/pm4_real.cpp (+ realkern.hip -> k_gfx1100.co via
clang-offload-bundler --unbundle). Build/run in hipvk-isolated container.

--------------------------------------------------------------------------------

## 6f. Decode-layer verification: PM4 does NOT help REAL kernels (REVERSES 6d/6e)

Before modifying the HIP runtime, we emulated a real decode layer in the
standalone PM4 harness: THREE distinct, dependent hipcc kernels chained N times
(pm4_layer.cpp vs hip_layer.cpp launching the SAME code object). Two chains:
  - "rms": k_rms (1-workgroup LDS reduction + __syncthreads) -> k_scale -> k_add
  - "elem": all-parallel elementwise (full-grid) dependent chain on x
Both produce BIT-IDENTICAL checksums between PM4 and HIP (correctness verified),
so the PM4 ABI replication (6e) is sound even with LDS, barriers and multi-block.

Per-iteration latency (3 kernels), all-elementwise dependent chain, W7900
PINNED clocks (sclk 971 / mclk 1124, so NOT a downclock artifact):

  M (elems)   PM4 (CS_PARTIAL_FLUSH+ACQUIRE_MEM)   HIP/AQL      PM4/HIP
  ---------   ----------------------------------   --------     -------
  4096          12.2 us                             11.8 us      ~1.0x (tie)
  65536         72.3 us                             12.2 us      5.9x SLOWER
  262144       271.9 us                             16.6 us     16.4x SLOWER

HIP stays nearly FLAT while PM4 grows LINEARLY with working set. Decomposition
(M=65536, unpinned, AGENT-scope fence):
  - PM4 FENCE=0 (no drain):          61 us/iter   == HIP (55 us)  <- front-end is fine
  - PM4 FENCE=1 (CS_PARTIAL_FLUSH):  571 us/iter  <- the wave-drain barrier is the cost
  - PM4 FENCE=2 (+ACQUIRE_MEM):      580 us/iter  <- L2 flush scope adds nothing
  - cache scope (RADV full-L2 vs AGENT L0/L1): identical -> NOT a cache-flush issue

MECHANISM: HIP/ROCr's AQL acquire/release PIPELINES dependent dispatches -- the
next kernel's waves launch as the previous retire (in-order, cache ops inline),
keeping HBM saturated, so the chain runs at ~memory-bandwidth cost. The explicit
PM4 CS_PARTIAL_FLUSH is a CP-stalling full-idle barrier: drain -> flush ->
relaunch, creating a GPU bubble every dispatch that exposes the full per-dispatch
launch+drain latency with zero overlap. For real kernels this dominates.

CONCLUSION (the important one): the PM4 front-end win measured in 6d/6e was an
ARTIFACT of near-zero-work kernels, where AQL's per-packet overhead dominated and
CS_PARTIAL_FLUSH drained instantly. For REAL decode kernels with real memory
traffic, HIP/AQL is already at the efficient frontier (saturates bandwidth on a
dependent chain) and the RADV-style PM4 path is 6-16x WORSE. The same caveat
applies to the original "HIP gap is ~2x Vulkan" result: it was measured on
trivial shaders (gap.comp / hip_gap), so it reflects per-dispatch fixed overhead,
NOT the cost that matters for real decode. Therefore routing real decode through
a PM4 dispatch path is NOT worth pursuing -- it would regress throughput. We do
NOT proceed with the CLR/HIP-runtime PM4 modification. The standalone decode-
layer emulation was the cheap, safe way to learn this before paying for the
runtime change (and a GPU-hang-prone integration).

Open micro-question (not blocking the conclusion): whether a lighter PM4
serialization (e.g. RELEASE_MEM+WAIT_REG_MEM, or relying on in-order completion
without a full CS_PARTIAL_FLUSH) could match HIP. Even if it could, it would only
TIE HIP, not beat it -- so there is no decode upside to the PM4 path.

Artifacts: pm4_gap/pm4_layer.cpp, pm4_gap/hip_layer.cpp, pm4_gap/layer_kernels.hip
(k_rms/k_scale/k_add -> layer_gfx1100.co). Env: CHAIN=elem|rms, FENCE=0|1|2,
GCR_MODE=0(radv)|1(agent), DBG=1, DBG_ONLY=rms|scale|add.

--------------------------------------------------------------------------------

## 6g. THREE-WAY decode-layer chain: Vulkan/RADV vs HIP/AQL vs raw PM4

The missing leg. Section 6f compared raw-KFD PM4 against HIP/AQL on the real
dependent decode chain and found PM4 6-16x slower. But RADV emits the SAME
CS_PARTIAL_FLUSH + full-L2-flush barrier (section 2.2), so the obvious question
is: does the ACTUAL Vulkan path blow up the same way the hand-rolled PM4 path
did, or does RADV serialize more cheaply? This section runs the identical chain
through real Vulkan compute pipelines and settles it.

Harness: vk_layer.cpp (+ layer_add.comp / layer_scale.comp). It runs the EXACT
"elem" chain (x += w; x *= w; x += w) N=1000 times on one dedicated compute
queue (ACE), with a SHADER_WRITE->SHADER_READ VkMemoryBarrier between every
dispatch -- the same barrier ggml-vulkan emits between dependent ops, which RADV
lowers to CS_PARTIAL_FLUSH + GL2 INV/WB. Same init values as pm4/hip_layer, so
the result is checksum-comparable. Buffers are DEVICE_LOCAL + host-visible
(ReBAR) for init/readback. CPU wall time of one submission of all 3*N dispatches.

Correctness: vk_layer, hip_layer and pm4_layer all produce the BIT-IDENTICAL
checksum 42020575.025146 at M=4096 (and matching values at every M), so the
three front-ends run the same computation.

Per-iteration latency (3 kernels), all-elementwise dependent chain, W7900,
ALL THREE under the SAME pinned clock (profile_standard: sclk 572 / mclk 1124):

  M (elems)   Vulkan/RADV (barrier)   HIP/AQL    raw PM4 (CS_PARTIAL_FLUSH)
  ---------   ---------------------   -------    --------------------------
  4096          21.05 us               22.92 us    11.38 us
  65536         22.33 us               23.27 us    66.68 us
  262144        32.87 us               33.75 us   311.96 us

  Vulkan front-end only (barrier=0, no sync -- ref, checksum invalid):
  4096 17.76 us ; 65536 18.15 us ; 262144 21.58 us

THE KEY RESULT: Vulkan/RADV stays ~FLAT (21 -> 22 -> 33 us) across a 64x growth
in working set, essentially TRACKING HIP/AQL (within ~5%, marginally faster).
The raw-PM4 path is the only one that explodes (11 -> 67 -> 312 us, ~9.5x slower
than both at M=262144). So:

1. The 6f "PM4 is 6-16x slower" blowup is NOT inherent to driving the GPU through
   the PM4 front-end with a cache barrier. RADV uses the PM4 front-end AND a full
   L2 flush and yet pipelines like HIP. The blowup was specific to OUR naive
   serialization: emitting a CS_PARTIAL_FLUSH that fully DRAINS all waves and
   idles the GPU on every single dispatch. RADV does not pay that bubble -- it
   overlaps the cache op with launch / relies on lighter in-order completion, so
   the next dispatch's waves keep HBM saturated. This directly answers 6f's open
   micro-question: a lighter PM4 serialization DOES match HIP (RADV is the proof).

2. On REAL kernels, Vulkan and HIP CONVERGE. Both become memory-bandwidth-bound
   on the dependent chain (the per-iter cost grows only ~1.5x for 64x more data
   = bandwidth-limited, not launch-limited). The "HIP gap is ~2x Vulkan" result
   from sections 3/5 was a TRIVIAL-kernel (gap.comp) artifact of per-dispatch
   fixed overhead; it disappears once the kernels do real memory work. Here HIP
   is even a hair faster than Vulkan at large M.

3. RADV's barrier is NOT free but it is cheap and overlapped: barrier=1 vs
   barrier=0 costs only ~3 us (M=4096) to ~11 us (M=262144) per iter, growing
   gently -- compare the raw-PM4 CS_PARTIAL_FLUSH which added ~500 us/iter at
   M=65536 (6f, FENCE=0 vs FENCE=1). Same logical fence, ~50x cheaper mechanism.

CONCLUSION (reinforced and sharpened): for real decode kernels there is NO
front-end that beats HIP/AQL on a dependent chain -- HIP, Vulkan/RADV all sit at
the memory-bandwidth frontier and tie. The PM4 front-end can match HIP only if
serialized RADV-style (no full wave-drain); done naively it is far worse. Either
way the ceiling is HIP's existing behavior, so there is no decode throughput
upside to routing real decode through a PM4 dispatch path. The 6f decision (do
NOT pursue the CLR/HIP-runtime PM4 modification) stands, now with the actual
production Vulkan path measured as the third data point rather than inferred.

Artifacts: vk_gap_test/vk_layer.cpp, vk_gap_test/layer_add.comp,
vk_gap_test/layer_scale.comp. Build (2nd container, has glslc + Vulkan headers):
glslc layer_*.comp -o *.spv ; g++ -O2 -std=c++17 vk_layer.cpp -lvulkan -o vk_layer.
Run: ./vk_layer [N] [M] [barrier 0/1]. RADV NAVI31, dedicated compute queue.
(hipvk-isolated container has no Vulkan toolchain; vk_layer was built and run in
test-framework-sshliapn-container-2nd, which carries glslc + the radeon ICD.)

--------------------------------------------------------------------------------

## 6h. Whole-pipeline check + WHERE the Vulkan-vs-PM4 us actually go

Two follow-up questions on 6g: (1) is the per-iter number the WHOLE pipeline
(kernel exec included) or only the inter-kernel gap? (2) Vulkan 21 us vs raw PM4
11 us at M=4096 -- that 2x looks suspicious, what is it?

### (1) It is the whole pipeline, not the gap

vk_layer measures CPU wall of ONE submission of all 3*N dispatches (submit ->
fence). Added per-dispatch GPU timestamps (VK_TS=1) to cross-check:

  M=4096 : whole-pipeline GPU span = 31.74 ms ; CPU wall = 31.81 ms ; ratio 1.002
  M=65536: whole-pipeline GPU span = 32.34 ms ; CPU wall = 32.42 ms ; ratio 1.002

So CPU wall == GPU execution span (the submit/fence roundtrip is amortized to
noise over 3000 dispatches). Per-dispatch decomposition (TOP->BOT = kernel,
BOT->BOT = period): kernel ~9.6 us, gap(barrier+CP+drain) ~0.9 us. The number is
DOMINATED by kernel execution + per-dispatch front-end state; the barrier/gap is
under 1 us/dispatch. (Note: VK_TS itself serializes and inflates the totals to
~31 us; use it only for the kernel-vs-gap split, not absolute timing.)

### (2) The 2x is per-dispatch FRONT-END STATE, not the barrier or a bug

Probed by toggling the per-dispatch vkCmdBindPipeline (VK_SAME = bind one
pipeline once, no rebind) and the barrier. Pinned clock (sclk ~569 / mclk 1124),
per-iter (3 kernels) us, per-dispatch in parens:

  M (elems)   VK rebind     VK no-rebind   HIP graph    HIP loop     raw PM4
  ---------   -----------   ------------   ----------   ----------   -------------
  4096        21.0 (7.0)    6.4 (2.1)      23.7 (7.9)   22.9 (7.6)   11.4 (3.8)
  65536       22.4 (7.5)    8.7 (2.9)      24.0 (8.0)   23.3 (7.8)   66.7 (22.2)
  262144      32.9 (11.0)   18.0 (6.0)     31.1 (10.4)  33.7 (11.2)  312.0 (104.0)

  Isolation at M=4096, barrier=0 (NO sync at all):
    VK rebind   = 5.9 us/dispatch
    VK no-rebind= 0.29 us/dispatch   => vkCmdBindPipeline costs ~5.6 us/dispatch

Findings:

a) The barrier is NOT the cost. With no barrier, rebind still costs 5.9 us/disp
   vs 0.29 us/disp without rebind. The whole Vulkan-vs-PM4 small-M gap is the
   per-dispatch STATE EMISSION, not cache coherence.

b) RADV's vkCmdBindPipeline re-emits the FULL compute pipeline state block
   (shader regs, user data, scratch/prefetch) every op = ~5 us/dispatch. The raw
   PM4 path swaps kernels with a minimal ~4-register write (PGM_LO/HI, RSRC1/2),
   so it is ~2x leaner per dispatch (3.8 vs 7.0 us). ggml-vulkan binds a pipeline
   per op too, so this cost is REAL for Vulkan decode -- not a harness artifact.
   Strip the rebind (VK_SAME) and Vulkan drops to 6.4 us/iter, LEANER than PM4 --
   proving the front-end mechanism is fine; the per-op state re-emit is the cost.

c) HIP graph (pre-recorded) ~= HIP loop at small M (23.7 vs 22.9). So HIP's
   ~7.8 us/dispatch is NOT CPU launch overhead -- it is GPU-side per-packet AQL
   work (acquire/release fence + state). HIP and Vulkan-rebind sit in the same
   ~7-8 us/dispatch band; raw PM4's hand-tuned minimum is ~3.8 us.

d) So the small-M ordering PM4 (11) < VK/HIP (21-23) is genuine and is the
   runtime per-dispatch fixed overhead (~2x). It IS relevant for decode, which is
   made of many small ops. BUT it is a fragile win: the SAME PM4 path explodes to
   104 us/dispatch at M=262144 because its CS_PARTIAL_FLUSH fully drains a wide
   grid every dispatch (no overlap). Vulkan no-rebind (6.0 us/disp at 262144) is
   the real floor and shows the ideal is lean-state + overlapped-barrier together
   -- which RADV achieves and a naive PM4 path does not.

Practical takeaway for decode: the lever is NOT switching to a PM4 dispatch path
(fragile, blows up on wide kernels, GPU-hang-prone to integrate). It is reducing
per-dispatch fixed overhead the safe way -- fuse adjacent small ops (fewer
dispatches), avoid redundant pipeline/state re-binds, and keep the dependent
chain overlapped (HIP/AQL and RADV both already do this). The ~4 us/dispatch the
raw PM4 path saves at decode sizes is real but only safely reachable by cutting
the dispatch count, not by hand-rolling PM4.

Artifacts: vk_layer.cpp (VK_TS=1 timestamps, VK_SAME=1 no-rebind probe),
pm4_gap/hip_layer_graph.cpp (hipGraph pre-recorded HIP, fair vs Vulkan/PM4).

--------------------------------------------------------------------------------

## 6i. CORRECTION: what actually drives the PM4 numbers (rebind / stability / 2x)

Follow-up dig into three questions (what is "rebind"; why Vulkan is stable while
PM4 is not; why PM4 is 2x faster than HIP at small M). Controlled probes (all
PINNED clocks, sclk ~570 / mclk 1124) OVERTURN the section-6f mechanism claim.

### Clarification: all three numbers are wall-clock of the WHOLE test

  - PM4 : t0 just before the doorbell write -> busy-poll the sentinel that the
          LAST packet writes -> t1. Pure GPU execution of the whole pre-built
          PM4 stream (all 3*N dispatches + fences). pm4_layer.cpp:285-294.
  - VK  : t0 before vkQueueSubmit -> vkWaitForFences -> t1. Whole submission.
          Cross-checked: GPU timestamp span == CPU wall, ratio 1.002 (6h).
  - HIP : t0 before the launch loop / graph launch -> hipDeviceSynchronize -> t1.
  All measure the same thing: GPU execution of one pre-recorded stream. The
  per-dispatch numbers are total/(3N).

### (Q1) "rebind" = vkCmdBindPipeline re-emitting full pipeline state per op

A Vulkan compute pipeline bundles the compiled shader + all its fixed register
state. vkCmdBindPipeline makes RADV emit that whole block into the stream
(COMPUTE_PGM_*, RSRC1/2/3, user-data/descriptor ptrs, tmpring/scratch, wave size,
plus a shader I-cache prefetch). The elem chain alternates add/scale/add, so it
re-binds 3x/iter. Measured cost (M=4096, barrier=0 so no sync at all): rebind
5.9 us/dispatch vs no-rebind (VK_SAME) 0.29 us/dispatch => ~5.6 us/dispatch is
the bind itself. VK no-rebind beats raw PM4 because it emits ZERO per-dispatch
state (same kernel, same args repeated -- only the dispatch packet recurs), while
PM4 still re-emits ~7 SET_SH_REG groups per dispatch (PGM_LO/HI, RSRC1/2,
USER_DATA kernarg ptr, START_X/dims, RESOURCE_LIMITS, TMPRING) because the chain
switches kernels and kernarg pointers. So VK_SAME is a best-case (nothing to set
up); the fair match to PM4-elem is VK-rebind, where PM4's lean register swap
(~7 regs) still beats RADV's full pipeline-state re-emit (~2x at small M).

### (Q2 + the "why 2x" mechanism) -- the 6f "wave-drain" story was WRONG

Section 6f blamed CS_PARTIAL_FLUSH (a full wave-drain bubble) for PM4's large-M
blowup. Re-measured under PINNED clocks, that is FALSE:

  PM4 elem, per-dispatch us:        M=4096    M=65536    M=262144
    FENCE=0 (NO fence at all)        1.0       23.3       96.1
    FENCE=1 (CS_PARTIAL_FLUSH)       3.8       21.6      102.9
    CACHED=1 (L2-resident buffers)   3.8       23.2      102.3   (uncached ~ same)
    CUMASK=1 (all 96 CUs enabled)    --        22.7      104.0   (no change)

  - The fence is NOT the large-M cost: FENCE=0 (no drain) is within ~10% of
    FENCE=1 at every size. The 6f "FENCE=1 = 571 us/iter" was an UNPINNED-CLOCK
    artifact -- the drain bubble let the W7900 autosuspend/downclock between
    dispatches. Pin the clock and the drain costs almost nothing.
  - Cache scope is NOT it: cached (L2-resident) == uncached.
  - CU mask is NOT it: hsaKmtSetQueueCUMask(all 96 CUs) returns success and
    changes nothing -- the queue already uses all CUs.
  - What IS it: PM4 cost scales PERFECTLY LINEARLY with workgroup count
    (~0.06-0.09 us/WG: 16 WGs->1.0 us, 256->23 us, 1024->96 us), i.e. ZERO
    parallel-launch benefit. This is the signature of LOW OCCUPANCY -- the SPI
    keeps too few waves resident, so the trivial kernel cannot hide memory
    latency and wide grids do not parallelize.

  DECISIVE CONTROL (same kernel binary, different dispatcher): hip_layer and
  pm4_layer load the SAME k_add (both from layer_kernels.hip -> layer.co /
  layer_gfx1100.co). At M=262144: HIP = 11.2 us/dispatch, PM4 = 103 us/dispatch.
  Same kernel, ~9x. So the gap is NOT the kernel code, NOT the PM4 front-end, and
  NOT fundamental -- it is the per-dispatch compute STATE we hand-program. The CP
  /SPI schedule waves automatically; the dispatch packet's register state
  (COMPUTE_PGM_RSRC1/2 VGPR/SGPR granularity, COMPUTE_RESOURCE_LIMITS WAVES_PER_SH
  /TG_PER_CU, LDS, dims) tells them HOW MANY waves to keep resident. pm4_layer
  writes COMPUTE_RESOURCE_LIMITS=0 and a minimal set; ROCr/RADV program the full
  occupancy-governing state. Get that wrong and occupancy collapses -> the linear
  WG scaling above. So Vulkan/HIP look "stable" (7 -> 7.5 -> 11 us) because they
  are bandwidth-bound at proper occupancy; PM4 looks "unstable" (3.8 -> 22 -> 103)
  because our dispatch state yields low occupancy. FIXABLE, not inherent.

  Why PM4 is 2x faster than HIP at SMALL M (decode-sized): there the kernel is
  ~free, so the number is pure per-dispatch overhead. PM4 streams hand-built raw
  packets the CP runs directly (lean register swap, ~1 us/dispatch with no fence,
  3.8 us with CS_PARTIAL_FLUSH). HIP's AQL dispatch costs ~7.6 us/dispatch
  because the MEC runs AQL-packet microcode per dispatch: acquire/release fence
  scope -> cache-op packets, completion-signal management, barrier-bit check, the
  AQL->PM4 translation. hipGraph (pre-recorded) does NOT cut it (23.7 ~ 22.9),
  proving it is GPU-side AQL packet processing, not CPU launch rate. That fixed
  per-dispatch overhead is the entire 2x; it only shows when the kernel is tiny.

### How to align raw PM4 with Vulkan/HIP (corrected)

Not "use a lighter fence" (fence is irrelevant under pinned clocks). The real
levers, in order:
  1. Per-dispatch overhead (small M): PM4 is ALREADY leaner than HIP/Vulkan.
     Nothing to align there -- PM4 wins. The way to give HIP/Vulkan that win is
     to cut per-dispatch work (op fusion -> fewer dispatches), not adopt PM4.
  2. Large-M occupancy: PM4 must program the occupancy-governing state correctly
     so the SPI keeps enough waves resident. Proven fixable by the same-kernel
     control (HIP gets 11 us with the identical binary). Prime suspect:
     COMPUTE_RESOURCE_LIMITS=0 and any occupancy state ROCr sets that pm4_layer
     omits. Fixing it would make PM4 EQUAL Vulkan/HIP (tie at the bandwidth
     floor), not beat them -- you cannot move the bytes faster than bandwidth.

Net: PM4 is NOT inherently slower than Vulkan -- a correctly-set-up dispatch
reaches the same bandwidth floor (the same-kernel control proves it). PM4's only
genuine EDGE is lower fixed per-dispatch overhead at decode-sized ops; at the
bandwidth floor it merely ties. So the safe way to bank the small-op win is
fewer/fused dispatches inside HIP, not a PM4 dispatch path.

Artifacts: pm4_gap/pm4_layer.cpp (env: FENCE=0|1|2, CACHED=1, CUMASK=1,
GCR_MODE, CHAIN=elem|rms). Compared vs vk_layer (VK_SAME) and hip_layer_graph.

--------------------------------------------------------------------------------

## 6j. "Fix occupancy" attempt: it is NOT occupancy; prime suspect is MTYPE/L2

Tried to close the large-M PM4 gap. Systematically ruled OUT the obvious
occupancy/launch suspects (all pinned clocks, same k_add binary as HIP):

  - CU mask          : hsaKmtSetQueueCUMask(all 96 CUs) -> success, NO change.
  - Shader-engine mask: wrote COMPUTE_STATIC_THREAD_MGMT_SE0..SE5 = 0xFFFFFFFF
                        (all CUs on all 6 gfx11 SEs) -> NO change.
  - COMPUTE_RESOURCE_LIMITS: already 0 in both pm4_layer AND kfdtest's validated
                        dispatch, so not a throttle.
  - Fence / cache scope / fine-vs-uncached: all ruled out earlier (6i).

DECISIVE CONTROL stands: identical k_add binary, HIP 11 us/dispatch vs PM4 103
us/dispatch at M=262144. Since it is not occupancy registers, the remaining
structural difference between the two dispatchers is the BUFFER MEMORY TYPE.
pm4_layer allocates fine-grain (HsaMemFlags CoarseGrain=0; MTYPE_CC/UC) for
CPU-coherent verify; hipMalloc/RADV use coarse-grain device VRAM (MTYPE_RW, full
L2 writeback). Fine-grain on a dGPU does not get L2 writeback caching, so every
dispatch re-streams the working set from HBM -- which fits the PERFECTLY LINEAR
-in-working-set PM4 cost.

ATTEMPTED FIX (COARSE=1), FIRST TRY -- BROKEN: allocating the data coarse-grain
made timing drop ~26x but the verify mirror showed the kernel's WRITES never
landed (x read back as the stale init even at N=1). That "speedup" was
dropped-write traffic, not real L2 caching. ROOT CAUSE of the dropped writes is
now identified -- see 6k.

--------------------------------------------------------------------------------

## 6k. PROVEN: replicating hipMalloc closes the large-M gap (PM4 == HIP)

The coarse-grain control is now correct and the MTYPE hypothesis is PROVEN.

Two bugs in the first coarse attempt, both fixed:
  1. HostAccess=1 + CoarseGrain=1 is contradictory: coarse-grain device VRAM is
     NOT CPU-coherent, so a direct CPU read of the device buffer always shows the
     stale init. FIX: allocate the compute buffers (x,y,w) as PURE device VRAM
     (NonPaged=1, CoarseGrain=1, HostAccess=0) -- exactly what hipMalloc returns.
  2. THE write-drop itself: the inter-stage fence used a RADV-style full flush
     GL2_WB|GL2_INV. On this path the GL2 INVALIDATE discards the dirty L2 lines
     (the device coherence point IS L2), so the GPU's just-written x was thrown
     away before any reader saw it. FIX: between dependent on-device kernels use
     an AGENT-scope fence (invalidate L0/L1 only, leave L2 intact). The dirty x
     stays resident and the next kernel reads it correctly.

Host I/O is done the way hipMemcpy does it: a host-accessible UNCACHED staging
buffer + an on-GPU k_copy kernel (H2D: x <- xs, w <- ws before the chain; D2H:
outs <- x after). outs is uncached so its write bypasses L2 to HBM and the CPU
reads it coherently. (New kernel k_copy added to layer_kernels.hip.)

CORRECTNESS (gfx1100, elem chain x+=w; x*=w; x+=w):
  N=0 round-trip (H2D then D2H, no chain):  outs == xs  (0.10000, 0.11000, ...)  OK
  N=1 COARSE:    checksum=8750.109214  x[0]=2.100000   <- EXACTLY matches
  N=1 fine-grain reference (CACHED=1):  checksum=8750.109214  x[0]=2.100000

WORKING-SET SWEEP, elem chain, N=1000, pinned profile_standard (593 MHz sclk),
us/iter (3 kernels). All four paths measured in ONE run on the same pinned device
(W7900 / RADV NAVI31). PM4 + HIP built and run in the hipvk-isolated container;
Vulkan/RADV also run in the SAME hipvk-isolated container (libvulkan + radeon_icd
present there). PM4 uses the RADV-matched ACQUIRE_MEM from 6k.1 (FENCE=2, fully
cache-coherent, NO access-pattern shortcut):

  M (floats)  PM4 COARSE   PM4 UNCACHED   HIP/AQL   VULKAN/RADV
  4096          11.99         17.50        22.78       21.13
  16384         19.93         31.13        22.76       21.33
  65536         20.23         87.89        23.15       22.47
  262144        21.53        314.60        33.64       32.95
  1048576       53.99       1180.89        65.78       70.11

READING THE TABLE:
- PM4 UNCACHED grows PERFECTLY LINEARLY with the working set (88 -> 315 -> 1181 us
  as M goes 65k -> 262k -> 1M). That is the HBM-bandwidth floor: every dispatch
  re-streams x and w from HBM because the buffer bypasses L2.
- PM4 COARSE (== hipMalloc) is FLAT until the working set exceeds L2 (~20-21 us
  across 16k..262k), then rises only when x+w no longer fit in the 6 MB L2 (1M
  floats = 4 MB x + 4 MB w = 8 MB > L2). It is now the FASTEST cached path at
  every size -- faster at small M (12.0 vs ~21 us) thanks to the leaner dispatch
  front-end, and still ahead in the L2 plateau and at the 1M bandwidth point
  (54.0 vs HIP 65.8 / Vulkan 70.1 us).
- HIP/AQL and Vulkan/RADV track each other closely (~21-23 us plateau); both pay
  a heavier fixed per-dispatch cost than the hand-rolled PM4 front-end.
- The ~15x gap from PM4 COARSE to PM4 UNCACHED at 1M (54 vs 1181 us) is entirely
  the missing L2 residency.

(Absolute numbers differ from the earlier profile_peak table because this run is
pinned at profile_standard, the shared all-users clock; the RELATIVE ordering is
what matters and it is consistent: COARSE < HIP ~= Vulkan << UNCACHED.)

--------------------------------------------------------------------------------

### 6k.1 Matching PM4's ACQUIRE_MEM to RADV (no access-pattern shortcut)

Decomposing the inter-dispatch cost at M=16384 (L2-resident, so NOT bandwidth),
pinned profile_standard, us/iter:

  variant                                    us/iter   us/disp   correct?
  PM4 coarse, no fence (FENCE=0)                2.50      0.83     NO (race)
  PM4 coarse, CS_PARTIAL_FLUSH only (FENCE=1)   5.20      1.73     YES (here)
  PM4 coarse, drain + ACQUIRE_MEM (FENCE=2)    20.37      6.79     YES
  Vulkan, no barrier                           17.81      5.94     NO
  Vulkan, full barrier                         21.25      7.08     YES

Findings:
- The PM4 front-end dispatch is much leaner than RADV's (no-sync floor 2.5 vs
  17.8 us/iter): the hand-rolled stream emits a minimal register set per dispatch,
  RADV re-emits far more pipeline state.
- The entire PM4-vs-Vulkan delta is the inter-dispatch barrier. CS_PARTIAL_FLUSH
  (wave drain) is cheap (+2.7 us/iter); the cache ACQUIRE_MEM is the bulk.
- FENCE=1 (drain only) was bit-exact here, but ONLY because every element is
  written and read at the same index. A reverse/scatter/transpose access could
  read a stale L0/L1 line that the drain alone never invalidates. So FENCE=1 is
  REJECTED as unsafe in general; the correct path is FENCE=2 with a real cache
  invalidate -- exactly what RADV does.

To make FENCE=2 both correct AND cheap, the PM4 ACQUIRE_MEM was matched byte-for-
byte to what RADV emits for a compute->compute buffer barrier (mesa
radv_cs.c:gfx10_cs_emit_cache_flush, verified against the GCR_CNTL layout in
src/amd/registers/pkt3.json):
  1. GCR_CNTL bit layout was WRONG: the code had GL2_INV=bit13 / GL2_WB=bit14, but
     the real layout is GL2_DISCARD=13, GL2_INV=14, GL2_WB=15. (This same bug was
     the original coarse "dropped writes": the old GCR_RADV_LIKE was setting
     GL2_DISCARD instead of GL2_WB, discarding dirty lines.) Bits corrected.
     NOTE: the AGENT-scope value used by the chain fence, GL1_INV|GLV_INV|GLK_INV
     = 0x380, was already correct and is byte-identical to RADV's compute barrier
     GCR_CNTL -- so the chain's per-dispatch GCR was right all along.
  2. ACQUIRE_MEM body fields aligned to RADV: CP_COHER_SIZE_HI 0 -> 0xffffff,
     POLL_INTERVAL 4 -> 0x0A (CP_COHER_CNTL=0, SIZE=0xffffffff, BASE=0 already
     matched).

Result: FENCE=2 dropped 23.2 -> 20.4 us/iter and now MATCHES/BEATS Vulkan (21.2)
with a fully cache-coherent barrier and bit-exact output -- no reliance on the
access pattern. This is the number used in the 6k sweep table above.

CONCLUSION (this REVERSES the earlier "PM4 6-16x slower, do not pursue"):
The large-M PM4 slowdown was NEVER the dispatch path, the CS_PARTIAL_FLUSH fence,
or occupancy (CUMASK/SEMASK had no effect). It was 100% the MEMORY TYPE: the
verify-friendly fine-grain/uncached buffers bypass L2, so PM4 paid full HBM
bandwidth every dispatch while HIP/Vulkan ran on L2-cached device-local VRAM.
With faithful hipMalloc-style allocation (coarse-grain device VRAM + AGENT-scope
on-device fences + staged host I/O), raw PM4 matches HIP at every working-set
size and beats it at decode-sized ops. The decode-relevant verdict is now
strictly favorable to PM4: same bandwidth floor at large M, lower fixed overhead
at small M. Env flags in pm4_layer.cpp: COARSE=1 (hipMalloc replica, VALIDATED),
CACHED=1 (fine-grain L2), default UNCACHED, COARSE_HA=1 (diagnostic).

--------------------------------------------------------------------------------

## 6l. 64-kernel chain + REVERSE-READ kernel (cache-coherence proof)

To (a) stress a much longer dependent chain and (b) actively prove the cache
invalidate is required (not just an access-pattern coincidence), a reverse-read
kernel was added to all three frameworks:

  k_revadd(dst,src): dst[i] = 0.5*dst[i] + 0.5*src[n-1-i]

Thread i reads src at the MIRRORED index n-1-i, which in the previous dispatch was
written by a DIFFERENT workgroup (different WGP / different L0,L1 vector cache).
The chain (CHAIN=rev, KLEN=64) ping-pongs two buffers x<->y: even dispatch writes
y reading reverse(x), odd writes x reading reverse(y), 64 dispatches/iter. The
averaging form keeps values bounded so the checksum stays finite and comparable.
Implemented identically in pm4_layer (k_revadd, slots for (y,x)/(x,y), coarse y
staging), hip_layer / hip_layer_graph, and vk_layer (layer_revadd.comp + two
ping-pong descriptor sets dsetXY/dsetYX). Vulkan built in the 2nd container
(glslc + headers), RUN in hipvk-isolated.

CACHE-COHERENCE PROOF (M=4096, N=20):
  PM4 COARSE FENCE=2 (RADV-matched acquire) : checksum 477.833330  CORRECT
  PM4 CACHED FENCE=2                         : checksum 477.833330  CORRECT
  HIP (CPU loop)                             : checksum 477.833330  CORRECT
  HIP (hipGraph)                             : checksum 477.833330  CORRECT
  VULKAN/RADV                                : checksum 477.833330  CORRECT
  PM4 COARSE FENCE=1 (CS_PARTIAL_FLUSH only) : checksum 492.679682  WRONG (x non-uniform)

=> The reverse kernel makes the drain-only fence visibly WRONG (stale L0/L1 lines),
   while the proper acquire (GLV_INV|GL1_INV) is bit-exact across ALL frameworks.
   This is the concrete justification for matching ACQUIRE_MEM to RADV (6k.1)
   instead of relying on FENCE=1. Functional equivalence is also confirmed: PM4,
   HIP and Vulkan agree bit-for-bit at every M (checksums verified equal).

TIMING, 64-kernel reverse chain, N=200, pinned profile_standard, us/iter (64 kern):

  M (floats)  PM4 COARSE   HIP/AQL(graph)   VULKAN/RADV
  4096          258.7          504.5           144.0
  16384         449.6          504.7           152.8
  65536         425.4          505.8           195.9
  262144        442.7          662.7           395.1

READING THE TABLE (this is a DIFFERENT regime from the 3-kernel elem chain):
- VULKAN is now FASTEST by a wide margin. The chain reuses ONE pipeline (pRev) for
  all 64 dispatches, so RADV emits almost no per-dispatch pipeline state (just a
  descriptor-set swap) -- its per-dispatch cost collapses to ~2.2 us. In the elem
  chain it ALTERNATED pAdd/pScale, paying pipeline re-bind state every dispatch.
- PM4 sits in the middle (~4 us/dispatch, roughly flat). Its per-dispatch cost is
  dominated by the ACQUIRE_MEM cache invalidate, which it pays 64x/iter regardless
  of whether the kernel changed. PM4 re-emits the full dispatch packet (incl. the
  COMPUTE_PGM_RSRC / kernel-descriptor registers) every dispatch even when the
  kernel is identical -- an optimization opportunity (emit KD regs once, only bump
  the kernarg pointer for same-kernel repeats).
- HIP/AQL(graph) is slowest (~7.9 us/dispatch): fixed AQL packet + acquire/release
  fence overhead per kernel.

CROSSOVER / HONEST VERDICT: the "PM4 is fastest" result is workload-specific. PM4
wins when the chain ALTERNATES short kernels (elem: its lean front-end beats RADV's
per-dispatch pipeline-state cost). On a LONG SAME-KERNEL chain, RADV's near-zero
state re-emission wins, and PM4's repeated ACQUIRE_MEM becomes the bottleneck. So
the takeaway is not "PM4 always faster" but "PM4's front-end is leaner per dispatch,
yet its mandatory per-dispatch cache acquire caps it; RADV amortizes pipeline state
better for repeated kernels." All three remain functionally identical (bit-exact).

--------------------------------------------------------------------------------

## 6m. GLK_INV (scalar-cache) drop: a real but COMPILER-SPECIFIC PM4 win, and a
##      corrected, clock-pinned fairness comparison

This section (a) records a large PM4 per-dispatch speedup from dropping the
scalar-cache invalidate, (b) proves from RADV source that this is NOT something
RADV does or could do, and (c) CORRECTS the attribution in earlier sections:
with matched cache semantics, PM4 is on par with -- not faster than -- Vulkan.

Clocks are now pinned reliably: the host has a NOPASSWD sudoers entry for
/usr/local/sbin/gpu_pin_freq.sh and gpu_restore_freq.sh, so every run below is
`profile_peak` (GFX 1778 MHz, MEM 1124 MHz, verified under load). Pin before /
restore after every GPU test.

### 6m.1 The optimization: GCR 0x380 -> 0x300 (drop GLK_INV bit 7)

The per-dispatch chain fence was ACQUIRE_MEM with GCR_CNTL = GLV_INV|GL1_INV|
GLK_INV = 0x380 (vector L0 + L1 + scalar K-cache invalidate). Dropping the
scalar-cache bit -> 0x300 (GLV_INV|GL1_INV) is BIT-EXACT identical in result on
both the mix (reverse-read) and serial chains across every M tested, yet 2.5-5x
cheaper per dispatch. New default in pm4_layer.cpp is 0x300 (GCR_RAW=0x380 to
restore the old behavior).

WHY IT IS CORRECT HERE (verified in the ISA, layer_gfx1100.co):
  - buffer DATA is read/written with VECTOR ops: global_load_b32 / global_store_b32
    -> flows through GLV (L0 vector) / GL1, which 0x300 invalidates.
  - the only SCALAR (s_load) traffic is the kernarg pointers/sizes, and those are
    written ONCE into distinct immutable slots (the ping-pong uses DIFFERENT slots
    -- slot 2 = (x,y), slot 7 = (y,x) -- never rewriting one), so the K-cache can
    never hold a stale value.
  Hence invalidating GLV+GL1 is sufficient; GLK_INV is pure overhead.

### 6m.2 RADV DOES set GLK_INV -- it is compiler-ABI-mandated, not a missed opt

Read directly from mesa (radv_cmd_buffer.c::radv_dst_access_flush, radv_cs.c::
gfx10_cs_emit_cache_flush):

  radv_cs.c:150-159   INV_SCACHE -> GL1_INV|GLK_INV ;  INV_VCACHE -> GL1_INV|GLV_INV
  radv_cmd_buffer.c:6700-6701  UNIFORM_READ -> INV_VCACHE | INV_SCACHE
  radv_cmd_buffer.c:6720-6723  "Unlike LLVM, ACO uses SMEM for SSBOs and we have to
                               invalidate the scalar cache." (SSBO read -> INV_SCACHE)

VK_ACCESS_SHADER_READ expands (vk_expand_dst_access_flags2) to include UNIFORM_READ
+ SHADER_STORAGE_READ, so for a shader-write->shader-read barrier RADV emits
GL1_INV|GLV_INV|GLK_INV = 0x380 -- the SAME bits as PM4's old default. RADV cannot
drop GLK_INV because its ACO compiler reads SSBO data via SCALAR (SMEM) loads, so
produced data lands in the K-cache. Our hipcc/clang kernels read data via vector
loads, so we can. The win is a COMPILER/ABI difference, not a faster dispatcher.

DO NOT drop GLK_INV for any kernel that reads another kernel's output via a
scalar/uniform load.

### 6m.3 CORRECTED fairness (pinned profile_peak, serial+mix, N=20)

The headline matters: at MATCHED cache semantics (both 0x380), PM4 is NOT faster
than Vulkan -- it is equal-or-slightly-slower. The big PM4 advantage is entirely
the 0x300 cache reduction from 6m.1.

Per-kernel dispatch time (us), LAYERS=8 (40 kern/iter):

  CHAIN=mix                              CHAIN=serial
  M       PM4 0x300 PM4 0x380 VK   HIPe  | PM4 0x300 PM4 0x380 VK   HIPe
  4096      1.18     2.96    4.56 3.94   |  1.18     3.01    4.55 3.95
  16384     1.24     5.63    4.57 3.96   |  1.23     5.90    4.57 3.97
  65536     1.51     5.69    4.64 4.08   |  1.50     5.74    4.66 4.08
  262144    2.56     6.07    5.97 5.54   |  2.57     5.86    5.91 5.52

End-to-end per decode TOKEN (us), LAYERS=60 (300 kern/token, realistic decode):

  CHAIN=mix                          CHAIN=serial
  M       PM4 0x300 PM4 0x380 VK     | PM4 0x300 PM4 0x380 VK
  16384     337     1722    1359     |   338     1677    1359
  65536     409     1685    1379     |   409     1631    1408

Reading it honestly:
- PM4 0x300 (1.2-2.6 us/kern) is the fastest by 2-4x, but ONLY because of the
  compiler-specific GLK_INV drop (6m.1/6m.2) -- not the dispatch path.
- PM4 0x380 (RADV-identical bits) is ~5.5-6 us/kern, i.e. ON PAR with or SLIGHTLY
  SLOWER than VK (~4.6-5.9). So at equal coherence work, RADV's path is as good as
  or better than the hand-rolled PM4 stream. Earlier sections (6d/6h) that implied
  a 2x PM4 dispatch-path win on trivial kernels do not generalize here.
- HIP eager (HIPe, ~4 us/kern) is CPU-LAUNCH-bound, not a GPU-throughput number;
  HIP graph is ~8 us/kern. Neither is the right comparand for a pre-built PM4
  stream. PM4 and VK both measure one GPU window (VK CPU-wall == GPU-span, 6h).

### 6m.4 WHY GLK_INV cost GROWS with M (the "scalar data is constant" puzzle)

Measured GLK_INV marginal cost = (PM4 0x380 - PM4 0x300) per dispatch, serial,
pinned:

  M       0x300   0x380   GLK delta
  4096    0.79    2.69    1.90
  8192    0.83    4.00    3.17
  16384   0.82    5.58    4.76
  32768   0.90    5.51    4.61
  65536   0.99    5.33    4.34
  262144  1.62    5.63    4.01

The 0x300 path is nearly flat; essentially ALL the M-dependence lives in the
GLK_INV bit, which rises steeply to ~4.7 us by M=16384 then SATURATES.

The user's intuition is correct: the amount of scalar DATA (2 pointers + n =
~20 bytes) is identical at every M. So the cost is NOT "invalidating more scalar
lines." Two facts constrain the mechanism:
  - magnitude rules out a bandwidth/drain story: at M=16384 the working set is
    64 KB; draining 64 KB to L2 at ~1-2 TB/s is tens of ns, not 4.7 us.
  - the cost tracks the WORKGROUP COUNT and saturates exactly where the grid
    covers the whole GPU: grid = ceil(M/256) WGs; M=4096->16 WGs, 8192->32,
    16384->64. The W7900 has 48 WGPs (96 CUs). The delta grows while WGs < ~48
    WGPs and flattens once the grid covers all WGPs (M>=16384).

BEST-SUPPORTED EXPLANATION (inferred, not firmware-traced): the scalar K-cache is
PER-WGP. GLK_INV must invalidate the K-cache instance in every WGP that ran a
wave of the producer, and the GCR engine waits for an acknowledgement from each.
That ack count scales with the number of occupied WGPs (= grid size) until the
grid covers the whole chip, then saturates -- which matches both the shape and
the ~16K saturation point. The vector L0/L1 invalidate (0x300) does not expose
this serialized per-WGP ack wait (it appears to be applied more cheaply / in
parallel on this uarch), which is why the 0x300 path stays flat. This is the most
likely mechanism given the data; the exact CP/GCR micro-behavior is closed
firmware. DECISIVE NEXT TEST (not yet run): at fixed large M, restrict the queue
CU mask to fewer WGPs; if the GLK delta drops proportionally, the per-WGP-ack
mechanism is confirmed.

### 6m.5 Net verdict (supersedes the "PM4 3-4x faster" framing)

- The GLK_INV drop is a genuine, bit-exact 2.5-5x per-dispatch speedup for THESE
  kernels, and the right default for this harness.
- It is NOT portable to RADV (ACO SSBO->SMEM forces GLK_INV) and must not be
  applied to kernels that read predecessors' output via scalar loads.
- With matched semantics, PM4 ties Vulkan; the dispatch-path is not the lever.
  The earlier trivial-kernel "2x PM4" result does not carry to these kernels.

Artifacts: pm4_gap/pm4_layer.cpp, vk_layer.cpp, pm4_gap/hip_layer.cpp. RADV
source: mesa-radv/src/amd/vulkan/radv_cs.c + radv_cmd_buffer.c. Clocks:
/usr/local/sbin/gpu_pin_freq.sh profile_peak.

--------------------------------------------------------------------------------

## 6n. ROOT CAUSE of the GLK_INV M-growth, and the FIX (PWS overlapped fence)

GLK_INV is kept ON by default for safety (6m.2). This section proves WHY the
blocking-acquire GLK cost grows with M, why VK/HIP do not, and FIXES PM4 so the
full-safety 0x380 fence is flat and FASTER than VK/HIP.

### 6n.1 The growth is a per-CU K-cache invalidate broadcast (CU-mask sweep)

At fixed M=65536 (grid covers the whole chip), restricting the queue CU mask and
measuring the GLK marginal cost (0x380 - 0x300) per dispatch:

  CUs enabled   4      8      16     24     48     96
  GLK delta    0.76   0.87   1.06   1.42   2.06   4.19  (us/dispatch)

The cost scales ~linearly with the number of active CUs, NOT with the (constant
~20-byte) scalar data. GLK_INV must invalidate the per-CU scalar K-cache in every
CU that ran a producer wave and the GCR engine waits for an ack from each, so the
cost = (active CUs) x ~0.044 us. In the M-sweep this is why it ramps until the
grid fills all 48 WGPs (~M=16384) then saturates.

Fence decomposition (serial, pinned, us/dispatch) confirms the wave-drain is flat
and cheap; GLK is the entire growing term:

  M       F0_none  F1_drain  F2_0x300  F2_0x380  | drain  glk
  4096     0.52     1.00      1.17      3.04      | 0.48   1.87
  16384    0.54     1.03      1.23      5.80      | 0.49   4.57
  65536    0.67     1.30      1.49      5.56      | 0.64   4.07
  262144   1.91     2.36      2.50      5.84      | 0.45   3.34

### 6n.2 Why VK/HIP do NOT expose this (and why VK is slower than HIP here)

It is NOT the bits: PM4 with RADV's exact full gcr (GCR_MODE=0 = GL2_WB|GL2_INV|
GLM|GLK|GLV|GL1) is bit-exact correct but still grows (4.0->7.7us) and is even
slower. SEQ=FORWARD variants do not help either. So matching RADV's GCR_CNTL does
not flatten PM4.

The difference is the WAIT MECHANISM. VK's GPU timestamps show the inter-dispatch
gap (period - kernel) is only ~0.5 us and the windows even OVERLAP at M=65536
(kernel 6.57 > period 6.30). RADV's cache flush is overlapped with the dispatch
window; the CP does not fully stall on it. PM4's blocking ACQUIRE_MEM serializes
(drain -> wait-for-GCR -> dispatch), so the per-CU GLK ack latency is fully
EXPOSED as inter-dispatch time. (Confirming this: PM4 0x300 grows only +1.4 us
across M, the SAME as VK's +1.3 us -- i.e. just bandwidth; the extra +1.5 us in
PM4 0x380 is the exposed GLK.)

Q3 answer (VK slower than HIP here): purely vkCmdBindPipeline. VK re-emits compute
pipeline state every dispatch; HIP has no pipeline-state object. VK no-rebind
(VK_SAME) = 3.94 us == HIP 3.94 us; VK rebind = 4.55 us. The ~0.6 us is the bind.
(In llama.cpp Vulkan still wins overall because it fuses/avoids per-op overhead
that HIP pays elsewhere; this microbench isolates only the dispatch fence/bind.)

### 6n.3 THE FIX: PWS deferred-wait fence (overlap the flush like RADV)

Replicated RADV's GFX11 overlap mechanism on the raw KFD MEC queue:
  fence = CS_PARTIAL_FLUSH                       (drain producer waves; REQUIRED)
        + RELEASE_MEM (EVENT=BOTTOM_OF_PIPE_TS, GCR cache flush, PWS_ENABLE=1)
        + ACQUIRE_MEM (PWS_STAGE_SEL=CP_ME, PWS_ENA/ENA2=1, GCR_CNTL=0; waits the
                       PWS counter)
The RELEASE_MEM issues the GCR flush asynchronously and bumps the PWS counter;
the PWS ACQUIRE_MEM waits on that counter without the CP blocking inline, so the
flush overlaps the next dispatch. Bit layouts from mesa pkt3.json
(RELEASE_MEM_OP_gfx11, ACQUIRE_MEM_PWS_2/_7). Implemented in pm4_layer.cpp
(Emit::release_mem_pws / acquire_pws), default ON, PWS=0 to fall back.

CORRECTNESS GOTCHA: the CS_PARTIAL_FLUSH wave-drain is REQUIRED before the PWS
pair. Without it the RELEASE_MEM EOP does not order the consumer against the
producer's outstanding stores on a raw MEC queue -> non-deterministic ~1%-wrong
checksums. With the drain, PWS is bit-exact and deterministic on BOTH the serial
and the reverse-read mix (cache-coherence stress) chains at every M.

RESULT (pinned profile_peak, serial LAYERS=8 N=20, us/dispatch):

  M       BLOCKING 0x380   PWS 0x380   VK(RADV)   HIP eager
  4096      3.06            2.52        4.55       3.95
  16384     5.08            2.57        4.57       3.98
  65536     5.68            2.84        4.67       4.10
  262144    5.84            3.91        5.89       5.54

PWS 0x380 is FLAT (residual growth is only bandwidth, like VK/HIP), bit-exact
with full GLK_INV cache safety, ~2x faster than the blocking acquire, and FASTER
than both VK and HIP at every M. End-to-end per decode token (300 kernels,
LAYERS=60) dropped from ~1630-1720 us (blocking) to ~725-1110 us. Verified on the
reverse-read mix chain too (deterministic x3 at M=262144).

CONCLUSION: the PM4 0x380 M-growth was the per-CU scalar-cache invalidate ack
EXPOSED by a blocking in-order ACQUIRE_MEM. RADV hides it by overlapping the
flush. Replicating that overlap with a PWS RELEASE_MEM/ACQUIRE_MEM pair (plus the
mandatory wave-drain) makes the full-safety PM4 fence flat and the fastest of the
three, with no loss of correctness. New default in pm4_layer.cpp.

Artifacts: pm4_gap/pm4_layer.cpp (Emit::release_mem_pws/acquire_pws; PWS=1
default, PWS=0 blocking; CUMASK=N CU-count knob). Encodings: mesa
src/amd/registers/pkt3.json. RADV PWS refs: radv_cs.c:259-280, radv_queue.c:632-650.

--------------------------------------------------------------------------------

## 7. Artifacts

Benchmarks (in vk_gap_test/):
- vk_gap_test (main.cpp, gap.comp)  -- Vulkan/RADV inter-kernel gap (trivial kernel)
- vk_layer.cpp (+ layer_add.comp, layer_scale.comp) -- Vulkan/RADV decode-layer chain; section 6g/6h
  (env: VK_TS=1 per-dispatch GPU timestamps; VK_SAME=1 no per-dispatch rebind)
- pm4_gap/hip_layer_graph.cpp       -- HIP hipGraph pre-recorded chain (fair vs Vulkan/PM4); section 6h
- hip_gap_test.cpp                  -- HIP CPU launch loop, wave-timeline + CP-boundary
- hip_gap_graph.cpp                 -- HIP graph (record-once replay)
- hip_gap_multi.cpp                 -- HIP multi-stream concurrency sweep
- hip_gap_anyorder.cpp              -- HIP hipExtAnyOrderLaunch / barrier-bit + GAP_* envs
- hip_gap_indep.cpp                 -- single stream, independent buffers (auto nosync header)
- pm4_gap/pm4_gap.cpp               -- raw KFD PM4 compute-queue dispatch loop (bypasses AQL); section 6d
- pm4_gap/pm4_real.cpp              -- PM4 dispatch of a REAL hipcc kernel (full AMDHSA ABI); section 6e
- pm4_gap/realkern.hip              -- inc_kernel(int*) compiled to k_gfx1100.co (real-kernel ABI test)
- pm4_gap/pm4_layer.cpp             -- decode-layer chain via PM4 (multi-kernel, LDS, multi-block); section 6f
- pm4_gap/hip_layer.cpp             -- HIP/AQL reference for the same chain (same code object); section 6f
- pm4_gap/layer_kernels.hip         -- k_rms/k_scale/k_add/k_copy/k_revadd -> layer_gfx1100.co
  (k_copy stages coarse host I/O; k_revadd is the reverse-read cache-coherence stress kernel)
- layer_revadd.comp (+ .spv)        -- Vulkan reverse-read shader for CHAIN=rev (section 6l)
  CHAIN=rev KLEN=64: 64-kernel reverse-read chain (ping-pong x<->y) across PM4/HIP/Vulkan
- pm4_gap/gap_kernel.s              -- gfx1100 RAW-dependency shader (data[0]++ with glc flat access)
- pm4_gap/build.sh                  -- offline-assembles shader + builds pm4_gap (run in hipvk-isolated container)
  NOTE: links libhsakmt directly; uses kfdtest PM4 struct headers from
  rocm-systems/.../libhsakmt/tests/kfdtest/include. Run with /dev/kfd access.

Clock pinning (run as root on HOST; sysfs is RO in the container):
- gpu_pin_freq.sh                   -- disable autosuspend + pin profile_standard (973 MHz)
- gpu_restore_freq.sh               -- restore saved original state

Source patch (env-gated, isolation only):
- rocm-systems/projects/clr/rocclr/device/rocm/rocvirtual.cpp  (GAP_NOSCOPE / GAP_NOBARRIER)
- custom build: rocm-systems/projects/clr/build-gap (libamdhip64.so 7.2.53211)
- NOTE: monorepo is checked out at tag rocm-7.2.1 (detached HEAD) with the patch
  applied in the working tree. To restore: `git checkout develop` and revert the
  rocvirtual.cpp edit.

--------------------------------------------------------------------------------

## 8. Appendix A: PM4 microbenchmark -- complete design & implementation reference

This appendix is a self-contained record of the PM4 dispatch work. It explains
the idea, the architecture, every moving part with code excerpts, the fence
designs (blocking + PWS), the memory model, the chain modes, and how to rebuild
and run. The goal is that the entire PM4 effort can be reconstructed from this
section alone. Primary artifact: `pm4_gap/pm4_layer.cpp`.

### 8.1 The idea (why drive the GPU with raw PM4 at all)

Both HIP/ROCm and Vulkan/RADV ultimately push the same thing onto the same
hardware ring: a stream of PM4 packets ("Programmable Microcode 4", the GFX
command-processor packet format). HIP wraps it in the HSA AQL dispatch protocol
(an AQL packet -> ROCr translates it into PM4 on the MEC); RADV emits PM4
directly from the userspace driver. Every "framework overhead" question in this
document (Is the gap the barrier? Is it the cache flush? Is it pipeline rebind?
Is it the launch path?) eventually bottoms out in: *what PM4 does each framework
actually emit between two dependent dispatches, and how does the command
processor execute it?*

The PM4 harness removes the framework entirely. It opens a raw KFD compute queue,
hand-builds the exact PM4 packet stream (register writes + dispatch + fence), and
submits it with a single doorbell ring. That gives:

1. A **ground-truth floor**: the minimum achievable per-dispatch period on this
   GPU with nothing between us and the CP. Anything HIP/Vulkan adds on top is
   pure framework overhead.
2. **Total control of the fence**: we can emit the precise cache-coherency packet
   (ACQUIRE_MEM with an arbitrary GCR_CNTL) and the precise wait mechanism
   (blocking acquire vs PWS deferred-wait), and measure each bit's cost in
   isolation. This is what let us decompose the large-M slowdown down to a single
   bit (GLK_INV, scalar K-cache invalidate; sections 6m/6n).
3. **An apples-to-apples comparison**: the SAME compiled code object
   (`layer_gfx1100.co`) is run by PM4 and by HIP/AQL (`hip_layer`), and a
   bit-identical kernel set is run by Vulkan (`vk_layer`), so the comparison
   isolates DISPATCH overhead, not kernel codegen.

The headline outcome: a correctly built PM4 fence (CS_PARTIAL_FLUSH wave-drain +
PWS-overlapped cache flush) is flat in M and faster than both HIP/AQL and
Vulkan/RADV, while remaining bit-exact even on the reverse-read cache-coherence
stress chain.

### 8.2 File / artifact organization

```
pm4_gap/
  build.sh            Build pipeline (run INSIDE hipvk-isolated container).
  layer_kernels.hip   The 6 real kernels (k_rms,k_scale,k_add,k_copy,k_blend,k_revadd).
  layer_gfx1100.co    Unbundled gfx1100 code object (loaded raw by PM4).
  layer.co            Bundled code object (loaded by HIP via hipModuleLoad).
  pm4_layer.cpp       *** the PM4 decode-layer harness (this appendix) ***
  hip_layer.cpp       HIP/AQL reference running the SAME layer_gfx1100 kernels.
  hip_layer_graph.cpp HIP hipGraph (pre-recorded) variant for a fair vs PM4/VK.
  pm4_gap.cpp         Earlier: trivial-kernel raw PM4 dispatch loop (section 6d).
  pm4_real.cpp        Earlier: first real-kernel ABI replication proof (section 6e).
  gap_kernel.s        Hand-written gfx1100 shader for pm4_gap.
```

The harness is built on three external pieces, all present in the rocm-systems
checkout:
- `libhsakmt` (static, `/opt/rocm/lib/libhsakmt.a`): the thin KFD ioctl wrapper
  (queue create/destroy, memory alloc/map, CU mask).
- kfdtest PM4 struct headers (`.../libhsakmt/tests/kfdtest/include`): the packet
  bitfield definitions (`pm4_pkt_struct_nv.h` for GFX10/11 = "NV/Navi",
  `kfd_pm4_opcodes.h`, the `asic_reg/gfx_7_2_*` register name headers).
- `libdrm_amdgpu` for the allocation path.

### 8.3 Build pipeline (`build.sh`)

The kernels are normal hipcc, but PM4 needs the *raw* (unbundled) single-target
code object, so the build compiles with `--genco` then unbundles:

```bash
# decode-layer kernels -> bundled .co (for HIP) + unbundled gfx1100 .co (for PM4)
/opt/rocm/bin/hipcc --genco --offload-arch=gfx1100 layer_kernels.hip -o layer.co
/opt/rocm/llvm/bin/clang-offload-bundler --type=o --unbundle --input=layer.co \
    --output=layer_gfx1100.co --targets=hipv4-amdgcn-amd-amdhsa--gfx1100

# PM4 harness: plain g++, link static libhsakmt + libdrm (NO hip runtime)
g++ -O2 -std=c++17 -Wall -I$KFDINC -I/opt/rocm/include \
    pm4_layer.cpp /opt/rocm/lib/libhsakmt.a \
    $(pkg-config --libs libdrm_amdgpu libdrm) -lnuma -lpthread -lrt -o pm4_layer
```

Note pm4_layer links NO HIP/ROCr -- it is a pure KFD client. The only ROCm
dependency at runtime is the kernel driver (`/dev/kfd`) and the code object file.

### 8.4 The kernels (`layer_kernels.hip`)

Six `extern "C" __global__` kernels, all `BS=256`, deliberately written with
ONLY compile-time block size and hardware `threadIdx/blockIdx` (no `blockDim`,
`gridDim`, dynamic LDS, or printf) so the COV5 kernarg segment holds ONLY the
explicit args -- no hidden implicit-args buffer. That keeps PM4 kernarg setup
trivial (just the pointers + n).

- `k_rms(x,y,n)`   -- LDS + `__syncthreads` tree reduce; exercises group_segment
  and barriers. Bit-exact form `y = 0.5*x + 0.5*(sum(x)*2^-20)` (no rsqrt/div,
  which are toolchain-ambiguous between HIP and GLSL).
- `k_scale(y,w,n)` -- `y[i] *= w[i]`.
- `k_add(x,y,n)`   -- `x[i] += y[i]`.
- `k_copy(dst,src,n)` -- `dst=src`; the GPU equivalent of hipMemcpy used to stage
  coarse-grain host<->device I/O.
- `k_blend(dst,src,n)` -- forward `dst=0.5*dst+0.5*src[i]` (realistic decode read).
- `k_revadd(dst,src,n)` -- reverse `dst=0.5*dst+0.5*src[n-1-i]`: thread i reads
  the mirrored index written by a DIFFERENT workgroup last dispatch -> the
  cache-coherence stress kernel. If the fence does not invalidate per-WGP L0/L1,
  the read hits a stale line and the checksum diverges. This is the correctness
  oracle for the cache flush.

All blends are contractive (0.5/0.5) so thousands of iterations stay bounded and
the float checksum is deterministic across PM4/HIP/Vulkan.

### 8.5 KFD queue + ring + doorbell (the submission model)

The harness allocates a ring buffer in GPU-visible memory, writes the whole PM4
stream into it once, then submits by writing the write-pointer and ringing the
doorbell. Completion is detected by polling a sentinel that the LAST packet
writes.

```cpp
// ring sized for the whole stream (~112 dwords/dispatch incl. PWS fence)
uint64_t needDwords = 64 + (uint64_t)N*perIter*112 + 16 + (coarse?512:0);
uint64_t ringBytes  = next_pow2(needDwords*4); if(ringBytes<0x10000) ringBytes=0x10000;
void* ring = alloc_gpu(node, ringBytes, /*exec*/true, /*uncached*/true);

HsaQueueResource res; memset(&res,0,sizeof(res));
CHECK(hsaKmtCreateQueue(node, HSA_QUEUE_COMPUTE, 100, HSA_QUEUE_PRIORITY_NORMAL,
                        ring, ringBytes, nullptr, &res));
...
Emit e((uint32_t*)ring);     // build the entire stream into the ring
... emit dispatches + fences ...
e.write_data((uint64_t)sentinel, 0xC0FFEE);   // final completion marker
uint32_t total = e.idx;

// SUBMIT: single write-ptr update + doorbell ring, then spin on the sentinel.
double t0 = now_s();
*res.Queue_write_ptr_aql = total;
asm volatile("":::"memory");
*res.Queue_DoorBell_aql  = total;
while(sentinel[0] != 0xC0FFEE) { /* timeout guard */ }
double us = (now_s()-t0)*1e6;
```

Because the whole stream is pre-recorded and submitted once, the measured wall
time is GPU-side end-to-end (no per-dispatch CPU launch), which is the fair
comparison against Vulkan's pre-recorded command buffer. (HIP eager, by
contrast, is a CPU launch loop -- see 8.12 / Q4.)

### 8.6 Loading a raw code object + ABI replication

PM4 has no runtime loader, so we parse the ELF code object ourselves: map every
PT_LOAD segment into an executable GPU allocation, then walk the symbol table for
`<name>.kd` symbols (the 64-byte AMDHSA *kernel descriptor*) and copy each KD.

```cpp
struct kernel_descriptor_t {           // 64-byte AMDHSA v3 KD
    uint32_t group_segment_fixed_size, private_segment_fixed_size, kernarg_size;
    uint8_t  reserved0[4];
    int64_t  kernel_code_entry_byte_offset;   // KD VA + this = shader entry
    uint8_t  reserved1[20];
    uint32_t compute_pgm_rsrc3, compute_pgm_rsrc1, compute_pgm_rsrc2;
    uint16_t kernel_code_properties, kernarg_preload;
    uint8_t  reserved2[4];
};
```

The ABI replication (validated first in pm4_real.cpp) is: copy `rsrc1`/`rsrc2`
*verbatim* from the KD, put the kernarg pointer in `s[0:1]` (set only
`ENABLE_SGPR_KERNARG_SEGMENT_PTR`), no scratch. We reject any kernel that needs
scratch (`private_segment_fixed_size != 0` or scratch-init properties) since the
harness does not set up a scratch ring.

One subtlety the AQL CP normally handles for us: the KD's `rsrc2` does NOT carry
the static LDS size (AQL fills `LDS_SIZE` from the dispatch packet's
group_segment field). We replicate that by OR-ing the LDS granule count into
rsrc2 before writing `COMPUTE_PGM_RSRC1/2`:

```cpp
uint32_t rsrc2    = kd.compute_pgm_rsrc2;
uint32_t ldsUnits = (kd.group_segment_fixed_size + 127) / 128;   // gfx11 128B granule
rsrc2 |= (ldsUnits << COMPUTE_PGM_RSRC2__LDS_SIZE__SHIFT) & COMPUTE_PGM_RSRC2__LDS_SIZE_MASK;
```

### 8.7 The PM4 packet emitter (`struct Emit`)

`Emit` is a tiny cursor over the ring that appends Type-3 PM4 packets. Header
helper sets `type=3, shaderType=1 (compute), opcode, count=dwords-2`. Key
emitters:

- `set_sh_reg(reg, vals, n)` -- IT_SET_SH_REG: write n consecutive SH registers
  (PGM_LO/HI, RSRC1/2, USER_DATA, thread dims, resource limits).
- `dispatch_direct(x,y,z,init)` -- IT_DISPATCH_DIRECT with `dispatch_initiator`.
- `partial_flush()` -- IT_EVENT_WRITE / CS_PARTIAL_FLUSH (drain producer waves).
- `acquire_mem(gcr)` -- IT_ACQUIRE_MEM, the BLOCKING cache flush+wait.
- `release_mem_pws(gcr)` / `acquire_pws()` -- the PWS deferred-wait pair (8.10).
- `write_data(addr,val)` -- IT_WRITE_DATA to set the completion sentinel.

`emit_dispatch` ties register setup + dispatch together. Note `DISPATCH_INIT`
has `USE_THREAD_DIMS` set, so `dim_x` is the TOTAL number of threads and the CP
derives groups = ceil(threads / NUM_THREAD_X):

```cpp
static const uint32_t DISPATCH_INIT = 0x00000021 | 0x8000; // CS_EN|USE_THREAD_DIMS|CS_W32

static void emit_dispatch(Emit& e, const KInfo& ki, uint64_t kernargVA, uint32_t numThreadsX){
    const uint32_t dims[8]={0,0,0, BS,1,1, 0,0};                  // COMPUTE_NUM_THREAD_X=BS
    e.set_sh_reg(mmCOMPUTE_START_X, dims, 8);
    uint64_t entry=(ki.kdVA + ki.kd.kernel_code_entry_byte_offset)>>8;
    const uint32_t pgm[2]={(uint32_t)entry,(uint32_t)(entry>>32)};
    e.set_sh_reg(mmCOMPUTE_PGM_LO, pgm, 2);
    uint32_t rsrc2=ki.kd.compute_pgm_rsrc2 | (ldsUnits<<...);     // LDS_SIZE OR-in (8.6)
    const uint32_t rsrc[2]={ki.kd.compute_pgm_rsrc1, rsrc2};
    e.set_sh_reg(mmCOMPUTE_PGM_RSRC1, rsrc, 2);
    e.set_sh_reg(mmCOMPUTE_RESOURCE_LIMITS, &zero, 1);
    e.set_sh_reg(mmCOMPUTE_TMPRING_SIZE,   &zero, 1);
    uint32_t udata[2]={(uint32_t)kernargVA,(uint32_t)(kernargVA>>32)};
    e.set_sh_reg(mmCOMPUTE_USER_DATA_0, udata, 2);               // kernarg ptr -> s[0:1]
    e.dispatch_direct(numThreadsX, 1, 1, DISPATCH_INIT);
}
```

This per-dispatch full register re-emit is intentional: it mirrors what RADV does
(rebind the pipeline state every dispatch), so the comparison is fair.

### 8.8 The cache-coherency fence: GCR_CNTL bit layout

The whole investigation hinges on the GCR_CNTL field of ACQUIRE_MEM. Layout
(matching mesa pkt3.json / PAL):

```
GLI_INV[0:1] GL1_RANGE[2:3] GLM_WB[4] GLM_INV[5] GLK_WB[6] GLK_INV[7]
GLV_INV[8] GL1_INV[9] GL2_US[10] GL2_RANGE[11:12] GL2_DISCARD[13]
GL2_INV[14] GL2_WB[15] SEQ[16:17] RANGE_IS_PA[18]
```

Three scopes are defined:

```cpp
// RADV VkMemoryBarrier: writeback+invalidate EVERY cache incl. L2 (overkill for
// device-local dependencies). GCR_MODE=0.
GCR_RADV_LIKE  = GL2_WB|GL2_INV|GLM_WB|GLM_INV|GL1_INV|GLV_INV|GLK_INV|GLI_INV;

// AGENT scope (what ROCr/HIP use between dependent kernels on one device): L2 is
// the device coherence point and L0/L1 are write-through, so only INVALIDATE the
// per-WGP L0 vector / L1 / scalar K caches. DEFAULT (GCR_MODE=1). == 0x380.
GCR_AGENT_LIKE = GL1_INV|GLV_INV|GLK_INV;
```

- `GLV_INV` (bit 8) -- invalidate L0 vector cache (per-WGP). Mandatory for the
  reverse-read chain (reader on a different WGP must not see a stale L0 line).
- `GL1_INV` (bit 9) -- invalidate the per-shader-array L1.
- `GLK_INV` (bit 7) -- invalidate the scalar K-cache. This is the expensive bit:
  it is broadcast to every CU that ran a producer wave and the GCR engine waits
  for an ACK from each, so its cost scales ~linearly with active CU count
  (section 6n.1). Dropping it (0x380 -> 0x300) is 2.5-5x cheaper per dispatch and
  bit-exact ONLY for kernels whose kernargs are immutable and that read buffer
  DATA via VECTOR loads (verified in the ISA for these hipcc kernels). RADV must
  keep GLK_INV because its ACO compiler reads SSBOs via SMEM scalar loads.
  We keep it ON by default for safety; the PWS fence (8.10) hides its cost
  instead of dropping it.

`acquire_mem` programs ACQUIRE_MEM byte-for-byte like RADV's gfx10/11 compute
path (CP_COHER_CNTL=0, full coherence range SIZE=0xFFFFFFFF / SIZE_HI=0xFFFFFF,
BASE=0, POLL_INTERVAL=0x0A, then GCR_CNTL):

```cpp
void acquire_mem(uint32_t gcr){
    PM4ACQUIRE_MEM_NV p; memset(&p,0,sizeof(p));
    set_hdr(p.header, IT_ACQUIRE_MEM, sizeof(p)/4);
    p.reserved=0;                       // CP_COHER_CNTL
    p.coher_size=0xFFFFFFFF;            // CP_COHER_SIZE
    p.ordinal4=0x00FFFFFF;              // CP_COHER_SIZE_HI (RADV: 0xffffff)
    p.coher_base_lo=0; p.ordinal6=0;    // CP_COHER_BASE / _HI
    p.bitfields5.poll_interval=0x0A;
    p.bitfields6.gcr_cntl=gcr;
    ...
}
```

### 8.9 The blocking fence and why it grows with M

The simplest correct fence is `CS_PARTIAL_FLUSH` (drain producer waves so their
stores retire) followed by a blocking `ACQUIRE_MEM(gcr)` (flush/invalidate caches
and STALL the CP until the flush acks). This is exactly the RADV compute barrier
shape. But on a raw in-order MEC queue the ACQUIRE_MEM blocks the CP inline, so
the per-CU GLK_INV ack latency is fully EXPOSED as inter-dispatch dead time. That
is the entire source of the PM4 large-M slowdown (3 us -> 5.8 us across M);
sections 6m.4 / 6n.1 prove it is the per-CU broadcast, not the (constant ~20-byte)
scalar data. RADV does not stall the same way because GFX11 lets it overlap the
flush -- which is what PWS replicates.

### 8.10 THE FIX: the PWS deferred-wait fence

GFX11 PWS (Pre-shader / Pixel Wait Sync) decouples flush-ISSUE from flush-WAIT. A
`RELEASE_MEM` EOP event carries the GCR cache flush AND bumps a PWS counter
(issued asynchronously -- the CP does not block on it); a later PWS `ACQUIRE_MEM`
waits on that counter. So the flush overlaps the next dispatch instead of stalling
the queue. This is RADV's GFX11 overlap mechanism, replicated on the raw MEC
queue:

```cpp
// RELEASE_MEM (EVENT=BOTTOM_OF_PIPE_TS, EVENT_INDEX=5), GCR flush carried in the
// RELEASE_MEM_OP field, PWS_ENABLE=1. Bit positions are translated from the
// ACQUIRE GCR_CNTL layout to the RELEASE_MEM_OP_gfx11 layout (mesa pkt3.json).
void release_mem_pws(uint32_t gcrAcq){
    uint32_t op = 40u | (5u<<8);          // EVENT_TYPE=BOTTOM_OF_PIPE_TS, EVENT_INDEX=5
    if(gcrAcq & GLM_WB ) op |= 1u<<12;
    if(gcrAcq & GLM_INV) op |= 1u<<13;
    if(gcrAcq & GLV_INV) op |= 1u<<14;
    if(gcrAcq & GL1_INV) op |= 1u<<15;
    if(gcrAcq & GL2_INV) op |= 1u<<20;
    if(gcrAcq & GL2_WB ) op |= 1u<<21;
    if(gcrAcq & GLK_WB ) op |= 1u<<24;
    if(gcrAcq & GLK_INV) op |= 1u<<30;
    op |= ((gcrAcq>>16)&3u)<<22;          // SEQ
    op |= 1u<<31;                         // PWS_ENABLE
    ... emit RELEASE_MEM (8 dw) ...
}
// PWS ACQUIRE_MEM: wait the counter at CP_ME stage, GCR_CNTL=0 (flush already
// issued by RELEASE_MEM).
void acquire_pws(){
    ... IT_ACQUIRE_MEM (8 dw) ...
    word1 = (5u<<11)|(0u<<14)|(1u<<17)|(0u<<18); // PWS_STAGE_SEL=CP_ME, COUNTER_SEL=TS, ENA2=1
    word6 = 1u<<31;                              // PWS_ENA
    word7 = 0;                                   // GCR_CNTL = 0
}
```

The fence selection lambda -- PWS is DEFAULT, with a blocking fallback:

```cpp
int usePWS = getenv("PWS") ? atoi(getenv("PWS")) : 1;   // default ON
auto fence=[&](){
    if(usePWS){ e.partial_flush(); e.release_mem_pws(GCR); e.acquire_pws(); return; }
    if(fmode>=1) e.partial_flush();         // FENCE>=1: wave-drain
    if(fmode>=2) e.acquire_mem(GCR);        // FENCE>=2: blocking acquire
};
```

CORRECTNESS GOTCHA (hard-won): the `CS_PARTIAL_FLUSH` wave-drain is REQUIRED
before the PWS pair. Without it the RELEASE_MEM EOP does not order the consumer
against the producer's outstanding stores on a raw MEC queue, giving
non-deterministic ~1%-wrong checksums. With the drain, PWS is bit-exact and
deterministic on BOTH the serial and the reverse-read mix chains at every M.

Result (pinned, serial LAYERS=8 N=20, us/dispatch): PWS 0x380 is FLAT (2.5-2.8
us, residual growth = bandwidth only), ~2x faster than blocking, and faster than
both VK (4.5-4.7) and HIP eager (3.95-4.1) at every M, with full GLK_INV safety.

### 8.11 Memory model: replicating hipMalloc + hipMemcpy

DEFAULT (no env): compute buffers x/y/w are COARSE-grain device VRAM (NonPaged +
CoarseGrain, NO HostAccess) -- exactly what hipMalloc returns, fully L2-cached,
and not CPU-coherent. Host I/O is therefore staged exactly like hipMemcpy: inputs
live in host-accessible fine-grain staging buffers (xs/ws/ys) and are moved into
device VRAM by on-GPU `k_copy` dispatches before the chain (H2D), and the result
is copied back into an uncached staging buffer `outs` after the chain (D2H).

This default matters: an earlier raw-UNCACHED default bypassed L2 and made
large-M dependent chains up to ~20x slower, repeatedly producing
non-comparable results. Diagnostic opt-outs: `UNCACHED=1` (raw L2-bypass),
`CACHED=1` (fine-grain MTYPE_CC), `COARSE_HA=1` (keep HostAccess on coarse buffers).

```cpp
bool coarse   = !(wantUncached || wantCached || getenv("DBG_ONLY"));
bool uncached = coarse ? false : (wantCached ? false : true);
// H2D before the chain: GPU-copy staged inputs into pure device VRAM, then fence.
if(coarse){ emit_dispatch(e,kCopy,kaH2Dx,thrFull); emit_dispatch(e,kCopy,kaH2Dw,thrFull);
            e.partial_flush(); e.acquire_mem(GCR_AGENT_LIKE); }
```

### 8.12 Chain modes & kernarg slots

`CHAIN` selects the dependent chain; kernarg slots are pre-baked (32 bytes/slot:
[ptr0][ptr1][n]). The serial/mix chains bind 5 DISTINCT kernels per layer
(`LAYERS` scales to ~300 dispatches/token), rebinding every dispatch -- the
realistic decode case:

```cpp
const KInfo& kMid = serial ? kBlend : kRev;   // serial=forward read, mix=reverse-read stress
for(int l=0;l<LAYERS;++l){
    emit_dispatch(e,kCopy, kaYrevX,thrFull); fence();  // y = x
    emit_dispatch(e,kScale,kaScale,thrFull); fence();  // y *= w
    emit_dispatch(e,kMid,  kaAdd,  thrFull); fence();  // x = 0.5x + 0.5*(rev(y)|y)
    emit_dispatch(e,kAdd,  kaScale,thrFull); fence();  // y += w
    emit_dispatch(e,kMid,  kaAdd,  thrFull); fence();  // x = 0.5x + 0.5*(rev(y)|y)
}
```

- `rms` (default): k_rms -> k_scale -> k_add (3 dispatches/iter).
- `elem`: all-elementwise dependent chain on x.
- `rev`: KLEN-kernel reverse-read chain, ping-pong x<->y (cache-coherence stress).
- `mix`: 5 distinct kernels x LAYERS, reverse-read blend (stress).
- `serial`: 5 distinct kernels x LAYERS, forward blend (realistic decode).

### 8.13 Environment knobs (complete list)

```
N (argv1)         iterations (default 2000)
M (argv2)         elements per buffer (default 4096)
co (argv3)        code object (default layer_gfx1100.co)
KFD_NODE          force a specific KFD node (else first gfx11 node)
CHAIN             rms(default)|elem|rev|mix|serial
KLEN              rev chain length (default 64, forced even)
LAYERS            repeat the 5-kernel block per iter (default 1; e.g. 60 = full token)
PWS               1=PWS deferred-wait fence (DEFAULT), 0=blocking fallback
FENCE             blocking fallback level: 0=none 1=drain 2=drain+acquire (default 2)
GCR_MODE          1=AGENT 0x380 (DEFAULT), 0=RADV-like full L2 flush
GCR0              diagnostic: ACQUIRE_MEM with NO cache op
GCR_RAW=0xNNN     diagnostic: override GCR_CNTL (e.g. 0x300 drops GLK_INV)
UNCACHED / CACHED / COARSE_HA   buffer memory-type overrides (8.11)
DBG_ONLY          isolate ONE kernel once (scale|rms|add); forces CPU-writable buffers
CUMASK=N          enable only first N CUs (per-CU GLK scaling proof; section 6n.1)
SEMASK=1          write COMPUTE_STATIC_THREAD_MGMT_SE0..5 = all CUs on all 6 SEs
DBG               dump buffer samples after the run
```

### 8.14 How to rebuild & run

```bash
# build (inside the isolated container; pins libhsakmt + offline code objects)
docker exec hipvk-isolated-sshliapn bash -c \
  'cd /home/sshliapn/code/llama.cpp/vk_gap_test/pm4_gap && ./build.sh'

# run the realistic decode chain, default PWS fence, pinned clocks
docker exec hipvk-isolated-sshliapn bash -c \
  'cd .../pm4_gap && CHAIN=serial LAYERS=8 ./pm4_layer 20 65536'

# A/B the fence: blocking vs PWS at large M
CHAIN=serial LAYERS=8 PWS=0 ./pm4_layer 20 65536    # blocking (grows with M)
CHAIN=serial LAYERS=8 PWS=1 ./pm4_layer 20 65536    # PWS overlap (flat)

# GLK_INV cost decomposition (drop bit 7) and per-CU scaling
GCR_RAW=0x300 PWS=0 ./pm4_layer 20 65536            # drop GLK_INV
CUMASK=8      PWS=0 ./pm4_layer 20 65536            # restrict active CUs
```

Reference encodings: mesa `src/amd/registers/pkt3.json` (ACQUIRE_MEM,
RELEASE_MEM_OP_gfx11, ACQUIRE_MEM_PWS_*). RADV refs: `radv_cs.c`
(gfx10_cs_emit_cache_flush, PWS at ~:259-280), `radv_cmd_buffer.c`
(CS_PARTIAL_FLUSH for compute barriers), `radv_queue.c:632-650`.

