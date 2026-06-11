# AMD Vulkan Compute Barrier Internals on RDNA3 (gfx1100)

What actually happens, in hardware command terms, between two consecutive
`vkCmdDispatch` calls separated by a single global `VkMemoryBarrier`
(`srcAccessMask = SHADER_WRITE`, `dstAccessMask = SHADER_READ`, both
`COMPUTE_SHADER` stage), inside one command buffer on one compute queue, on a
Radeon PRO W7900 / gfx1100 (RDNA3, GFX11).

Investigated from source for both open-source AMD Vulkan drivers.

Sources read (shallow clones, commits pinned):
- **RADV / Mesa** `src/amd` @ `864bee8` (gitlab.freedesktop.org/mesa/mesa)
- **PAL** @ `c5e8000` (github.com/GPUOpen-Drivers/pal)
- **XGL (AMDVLK ICD)** @ `e9782eb` (github.com/GPUOpen-Drivers/xgl)

Background note used throughout: on RDNA the cache hierarchy per CU is
**L0 vector (TCP, "GLV/V$")**, **scalar L0 (SQC/K$, "GLK")**, a shader-array
**GL1**, and the device-wide **GL2 == TCC (L2)** plus its metadata side-cache
**GLM**. GL2/TCC is the device coherence point: all shader and RB clients read
and write through GL2, so compute-to-compute coherence only requires
invalidating the *reader-side* L0/L1 caches once the producer's writes have
reached GL2 -- a GL2 writeback/invalidate is **not** fundamentally required.

---

## A. Which hardware cache operations are emitted?

### RADV (Mesa)

The barrier is split into a "source" (visibility) and "destination"
(availability) cache-flag computation, then lazily emitted as a single GCR
(global cache request) packet at the next dispatch.

**Source side** -- `radv_src_access_flush()`,
`src/amd/vulkan/radv_cmd_buffer.c:7548`:

```7548:7562:src/amd/vulkan/radv_cmd_buffer.c
   if (src_flags & (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)) {
      ...
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }
```

