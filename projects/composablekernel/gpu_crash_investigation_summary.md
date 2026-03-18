# GPU Crash Investigation: ATHUB_INTERRUPT in GEMM HotLoopScheduler

## Table of Contents

1. [Original Issue](#1-original-issue)
2. [Environment and Reproduction](#2-environment-and-reproduction)
3. [Initial Experiments (Tests 0-5)](#3-initial-experiments-tests-0-5)
4. [Assembly-Level Analysis](#4-assembly-level-analysis)
5. [Isolation Experiments (Tests 6-8)](#5-isolation-experiments-tests-6-8)
6. [Root Cause](#6-root-cause)
7. [Suspected Code](#7-suspected-code)
8. [Suggested Confirmation Test](#8-suggested-confirmation-test)

---

## 1. Original Issue

Running a GEMM kernel on **MI350X (gfx950)** causes a catastrophic GPU failure:
the GPU is lost from the PCIe bus, triggers hardware error recovery, and
in some cases forces a full system reboot.

### Kernel log (abbreviated)

```
[  918.654913] amdgpu 0000:15:00.0: {1}uncorrectable hardware error(ERREVENT_ATHUB_INTERRUPT) detected!
[  918.664265] pcieport 0000:72:00.0: pciehp: Slot(15): Link Down
[  918.664272] pcieport 0000:72:00.0: pciehp: Slot(15): Card not present
[  918.664311] amdgpu 0000:75:00.0: amdgpu: Partial read for checksum, res:-13
[  918.664496] amdgpu 0000:75:00.0: amdgpu: RAS table incorrect checksum or error:-13, try to recover
[  918.664502] amdgpu 0000:75:00.0: amdgpu: Failed to write EEPROM table header:-13
[  918.682637] amdgpu 0000:75:00.0: amdgpu: VM memory stats for proc (0) task (0) is non-zero when fini
[  918.683393] amdgpu 0000:75:00.0: amdgpu: amdgpu: finishing device.
[  919.159699] amdgpu 0000:75:00.0: amdgpu: device lost from bus!
[  919.171504] amdgpu 0000:15:00.0: amdgpu: GPU reset begin!. Source:  2
[  919.172001] amdgpu 0000:15:00.0: amdgpu: GPU reset end with ret = -19
```

### Key characteristics

- **Not an immediate hang.** The kernel completes successfully for some
  invocations before eventually crashing. With `--warmup 50 --repeat 100`
  the crash occurs somewhere during the 150 total invocations.
- **Shape-dependent.** Triggered by m=4352, n=4096, k=16384. Smaller
  shapes (e.g. 4096x4096x4096) work fine.
- **Layout-independent.** Reproduces with both RowMajor B and ColumnMajor B.
- **Kernel-type-independent.** Both persistent and non-persistent GEMM
  variants crash. Investigation focused on the non-persistent variant.
- **Scheduler-dependent.** Crashes only when `HotLoopScheduler()` is
  enabled in the hot loop body. Disabling it in the hot loop (while
  keeping it in the peeled last iteration) eliminates the crash.

---

## 2. Environment and Reproduction

### Hardware

- **GPU:** AMD Instinct MI350X (gfx950 / CDNA4)

### Reproducer command

```bash
./bin/tile_example_gemm_universal_persistent_all_reduce_ref_default \
  --m 4352 --n 4096 --k 16384 \
  --config 1 --warmup 50 --repeat 100 \
  --verify 0 --compare 0 \
  --tokens_per_reduction 0 \
  --skip_nccl 1 \
  --sync_each 1 --non_persistent 1
```

### Source file under investigation

```
include/ck_tile/ops/gemm/pipeline/gemm_pipeline_ag_bg_cr_comp_v3.hpp
```

### Disassembly directory

```
gemm_default_tests_isolated_cfg1_disasm/
```

### GEMM tile parameters for config 1

| Parameter    | Value |
|------------- |-------|
| MPerBlock    | 256   |
| NPerBlock    | 256   |
| KPerBlock    | 64    |
| MPerXDL      | 32    |
| NPerXDL      | 32    |
| KPerXDL      | 16    |
| WaveSize     | 64    |
| BlockSize    | 256   |
| WaveNumM     | 2     |
| WaveNumN     | 2     |
| PrefetchStages / GlobalBufferNum | 2 (double buffered) |

With K=16384 and KPerBlock=64: `num_loop = 256`, hot loop runs 254 iterations.

---

## 3. Initial Experiments (Tests 0-5)

Each test toggles `HotLoopScheduler()` in two locations:
- **Hot loop** (line 623 in source): the main `while(i < num_loop - 2)` body
- **Last iteration** (line 664 in source): the peeled final iteration

`__builtin_amdgcn_sched_barrier(0)` (line 624/665) was **always present**
in every test. With mask=0 and no active scheduling groups, this intrinsic
is effectively a no-op — the compiler ignores it.

| Test | Hot Loop Scheduler | Last Iter Scheduler | B Layout    | Result       |
|------|--------------------|---------------------|-------------|--------------|
| 0    | DISABLED           | DISABLED            | RowMajor    | **WORKS**    |
| 1    | ENABLED            | ENABLED             | RowMajor    | **CRASH**    |
| 2    | ENABLED            | DISABLED            | RowMajor    | **CRASH**    |
| 3    | DISABLED           | ENABLED             | RowMajor    | **WORKS**    |
| 4    | DISABLED           | DISABLED            | ColumnMajor | **WORKS**    |
| 5    | ENABLED            | ENABLED             | ColumnMajor | **CRASH**    |

### Conclusions from Tests 0-5

1. The crash is caused **exclusively** by `HotLoopScheduler()` being active
   in the **hot loop body** (Tests 1, 2, 5 crash; Tests 0, 3, 4 work).
2. The peeled last iteration's scheduler is **harmless** (Test 3 works).
3. B matrix layout is irrelevant (both RM and CM crash).
4. `__builtin_amdgcn_sched_barrier(0)` alone does not prevent the crash
   and does not prevent loop rotation — it is a no-op without active
   scheduling groups.

---

## 4. Assembly-Level Analysis

### Disassembly files examined

| File | Configuration |
|------|---------------|
| `RM_CM_RM_both_schedulers_enabled_non_persistent_gemm.asm` | CM B, scheduler enabled (crashes) |
| `RM_CM_RM_both_schedulers_disabled_non_persistent_gemm.asm` | CM B, scheduler disabled (works) |
| `RM_RM_RM_both_schedulers_enabled_non_persistent_gemm.asm` | RM B, scheduler enabled (crashes) |
| `RM_CM_RM_both_VMEM_read_commented_out_non_persistent_gemm.asm` | CM B, VMEM_READ groups removed (works) |
| `RM_CM_RM_stage2_skipped_non_persistent_gemm.asm` | CM B, Stage 1 only (crashes) |
| `RM_CM_RM_stage1_skipped_non_persistent_gemm.asm` | CM B, Stage 2 only (works) |

### Checks performed on the crashing assembly (CM B, scheduler enabled)

The following areas were exhaustively verified in the hot loop assembly and
found to be **correct** — none of these are the root cause:

1. **Instruction counts.** 16 `ds_write_b128`, 16 `buffer_load_dwordx4`,
   64 `v_mfma_f32_32x32x16_f16`, and 32 `ds_read_b128` per hot loop
   iteration. All match the `HotLoopScheduler` calculations.

2. **`vmcnt` correctness.** Each `s_waitcnt vmcnt(15)` in the hot loop is
   issued with exactly 16 pending VMEM operations, correctly draining the
   oldest load before the subsequent `ds_write` consumes its data.

3. **RAW (Read-After-Write) data hazards.** Every `ds_write` reads from
   VGPRs whose `buffer_load` has completed (guaranteed by the preceding
   `vmcnt` wait). No stale data reads.

4. **WAR (Write-After-Read) hazards in VGPR reuse.** The `buffer_load`
   destinations (e.g. `v[168:171]`) are not overwritten until after the
   `ds_write` has consumed their data. No clobbering.

5. **MFMA operand safety.** All `v_mfma` instructions read from VGPRs
   that are stable (not being overwritten by concurrent loads). Accumulators
   in AGPRs (`a[0:255]`) are correctly partitioned.

6. **`s_barrier` placement.** `s_waitcnt lgkmcnt(0)` followed by `s_barrier`
   appears at the correct position between the ds_write phase (LDS store)
   and the ds_read phase (LDS load), ensuring workgroup synchronization.

7. **Address computation correctness.** Offset VGPRs for `buffer_load` and
   `ds_write` are correctly computed using shift/add/xor sequences. SRD
   registers (`s[8:11]` for A, `s[12:15]` for B) are used consistently.

8. **No register spilling.** No `buffer_store_dword` / `buffer_load_dword`
   to scratch space within the hot loop. The kernel does not spill to
   memory.

9. **`ds_write` data consumption order.** The VGPR ranges consumed by
   `ds_write` instructions perfectly match the FIFO order of `buffer_load`
   issue, ensuring the `vmcnt(15)` wait targets the correct load.

10. **Peeled iteration drain sequence.** The peeled last iteration correctly
    uses a decrementing `vmcnt` sequence (15 → 0) to drain all pending VMEM
    loads without issuing new ones.

### Key structural difference: hot loop memory patterns

**Crashing version (scheduler enabled):**

The `HotLoopScheduler` groups force each `buffer_load` to be interleaved
1:1 with `ds_write` operations:

```
vmcnt(15) → ds_write → buffer_load → MFMA → MFMA →
vmcnt(15) → ds_write → buffer_load → MFMA → MFMA →
vmcnt(15) → ds_write → buffer_load → MFMA → MFMA →
... (×16)
lgkmcnt(0) → s_barrier → ds_reads ...
```

Each `vmcnt(15)` drains one old load; each `buffer_load` adds one new load.
The pending VMEM count is **constantly 16**, never dropping below 15. Old
loads being completed and new loads being issued overlap in time continuously.

**Working version (scheduler disabled):**

The compiler, free from scheduling group constraints, chooses a
drain-then-burst pattern:

```
vmcnt(15) → ds_write → MFMA →
vmcnt(14) → ds_write → MFMA →
vmcnt(13) → ds_write → MFMA →
...
vmcnt(0)  → ds_write → MFMA →
(all old loads fully drained — pending VMEM count = 0)
buffer_load ×16 (burst — pending count jumps to 16)
lgkmcnt(0) → s_barrier → ds_reads ...
```

All 16 old `buffer_load` results are consumed first (vmcnt drains from 15
down to 0), and only then are 16 new `buffer_load`s issued as a burst.
There is a **period of zero pending VMEM operations** between the drain
and the burst.

---

## 5. Isolation Experiments (Tests 6-8)

These tests were designed to identify which specific scheduling group type
within `HotLoopScheduler` triggers the crash.

### HotLoopScheduler structure

The scheduler emits scheduling groups in two stages:

**Stage 1** (global prefetch + LDS store): for each of the 16 `buffer_load`
instructions, emits:
- `0x200` (DS_WRITE) × `num_dswrite_per_issue` — one ds_write per group
- `0x008` (MFMA) × 1 — one MFMA per ds_write
- **`0x020` (VMEM_READ) × 1** — one buffer_load
- `0x008` (MFMA) × remaining — fill remaining MFMAs

**Stage 2** (LDS read + compute): DS_READ groups interleaved with MFMA
groups (currently commented out in source, no effect).

### Test results

| Test | Modification | Result |
|------|-------------|--------|
| 6    | Remove **VMEM_READ groups** (`0x020`) from Stage 1, keep DS_WRITE + MFMA groups | **WORKS** |
| 6.1  | Transpose-read fixes only (keeping VMEM_READ groups) | **CRASH** |
| 7    | Remove **Stage 1 entirely**, keep Stage 2 only | **WORKS** |
| 8    | Remove **Stage 2 entirely**, keep Stage 1 (with VMEM_READ) | **CRASH** |

### Assembly files for isolation tests

| Test | Assembly file |
|------|---------------|
| 6 | `RM_CM_RM_both_VMEM_read_commented_out_non_persistent_gemm.asm` |
| 7 | `RM_CM_RM_stage1_skipped_non_persistent_gemm.asm` |
| 8 | `RM_CM_RM_stage2_skipped_non_persistent_gemm.asm` |

### Conclusions from isolation tests

1. **Stage 2 (DS_READ groups) is harmless** (Test 7 works).
2. **Stage 1 DS_WRITE + MFMA groups alone are harmless** (Test 6 works).
3. **The `0x020` VMEM_READ groups are the sole trigger** (Test 6 vs 6.1,
   Test 8 crashes).
4. The ds_read transposed instruction count fix does not affect the
   outcome (Test 6.1 still crashes with VMEM_READ present).

### Assembly verification of Test 6 (VMEM_READ removed, works)

With the VMEM_READ groups removed, the compiler generated the following
hot loop vmcnt sequence:

```
vmcnt(15) → vmcnt(14) → vmcnt(13) → ... → vmcnt(1) → vmcnt(0)
(drain complete)
buffer_load ×16 (burst)
```

This is the same **drain-then-burst** pattern seen when the scheduler is
fully disabled, confirming that the VMEM_READ groups are what force the
constant-16-pending interleaving.

### Assembly verification of Test 8 (Stage 1 only, crashes)

With VMEM_READ groups still present, the hot loop shows:

```
vmcnt(15) → ds_write → buffer_load →
vmcnt(15) → ds_write → buffer_load →
... (×16, constant 16 pending)
```

Identical to the original crashing pattern.

---

## 6. Root Cause

### The problematic intrinsic

```cpp
__builtin_amdgcn_sched_group_barrier(0x020, 1, 0); // VMEM read
```

This intrinsic appears at **line 376** (A buffer_load loop) and **line 387**
(B buffer_load loop) of `gemm_pipeline_ag_bg_cr_comp_v3.hpp`. It is
emitted 16 times per hot loop iteration (8 for A + 8 for B).

### Mechanism

The VMEM_READ scheduling groups force the LLVM AMDGPU IGroupLP scheduler
pass to place each `buffer_load_dwordx4` immediately after its
corresponding `ds_write_b128` + MFMA group, creating a strict 1:1
interleaving. This prevents the compiler from batching buffer_loads
and instead produces a pattern where:

- Each `ds_write` drains one old load (`vmcnt(15)`)
- Each `buffer_load` immediately replaces it
- The pending VMEM operation count remains at **exactly 16 at all times**
- There is **no drain period** where the VMEM pipeline can fully clear

Without these groups, the compiler naturally produces a drain-then-burst
pattern where all old loads complete (vmcnt goes to 0) before new loads
are issued, creating a periodic **zero-pending window**.

### Why this causes ATHUB_INTERRUPT on MI350X

The generated assembly is **semantically correct** — all `vmcnt` values,
data dependencies, barrier placements, and register usage are verified
correct. This is not a compiler code-generation bug.

The issue is that the specific hardware execution pattern triggers a
problem in the MI350X memory subsystem:

1. **Sustained VMEM pressure without relief.** With 16 pending VMEM ops
   constantly maintained across 254 hot loop iterations, the Address
   Translation Hub (ATHUB) and TLB are under continuous maximum load.
   For K=16384, the buffer_load addresses span a wide address range
   (stride between consecutive K-steps), causing frequent TLB misses.

2. **No drain window for TLB recovery.** The drain-then-burst pattern in
   working versions provides a window (at vmcnt=0) where the TLB and
   VMEM return path can fully process all outstanding translations and
   completions. The constant-16 pattern never provides this window.

3. **Intermittent nature explained.** The TLB miss rate depends on
   runtime address mapping. Under sustained maximum pressure, internal
   queues gradually accumulate backlog. After enough iterations (warmup +
   some repeat runs), the backlog exceeds capacity and triggers the
   uncorrectable ATHUB_INTERRUPT.

4. **Shape dependence explained.** Larger K means more hot loop iterations
   (256 for K=16384 vs 64 for K=4096), giving more time for the
   resource exhaustion to build up.

---

## 7. Suspected Code

### File

```
include/ck_tile/ops/gemm/pipeline/gemm_pipeline_ag_bg_cr_comp_v3.hpp
```

### Exact lines (376 and 387)

```cpp
// Lines 369-390: Stage 1 scheduling groups
static_for<0, num_buffer_load_inst_a, 1>{}([&](auto i) {
    ignore = i;
    static_for<0, num_dswrite_per_issue_a, 1>{}([&](auto idswrite) {
        ignore = idswrite;
        __builtin_amdgcn_sched_group_barrier(0x200, 1, 0); // DS write — SAFE
        __builtin_amdgcn_sched_group_barrier(0x008, 1, 0); // MFMA   — SAFE
    });
    __builtin_amdgcn_sched_group_barrier(0x020, 1, 0); // VMEM read ← PROBLEM (line 376)
    __builtin_amdgcn_sched_group_barrier(
        0x008, num_mfma_per_issue - num_dswrite_per_issue_a, 0); // MFMA — SAFE
});
static_for<0, num_buffer_load_inst_b, 1>{}([&](auto i) {
    ignore = i;
    static_for<0, num_dswrite_per_issue_b, 1>{}([&](auto idswrite) {
        ignore = idswrite;
        __builtin_amdgcn_sched_group_barrier(0x200, 1, 0); // DS write — SAFE
        __builtin_amdgcn_sched_group_barrier(0x008, 1, 0); // MFMA   — SAFE
    });
    __builtin_amdgcn_sched_group_barrier(0x020, 1, 0); // VMEM read ← PROBLEM (line 387)
    __builtin_amdgcn_sched_group_barrier(
        0x008, num_mfma_per_issue - num_dswrite_per_issue_b, 0); // MFMA — SAFE
});
```

### Call site (hot loop body, line 623)

```cpp
// Lines 580-627: Main hot loop
index_t i = 0;
while(i < (num_loop - 2))
{
    block_sync_lds();
    // ... LocalPrefill (ds_writes) ...
    // ... load_tile (buffer_loads) ...
    // ... block_gemm (MFMAs) ...
    block_sync_lds();
    block_gemm.LocalPrefetch(...);
    HotLoopScheduler();                    // ← Contains the problematic VMEM_READ groups
    __builtin_amdgcn_sched_barrier(0);     // ← No-op without active groups
    i += 1;
}
```

### Why the VMEM_READ groups are problematic

- The `0x020` mask targets VMEM read instructions (`buffer_load_dwordx4`).
- With count=1, each group schedules exactly **one** buffer_load.
- Placed inside the `static_for` loop over `num_buffer_load_inst_{a,b}`,
  this creates 16 individual VMEM_READ groups (8 for A + 8 for B).
- The IGroupLP scheduler pass faithfully places one `buffer_load` per
  group, interleaving them 1:1 with ds_writes.
- This prevents the compiler from batching buffer_loads after the ds_write
  drain phase, forcing the constant-16-pending VMEM pattern.

### The DS_WRITE and MFMA groups are safe

Tests 6 and 7 confirm that the DS_WRITE (`0x200`) and MFMA (`0x008`)
groups do not cause any issues. They provide useful scheduling structure
(ordering ds_writes relative to MFMAs) without constraining buffer_load
placement.

---

## 8. Suggested Confirmation Test

To further confirm that the issue is specifically the **1:1 interleaving
pattern** (not just the presence of VMEM_READ groups in general), the
following test can be performed:

### Test: Batched VMEM_READ groups

Replace the per-iteration VMEM_READ groups with a single batched group
placed **after** all ds_write groups:

```cpp
// Stage 1 — modified
static_for<0, num_buffer_load_inst_a, 1>{}([&](auto i) {
    ignore = i;
    static_for<0, num_dswrite_per_issue_a, 1>{}([&](auto idswrite) {
        ignore = idswrite;
        __builtin_amdgcn_sched_group_barrier(0x200, 1, 0); // DS write
        __builtin_amdgcn_sched_group_barrier(0x008, 1, 0); // MFMA
    });
    // REMOVED: __builtin_amdgcn_sched_group_barrier(0x020, 1, 0);
    __builtin_amdgcn_sched_group_barrier(
        0x008, num_mfma_per_issue - num_dswrite_per_issue_a, 0);
});
static_for<0, num_buffer_load_inst_b, 1>{}([&](auto i) {
    ignore = i;
    static_for<0, num_dswrite_per_issue_b, 1>{}([&](auto idswrite) {
        ignore = idswrite;
        __builtin_amdgcn_sched_group_barrier(0x200, 1, 0); // DS write
        __builtin_amdgcn_sched_group_barrier(0x008, 1, 0); // MFMA
    });
    // REMOVED: __builtin_amdgcn_sched_group_barrier(0x020, 1, 0);
    __builtin_amdgcn_sched_group_barrier(
        0x008, num_mfma_per_issue - num_dswrite_per_issue_b, 0);
});

// Batch all VMEM reads after the ds_write/MFMA groups
__builtin_amdgcn_sched_group_barrier(
    0x020, num_buffer_load_inst_a + num_buffer_load_inst_b, 0);
```

**Expected outcome:** If this works, it confirms the issue is the 1:1
interleaving. The batched group keeps buffer_loads within Stage 1 but
allows the compiler to place them after the ds_write drain, preserving
the drain-then-burst pattern.

### Alternative: Remove VMEM_READ groups entirely

Simply comment out lines 376 and 387. The DS_WRITE and MFMA groups alone
provide sufficient scheduling control. The compiler will batch buffer_loads
naturally.

---

*Investigation performed on: March 2026*
*Architecture: MI350X (gfx950)*
*Source: `gemm_pipeline_ag_bg_cr_comp_v3.hpp` in Composable Kernel*