For a global `VkMemoryBarrier` (and for `VkBufferMemoryBarrier` too -- RADV
passes `image = NULL` for both, `radv_cmd_buffer.c:15265` and `:15275`),
`image_is_coherent` is false, so the **producer side unconditionally requests
`INV_L2` (GL2 writeback + invalidate)**. The driver comment at
`radv_cmd_buffer.c:7520` says this is deliberate/conservative ("we always
invalidate L2 on the src side ... we can use our knowledge of past usage to
optimize flushes away").

**Destination side** -- `radv_dst_access_flush()`,
`src/amd/vulkan/radv_cmd_buffer.c:7670`:

```7670:7685:src/amd/vulkan/radv_cmd_buffer.c
   if (dst_flags & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | ...)) {
      ...
         if (!pdev->use_llvm && !image)
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE;   /* ACO uses SMEM for SSBOs */
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
      if (flush_L2_metadata)
         flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }
```

On the dst side `image_is_coherent` is forced true for buffers on GFX10+ via
`can_skip_buffer_l2_flushes()` (`radv_cmd_buffer.c:7490`, true when
`gfx_level >= GFX10 && !tcc_rb_non_coherent`) at line 7631, so the dst side does
**not** add a second `INV_L2`; it adds `INV_VCACHE` (V$/TCP) and `INV_SCACHE`
(K$/SQC, because ACO loads SSBOs via SMEM).

**Translation to hardware** -- `gfx10_cs_emit_cache_flush()`,
`src/amd/vulkan/radv_cs.c:37`. The flush bits become a single GCR field
(`gcr_cntl`) plus an event:

```52:85:src/amd/vulkan/radv_cs.c
   if (flush_bits & RADV_CMD_FLAG_INV_SCACHE) { gcr_cntl |= S_587_GLK_INV(1); ... }
   if (flush_bits & RADV_CMD_FLAG_INV_VCACHE) { gcr_cntl |= S_587_GLV_INV(1); ... }
   if (flush_bits & (RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE) && gfx_level < GFX12) {
      gcr_cntl |= S_587_GL1_INV(1); ... }
   if (flush_bits & RADV_CMD_FLAG_INV_L2) {
      gcr_cntl |= S_587_GL2_INV(1) | S_587_GL2_WB(1);   /* writeback AND invalidate */
   } ...
   if (gfx_level < GFX12 && (flush_bits & (RADV_CMD_FLAG_INV_L2 | ...))) {
      gcr_cntl |= S_587_GLM_INV(1) | S_587_GLM_WB(1);   /* metadata cache */
   }
```

The `gcr_cntl` word is emitted on the compute (MEC) queue as an
**`ACQUIRE_MEM`** packet (`radv_cs.c:210-211`, `ac_emit_cp_acquire_mem`, engine
`PREFETCH_PARSER`); CB/DB/RB paths are stripped on compute queues at
`radv_cmd_buffer.c:15183-15188`. The wave-drain event (next section) is a
separate `CS_PARTIAL_FLUSH` event write.

**RADV verdict for SHADER_WRITE -> SHADER_READ compute/compute:**
`ACQUIRE_MEM` with **GLV_INV (vector L0/TCP) + GL1_INV + GLK_INV (scalar K$) +
GL2_INV + GL2_WB (L2/TCC writeback+invalidate) + GLM_INV/WB (L2 metadata)**, and
a separate **`CS_PARTIAL_FLUSH`** event. No RELEASE_MEM/EOP, no CB/DB flush on
the compute queue. Note RADV **does flush+invalidate L2/TCC** here even though
on RDNA that is not strictly necessary for compute/compute coherence -- it is a
deliberate conservative choice on the producer side, and RADV cannot scope it
away for buffers because it does not track per-buffer L2 residency (image is
always `NULL`).

### PAL (AMDVLK)

The translation `VkAccess -> Pal coherency flags -> GCR` lives in
`src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp`
(`BarrierMgr::GetCacheSyncOps`, line ~278). For `CoherShaderWrite -> CoherShaderRead`:

```347:365:src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp
   if (TestAnyFlagSet(dstAccessMask, dstCacheInvMask) && (canSkipCacheInv == false))
   {
      cacheOps.glxFlags |= SyncGlkInv | SyncGlvInv | SyncGl1Inv | SyncGlmInv;
   }
   // dstAccessMask == 0 is for split barrier, assume the worst case.
   if (TestAnyFlagSet(srcAccessMask, CacheCoherBypassGl2AccessMask) && ...) {
      cacheOps.glxFlags |= SyncGl2Inv;
      cacheOps.glxFlags |= SyncGl2Wb;
   }
```

- The reader-side invalidate is **GLK (scalar) + GLV (vector L0/TCP) + GL1 +
  GLM (L2 metadata)** -- the same set RADV uses, minus the L2 body.
- The GL2 (TCC) writeback/invalidate is gated on
  `CacheCoherBypassGl2AccessMask = CoherCpu | CoherMemory | CoherPresent`
  (line 68). `CoherShaderWrite`/`CoherShaderRead` are **not** in that mask, so
  for a shader/shader transition PAL does **not** flush GL2 here. This matches
  the RDNA coherence model (GL2 is the coherence point).
- **Exception -- the global path.** A `VkMemoryBarrier` is routed by XGL into
  PAL's *global* access masks (BarrierType::Global, see section evidence below).
  The "misaligned-metadata workaround" block then fires because
  `WaRefreshTccCoherMask` includes `CoherShaderWrite` and the condition is taken
  unconditionally for `BarrierType::Global`:

```407:430:src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp
   if (TestAnyFlagSet(waSrcAccessMask, WaRefreshTccCoherMask) &&
       ((barrierType == BarrierType::Global) || ...))
   {
      ...
      if ((backToBackDirectWrite == false) && (backToBackIndirectWrite == false))
         cacheOps.glxFlags |= SyncGl2WbInv;
   }
```

  So for a *global* `VkMemoryBarrier` with a write source PAL **also adds a GL2
  writeback+invalidate** (`SyncGl2WbInv`), just like RADV. For a *buffer-scoped*
  `VkBufferMemoryBarrier` (BarrierType::Buffer, no metadata) this block is not
  taken and **GL2 is skipped** -- PAL is finer-grained than RADV here.

The `glxFlags` are emitted as an **`ACQUIRE_MEM`** packet
(`AcquireMemGeneric` / `BuildAcquireMemGeneric`,
`gfx9AcquireReleaseBarrier.cpp:2037-2041`), preceded by the wave-drain event.

**PAL verdict:** identical reader-side L0/L1/K$/GLM invalidate via `ACQUIRE_MEM`
+ a `CS_PARTIAL_FLUSH` wave drain; GL2/TCC writeback+invalidate **only for the
global VkMemoryBarrier path** (workaround), skipped for buffer-scoped barriers.

### A -- summary
| op | RADV (global or buffer) | PAL global VkMemoryBarrier | PAL VkBufferMemoryBarrier |
|---|---|---|---|
| Vector L0 (TCP/GLV) inv | yes | yes | yes |
| GL1 inv | yes | yes | yes |
| Scalar K$ (SQC/GLK) inv | yes (ACO) | yes | yes |
| L2 metadata (GLM) inv/wb | yes | yes | yes |
| **GL2/TCC wb+inv** | **yes (conservative)** | **yes (Wa)** | **no** |
| CS_PARTIAL_FLUSH (wave drain) | yes | yes | yes |
| RB/CB/DB flush | no (stripped on compute) | no | no |
| ACQUIRE_MEM packet | yes | yes | yes |
| RELEASE_MEM / EOP TS | no | no (uses CS_PARTIAL_FLUSH) | no |

An L2/TCC flush is **not architecturally required** for compute->compute on RDNA
(GL2 is the coherence point); both drivers nonetheless emit it for a *global*
memory barrier as a conservative/workaround choice. Only PAL's buffer-scoped
path actually skips it.

---

## B. Does the barrier force a full wait-for-idle (drain dispatch N) before N+1?

**Yes -- mandatory `CS_PARTIAL_FLUSH` on the producer side. Waves of N+1 cannot
start launching until all waves of N have retired.**

### RADV
`radv_stage_flush()` adds `CS_PARTIAL_FLUSH` whenever the source stage is
compute:

```7465:7472:src/amd/vulkan/radv_cmd_buffer.c
   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | ...)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;
   }
```

It is emitted as a `CS_PARTIAL_FLUSH` event (`radv_cs.c:143-149`). The dispatch
path documents the resulting state directly -- after the flush the CUs are
idle:

```14356:14361:src/amd/vulkan/radv_cmd_buffer.c
   if (cs == cmd_buffer->cs)
      radv_emit_cache_flush(cmd_buffer);
   ...
   /* <-- CUs are idle here if shaders are synchronized. */
```

### PAL
`GetReleaseEvents()` sets `release.cs` for a compute source acquired at the ME
stage (the compute-engine acquire point for a `CS` destination):

```470:482:src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp
   if (TestAnyFlagSet(srcStageMask, StallReqStageMask[acquirePoint])) {
      ...
      release.cs = TestAnyFlagSet(srcStageMask, CsWaitStageMask);
      ...
   }
```
which becomes a `WriteWaitCsIdle()` (`gfx9AcquireReleaseBarrier.cpp:2027-2031`),
and `BuildWaitCsIdle` emits a **`CS_PARTIAL_FLUSH`** on the compute engine:

```4298:4302:src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp
   if (CanUseCsPartialFlush(engineType))
      totalSize = BuildNonSampleEventWrite(CS_PARTIAL_FLUSH, engineType, pBuffer);
```

`CS_PARTIAL_FLUSH` blocks the CP at that point until every outstanding compute
wave has completed. The reader caches are then invalidated by the following
`ACQUIRE_MEM`. So there is **hard serialization**: N's waves fully drain, caches
invalidate, then N+1 launches. N+1's waves cannot overlap N's tail.
(On GFX11 with PWS enabled the same CsDone wait can instead be expressed as a
deferred `RELEASE_MEM`+`ACQUIRE_MEM`-PWS pair allowing a later acquire point for
*graphics* destinations, but for a CS destination on a compute queue it still
resolves to waiting for CsDone before N+1's waves run --
`gfx9AcquireReleaseBarrier.cpp:1683-1709`, `:1961`.)

---

## C. Fixed per-dispatch CP prelude cost

Per `vkCmdDispatch`, independent of grid size:

### RADV (`radv_compute_dispatch` -> `radv_before_dispatch` ->
`radv_emit_dispatch_packets`)
- **Pipeline registers only if the pipeline changed** (`RADV_CMD_DIRTY_COMPUTE_PIPELINE`):
  `radv_emit_compute_pipeline()` reserves ~25 dwords (`radv_cmd_buffer.c:8566`)
  to `SET_SH_REG` COMPUTE_PGM_LO/HI, RSRC1/2/3, NUM_THREAD_X/Y/Z and resource
  limits. For back-to-back dispatches of the *same* pipeline this is **skipped**
  (`radv_cmd_buffer.c:14346-14349`).
- **Changed user data / push constants / descriptor pointers** via `SET_SH_REG`
  (dirty-tracked; only deltas are emitted).
- **Grid size**: a few-dword `SET_SH_REG` of the grid SGPR when used.
- **The dispatch itself**: one `PKT3_DISPATCH_DIRECT` = **5 dwords**
  (`radv_cmd_buffer.c:14263-14269`): 3x block dims + `DISPATCH_INITIATOR`.

```14263:14269:src/amd/vulkan/radv_cmd_buffer.c
   radeon_emit(PKT3(PKT3_DISPATCH_DIRECT, 3, predicating) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(blocks[0]);
   radeon_emit(blocks[1]);
   radeon_emit(blocks[2]);
   radeon_emit(dispatch_initiator);
```

### PAL (`ComputeCmdBuffer::CmdDispatch` -> `ValidateDispatchPalAbi` ->
`BuildDispatchDirect`)
Same structure, with dirty tracking:
```675:707:src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp
   if (m_computeState.pipelineState.dirtyFlags.pipeline) {
      pCmdSpace = pNewPipeline->WriteCommands<true>(...);   // full reg set, only if pipeline changed
      ...
      pCmdSpace = ValidateUserData<true>(...);
   } else {
      pCmdSpace = ValidateUserData<false>(...);             // only changed user-data SGPRs
   }
```
plus a `SET_SH_REG` of the `numWorkGroups` SGPR pair (direct dispatch packs the
grid into embedded data, `gfx9ComputeCmdBuffer.cpp:719-734`) and finally
`BuildDispatchDirect` (the `DISPATCH_DIRECT` packet, `gfx9ComputeCmdBuffer.cpp:246`).

**Magnitude:** the *fixed* per-dispatch CP cost when the pipeline is unchanged is
small -- on the order of a `DISPATCH_DIRECT` (5 dwords) plus a handful of changed
`SET_SH_REG` words (grid + any changed user data). A pipeline switch adds the
~20-30 dword register block. This is tens of dwords the CP parses in a few CP
clocks -- it is **not** the dominant inter-dispatch cost; the cache invalidate
latency and the wave-drain tail (section B) dominate the gap.

---

## D. Can the CP prefetch/overlap N+1's register setup with N's drain? (KEY)

**Partially -- and it does not remove the bubble.** Pre-recording many
dispatch+barrier sequences in one command buffer lets the CP *fetch and parse*
the PM4 for N+1 (it streams the ring continuously and has a PFP prefetch stage),
so the **host-side launch cost is fully amortized** -- there is no per-dispatch
CPU round trip, doorbell, or queue-empty stall between N and N+1.

**But the barrier's `CS_PARTIAL_FLUSH` is a hard execution barrier inside the
CP/SPI, not just a fetch dependency.** When the CP reaches the `CS_PARTIAL_FLUSH`
it stops dispatching new threadgroups until SPI reports all of dispatch N's
waves retired; only then does the following `ACQUIRE_MEM` invalidate the reader
caches, and only then can N+1's threadgroups be launched. RADV states this
plainly: `/* <-- CUs are idle here if shaders are synchronized. */`
(`radv_cmd_buffer.c:14361`).

Concretely, what *can* and *cannot* be hidden:
- **Hidden by batching:** parsing N+1's `SET_SH_REG`/`DISPATCH_DIRECT` packets,
  building command buffers, host submit latency, doorbell/queue-empty stalls.
  The CP can have N+1's register writes parsed and waiting.
- **NOT hidden:** the wave-drain *tail* of N (the time from "last threadgroup
  issued" to "last wave retired", i.e. the ramp-down where occupancy falls to
  zero) **plus** the GCR cache-invalidate latency. Because N+1's waves are gated
  behind CsDone, the GPU necessarily goes to ~zero compute occupancy between
  every barrier-separated dispatch. That idle notch is the irreducible
  inter-kernel gap, and it is the same whether the dispatches are pre-recorded
  or launched one at a time.

So **batching/pre-recording hides the host launch overhead but does NOT hide the
barrier bubble.** With a `SHADER_WRITE -> SHADER_READ` barrier between every
dispatch, RDNA3 serializes: drain N -> invalidate L0/L1(/L2) -> launch N+1, and
the drain+invalidate is exposed on the GPU timeline every time. The only way to
hide it is to remove or weaken the barrier (e.g. no true RAW dependency, or
overlapping independent dispatches on different queues), not to batch.

---

## E. Comparison with the HIP/HSA compute dispatch path

> Note: PAL/RADV/XGL were read from source. The ROCR/HSA runtime and CP
> microcode were **not** available to clone in this environment (web access was
> blocked), so this section is architectural reasoning grounded in (1) the AQL
> kernel-dispatch packet definition and (2) PAL's own HSA-ABI dispatch path
> (`ComputeCmdBuffer::ValidateDispatchHsaAbi`, `gfx9ComputeCmdBuffer.cpp:749`),
> which compiles the *same* SGPR setup + `DISPATCH_DIRECT` for an HSA kernel.
> Points that are inference are labeled.

**Same silicon, same microengine.** On gfx1100 a Vulkan/PAL compute queue and a
HIP/HSA compute queue are both serviced by the MEC/ACE microengines. HSA AQL
`hsa_kernel_dispatch_packet_t`s are consumed by the CP's AQL packet processor
and translated into the *same* PM4 primitives: register/SGPR setup, a
`DISPATCH_DIRECT`-equivalent, and GCR cache `ACQUIRE_MEM`/`RELEASE_MEM` ops.
PAL's `ValidateDispatchHsaAbi` writing the kernarg pointer + dims + the same
dispatch packet is direct evidence that the HSA ABI funnels into identical
hardware commands.

**Cache + drain semantics map 1:1.** The AQL packet header carries an
**acquire fence scope** and a **release fence scope** (NONE / AGENT / SYSTEM).
For a normal data-dependent kernel sequence the runtime uses **agent scope**,
which the packet processor realizes as the same GCR operations Vulkan emits:
- acquire (agent) ~ invalidate vector L0 / GL1 / scalar K$ before the kernel
  (equivalent to the Vulkan dst-side `ACQUIRE_MEM` GLV/GL1/GLK invalidate);
- release (agent) ~ write back/invalidate caches to the GL2 coherence point
  after the kernel.
A producer kernel's waves must retire before the dependent kernel's acquire and
launch -- the AQL "barrier bit" / queue ordering plus the release fence enforce
exactly the `CS_PARTIAL_FLUSH`-then-invalidate behavior of the Vulkan barrier.

**Therefore the per-event hardware cost is the same.** The cache-op set + CP
prelude + wave-drain tail for one barrier-separated dispatch is essentially
identical between a Vulkan pre-recorded dispatch and a HIP kernel launch, because
it is the same hardware doing the same GCR + SPI drain. (Inference, but strongly
supported by the shared microengine and shared GCR machinery.)

**Where a HIP per-launch can show a *larger* gap -- and why it is not
fundamental:**
1. **Host-side launch latency / queue-empty bubbles.** If kernels are launched
   one at a time and the host cannot keep the AQL queue full, the CP runs dry
   between kernels and the GPU idles waiting for the next packet + doorbell. A
   pre-recorded Vulkan command buffer never has this gap because all
   dispatch+barrier packets are already resident and streamed back-to-back. This
   is the dominant, *avoidable* difference, and HIP closes it with batching
   (HIP graphs / pre-enqueued packets / a persistently full queue).
2. **Fence scope.** If the HIP path (or a default stream policy) issues a heavier
   **SYSTEM**-scope release/acquire, the packet processor would add a full
   **GL2/TCC** writeback+invalidate that an AGENT-scope barrier (and PAL's
   buffer-scoped Vulkan barrier) would skip -- making the HIP gap larger by the
   L2 flush latency. With agent scope the costs converge. (Inference.)

**Bottom line for E:** there is **no structural reason** the *hardware* cost of a
single SHADER_WRITE->SHADER_READ compute boundary differs between HIP and Vulkan
on gfx1100 -- same drain, same GCR invalidates. A HIP per-kernel launch exposes a
*larger* gap only to the extent that (a) host launch latency / queue-empty
stalls are not amortized (fixed by batching/graphs), or (b) it uses a wider fence
scope (system vs agent) that adds an L2 flush. When dispatches are batched and
use agent scope, the HIP gap should converge to the Vulkan pre-recorded gap.

---

## Bottom line on the inter-kernel gap

- A compute->compute `SHADER_WRITE -> SHADER_READ` barrier on gfx1100 compiles,
  in both RADV and PAL, to: **(1) a `CS_PARTIAL_FLUSH` that drains all producer
  waves, then (2) one `ACQUIRE_MEM` GCR packet that invalidates vector L0 (TCP),
  GL1, scalar K$ (SQC), and L2 metadata (GLM)** on the reader side. No RB/CB/DB
  work and no EOP timestamp on the compute queue.
- **L2/TCC is the RDNA coherence point, so an L2 flush is not architecturally
  required for compute/compute.** Yet a *global* `VkMemoryBarrier` triggers a
  full **GL2 writeback+invalidate** in both drivers (RADV: always on the src
  side; PAL: via the metadata-misalignment workaround on the global path). Only
  PAL's *buffer-scoped* `VkBufferMemoryBarrier` actually skips the L2 flush.
  Using buffer/scoped barriers on AMDVLK is therefore measurably cheaper than a
  blanket global memory barrier; RADV cannot scope it away.
- The barrier is **hard serialization**: N's waves fully retire and reader caches
  invalidate before N+1's waves launch. The GPU occupancy goes to ~zero at every
  barrier.
- **Batching/pre-recording many dispatches in one command buffer hides the host
  launch overhead and lets the CP pre-parse N+1's tiny register prelude, but it
  does NOT hide the barrier bubble.** The wave-drain tail + cache-invalidate
  latency between every dispatch is exposed on the GPU timeline regardless of
  batching. (Answer to D.)
- **HIP vs Vulkan: the per-event hardware cost is the same** (same MEC, same GCR
  invalidates, same SPI drain). A HIP launch only looks worse when host launch
  latency / queue-empty stalls are not amortized, or when a wider (system) fence
  scope forces an extra L2 flush. Batching + agent-scope fences make them
  converge. (Answer to E.)
- The fixed CP prelude per dispatch (when the pipeline is unchanged) is tiny -- a
  `DISPATCH_DIRECT` (5 dwords) plus a few changed `SET_SH_REG` words -- and is
  not the source of the gap. The gap is dominated by **wave drain + cache
  invalidate**, which the barrier makes unavoidable.

### Explicitly not determined from source
- Whether `tcc_rb_non_coherent` is set on the specific W7900 firmware/kernel
  (affects only the RADV dst-side L2-skip optimization, not the src-side L2 wb).
- Whether PAL's `WaRefreshTcc` global-path GL2 flush is additionally gated by a
  per-ASIC setting on gfx1100 -- the code path as written takes it for any
  global write source; I did not trace a settings override.
- The HSA/HIP side (section E) was reasoned from the AQL packet model and PAL's
  HSA-ABI dispatch path; ROCR runtime + CP microcode were not read (no repo
  access in this environment).
