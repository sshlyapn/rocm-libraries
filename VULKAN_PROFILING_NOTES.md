# llama.cpp Vulkan Profiling Notes (AMD RDNA3/RDNA4)

Detailed guide for measuring true per-kernel GPU times and inter-kernel gaps for
llama.cpp on the Vulkan backend, on AMD GPUs (validated on a Radeon PRO W7900,
gfx1100, Ubuntu 24.04, inside the `test-framework-sshliapn-container-*` docker).

Goal that drove these notes: get **pure per-dispatch GPU-busy time** for a decode
token, sum it, compare against the end-to-end iteration time (TPOT), and find out
whether Vulkan has the same ~3 us inter-kernel gap that HIP shows (wave drain,
cache flush, command-processor turnaround).

---

## TL;DR - what works

- **RADV (Mesa) RGP/SQTT capture of compute dispatches: does NOT work** on this
  stack (Mesa 25.0.7, gfx1100). You get fill/barrier/copy API markers but **zero
  compute-dispatch wave events**. Dead end for per-kernel timing.
- **AMDVLK PAL GpuProfiler (file mode): WORKS** and is the recommended path. It
  emits per-command CSVs with exact GPU-busy time for every `CmdDispatch()` and
  every `CmdReleaseThenAcquire()` barrier (with itemized cache flush/invalidate).
- llama.cpp is **headless compute (no swapchain / no `vkQueuePresentKHR`)**, so the
  profiler never sees a "frame" by default. A **one-line source patch** that emits
  an `AmdFrameEnd` queue debug label per `graph_compute` gives the profiler a frame
  to flush. Gate it behind `GGML_VK_FRAME_MARKER=1`.
- The marker does **not** advance the file profiler's frame counter (it stays at
  frame 0 = whole run, flushed at device destroy). Use a **differential capture**
  (run N tokens minus M tokens) to cancel warmup/cold-start and isolate a clean
  steady-state token.

---

## Driver matrix - who can produce what

| Capability | RADV (Mesa 25.0.7) | AMDVLK 2025.Q2.1 (PAL) |
|---|---|---|
| Runs llama.cpp Vulkan | Yes (~116 tok/s, 8B Q4_K_M) | Yes (~118 tok/s clean, ~90 tok/s under profiler) |
| `MESA_VK_TRACE=rgp` SQTT (`.rgp`) | Produces `.rgp`, but only fill/barrier/copy markers; **no compute dispatch events** on gfx1100 | n/a |
| Per-dispatch GPU-busy CSV | No | **Yes** (PAL GpuProfiler `GpuProfilerMode=1/2/3`) |
| Per-barrier cost + cache-op breakdown | No | **Yes** (`CmdReleaseThenAcquire()` rows) |
| `.rgp` for RGP GUI | Best-effort, broken for compute here | Only via DevDriver/`RadeonDeveloperService` (file mode writes CSV, not `.rgp`) |

Why RADV fails here: AMD's own docs state RADV Vulkan RGP capture is best-effort
and "not supported by the Radeon Developer Panel". On this Mesa build the SQTT
buffer records ~no compute-wave data (a full-token capture is ~65 KB; a real SQTT
would be many MB), so RGP shows only the few API-marker commands.

Switch between drivers without uninstalling:
```bash
# Force AMDVLK only:
export VK_DRIVER_FILES=/opt/amdvlk/amdvlk_icd.json
export AMD_VULKAN_ICD=AMDVLK
# Force RADV: unset the above (RADV is the distro default ICD).
# Sanity check: llama.cpp prints the driver name at startup:
#   "AMD Radeon PRO W7900 (AMD open-source driver)"  -> AMDVLK
#   "AMD Radeon PRO W7900 (RADV ...)"                -> RADV
```

---

## One-time setup

### 1. Install AMDVLK (no system install needed - extract the deb)
```bash
cd /tmp
curl -sL -o amdvlk.deb \
  https://github.com/GPUOpen-Drivers/AMDVLK/releases/download/v-2025.Q2.1/amdvlk_2025.Q2.1_amd64.deb
rm -rf /opt/amdvlk && mkdir -p /opt/amdvlk
dpkg -x amdvlk.deb /opt/amdvlk
# Rewrite the ICD json with an absolute .so path:
python3 - <<'PY'
import json
d = json.load(open("/opt/amdvlk/etc/vulkan/icd.d/amd_icd64.json"))
d["ICD"]["library_path"] = "/opt/amdvlk/usr/lib/x86_64-linux-gnu/amdvlk64.so"
json.dump(d, open("/opt/amdvlk/amdvlk_icd.json", "w"), indent=2)
PY
```
2025.Q2.1 is the final AMDVLK release (the project is discontinued). It supports
RX 9070 / RX 7900-7600 / RX 6000 / RX 5000 / W5000+ (gfx10+).

### 2. Build the Vulkan backend (inside the container)
Requires `glslc` (Vulkan SDK / `glslang-tools`) for shader compilation.
```bash
cd /home/sshliapn/code/llama.cpp
# Configure (once). Key flag: GGML_VULKAN=ON. HIP off for a clean Vulkan build.
cmake -S . -B build_vulkan -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON
cmake --build build_vulkan --target llama-bench --parallel
```

---

## The source patch (ggml-vulkan.cpp)

Two env-gated patches were added. Both are no-ops unless their env var is set, so
they are safe to keep in the tree.

### A. `GGML_VK_FRAME_MARKER` - per-token frame boundary (REQUIRED for AMDVLK profiling)
Headless compute never calls `vkQueuePresentKHR`, so the PAL GpuProfiler sees no
frame and writes nothing. PAL recognizes the magic debug-utils label `AmdFrameEnd`
(and `AmdFrameBegin`) as a present-equivalent frame boundary. We emit it once at
the end of every `ggml_backend_vk_graph_compute` (= one decode token).

- Declaration (struct `vk_instance_t`, ~line 2147):
  ```cpp
  PFN_vkQueueInsertDebugUtilsLabelEXT pfn_vkQueueInsertDebugUtilsLabelEXT = {};
  ```
- Loader (debug-utils block, ~line 6506):
  ```cpp
  vk_instance.pfn_vkQueueInsertDebugUtilsLabelEXT =
      (PFN_vkQueueInsertDebugUtilsLabelEXT) vkGetInstanceProcAddr(
          vk_instance.instance, "vkQueueInsertDebugUtilsLabelEXT");
  ```
- Emit (end of `ggml_backend_vk_graph_compute`, ~line 15996):
  ```cpp
  if (vk_instance.debug_utils_support && vk_instance.pfn_vkQueueInsertDebugUtilsLabelEXT &&
      getenv("GGML_VK_FRAME_MARKER") != nullptr) {
      vk::DebugUtilsLabelEXT dul = {};
      dul.pLabelName = "AmdFrameEnd";
      vk_instance.pfn_vkQueueInsertDebugUtilsLabelEXT(
          ctx->device->compute_queue.queue,
          reinterpret_cast<VkDebugUtilsLabelEXT*>(&dul));
  }
  ```

Important: this label requires the `VK_EXT_debug_utils` instance extension, which
llama.cpp only enables when `GGML_VK_DEBUG_MARKERS=1`. So **always set both**
`GGML_VK_DEBUG_MARKERS=1` and `GGML_VK_FRAME_MARKER=1` when profiling with AMDVLK.

Caveat: the label is enough for the profiler to flush a capture, but it does NOT
advance the file profiler's frame counter - everything lands in `frame000000`.
That is why we use the differential method below instead of `StartFrame`/
`FrameCount` windowing (windowing to frame > 0 captures nothing here).

### B. `GGML_VK_ONE_SUBMIT` - collapse a token into one vkQueueSubmit (OPTIONAL)
Used only for RADV SQTT experiments (to capture a whole token in one trace).
Not needed for the AMDVLK CSV path. By default ggml splits a token into multiple
submits (every ~100 nodes or ~100 MB). When set, the whole graph goes in one
submit. Located around line 15892-15902 (gates the early-submit triggers).

---

## AMDVLK PAL GpuProfiler - configuration

PAL reads settings from `amdPalSettings.cfg` in `$AMD_CONFIG_DIR` (or `/etc/amd`),
one `Name,Value` per line; `;` starts a comment. Output goes under `$AMD_DEBUG_DIR`.

```ini
; /tmp/amdcfg/amdPalSettings.cfg
GpuProfilerMode,1                       ; 1=timing(sqtt off, light), 2=sqtt thread-trace, 3=sqtt for RGP
GpuProfilerConfig.LogDirectory,gpuprof  ; relative to $AMD_DEBUG_DIR
GpuProfilerConfig.Granularity,0         ; 0=per-draw/dispatch (what we want), 1=per-cmdbuf
GpuProfilerConfig.StartFrame,0          ; keep 0 (marker does not advance the counter)
GpuProfilerConfig.FrameCount,1          ; capture the single (whole-run) frame 0
```

Mode guidance:
- **Mode 1 (timing, SQTT off): use this.** Light overhead, produces the per-command
  CSVs with exact GPU-busy times. This is all you need for sum-vs-iter and the gap.
- Mode 3 (SQTT for RGP) in file mode also writes the CSVs but does NOT write a
  `.rgp` here, and adds large overhead (a windowed mode-3 capture took ~5 minutes).
  Only worth it if you also stand up `RadeonDeveloperService` for a true `.rgp`.

Output files per captured frame, one CSV per hardware engine:
- `frame000000Dev0EngAce0-00.csv`  - **ACE = Asynchronous Compute Engine** (this is
  where llama.cpp dispatches run; this is the file you analyze).
- `frame000000Dev0EngDma0-00.csv`  - SDMA (transfers).
- `GPUProfiler.txt`                 - settings echo / log.

CSV columns of interest: `CmdBuffer Call`, `Start Clock`, `End Clock`,
`Time (us) [Frequency: 100000000]`, `PipelineHash`, `CompilerHash`,
`Verts/ThreadGroups`. Clock ticks are at 100 MHz here (1 tick = 0.01 us); recompute
durations from `End - Start` so barriers (which have no precomputed `Time`) are
included. Notable `CmdBuffer Call` values:
- `CmdDispatch()` - a compute kernel (the thing you sum).
- `CmdReleaseThenAcquire()` - a pipeline barrier; sub-rows list the cache ops
  (Invalidate TCP / GL1 / TCC / SQK$, Flush TCC L2, CS Partial Flush, EOP/EOS TS).
- `Begin()` / `End()` - command-buffer boundaries.

---

## Full capture recipe (steady-state, differential)

GPU safety first (per workspace rule): confirm the target GPU is idle.
```bash
docker exec <container> bash -c "amd-smi process"   # ensure no other compute procs
```

Capture two runs (whole run = frame 0) at two token counts and diff them. The
difference cancels model load, warmup, clock ramp and cold first-token effects.

```bash
docker exec <container> bash -lc '
run() {
  local N=$1 OUT=$2
  cat > /tmp/amdcfg/amdPalSettings.cfg <<EOF
GpuProfilerMode,1
GpuProfilerConfig.LogDirectory,gpuprof
GpuProfilerConfig.Granularity,0
GpuProfilerConfig.StartFrame,0
GpuProfilerConfig.FrameCount,1
EOF
  rm -rf /tmp/amdprof/gpuprof
  export VK_DRIVER_FILES=/opt/amdvlk/amdvlk_icd.json AMD_VULKAN_ICD=AMDVLK
  export AMD_CONFIG_DIR=/tmp/amdcfg AMD_DEBUG_DIR=/tmp/amdprof
  export GGML_VK_DEBUG_MARKERS=1 GGML_VK_FRAME_MARKER=1
  local M=/path/to/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf
  local B=/home/sshliapn/code/llama.cpp/build_vulkan/bin/llama-bench
  HIP_VISIBLE_DEVICES=0 $B -m $M -p 0 -n $N -fa 1 -ngl 99 -r 1 2>&1 | grep -E "tg[0-9]"
  cp "$(find /tmp/amdprof -name "*EngAce0*.csv")" "$OUT"
}
run 80 /tmp/ace_n80.csv
run 16 /tmp/ace_n16.csv
'
# Analyze each and diff (parser below):
python3 parse_pal_profiler.py /tmp/ace_n80.csv
python3 parse_pal_profiler.py /tmp/ace_n16.csv
```

Steady-state per token = `(metric_N80 - metric_N16) / (80 - 16)`.

The parser script is committed alongside this file: `parse_pal_profiler.py`
(separates dispatch time vs barrier time vs span; prints top pipelines by GPU time).

---

## Reference result (W7900, Llama-3.1-8B Q4_K_M, fa=1, AMDVLK, profiler on)

Differential over 64 steady-state tokens:

| Component | us/token | % of token | notes |
|---|---|---|---|
| GPU queue span (wall) | 11140.8 | 100% | = 89.8 tok/s (profiled) |
| Pure dispatch (kernel busy) | 9329.3 | 83.7% | 485 dispatches/tok, 19.2 us avg |
| Barriers (ReleaseThenAcquire) | 405.0 | 3.6% | 326/tok, 1.24 us each |
| Other (fill/copy) | 7.8 | 0.1% | |
| Unaccounted (CP/launch/idle) | 1398.7 | 12.6% | |

Inter-kernel gap answer: **Vulkan has it too** - ~3.72 us of non-kernel time per
dispatch (0.84 us barrier + 2.88 us CP turnaround/launch/idle). The actual cache
flush+invalidate barrier costs only ~1.24 us; the rest is command-processor /
launch overhead, the same flavor as HIP's ~3 us gap.

---

## Caveats / gotchas

1. **Absolute numbers are AMDVLK + profiler-on**, not RADV. AMDVLK runs llama
   slower than RADV (~90 vs ~116 tok/s here) and the profiler wraps every command
   with timestamps. The **per-dispatch GPU-busy times are exact** (hardware
   timestamps), but the **12.6% "unaccounted" CP/idle is an upper bound** -
   partly inflated by the profiler's per-command serialization. Use proportions
   and per-kernel times as the trustworthy outputs; treat the gap as an upper bound.
2. **Always set `GGML_VK_DEBUG_MARKERS=1` with `GGML_VK_FRAME_MARKER=1`** - the
   frame label needs `VK_EXT_debug_utils`, which is only enabled by the markers env.
3. **Do not use `StartFrame`>0 / `FrameCount` windowing** on this path - the marker
   does not advance the frame counter, so any window past frame 0 captures nothing.
   Everything is `frame000000`; use the differential method for steady state.
4. **Analyze `EngAce0`** (async compute), not `EngGfx`/`EngDma`. That is where the
   model dispatches land.
5. **`frame000000` = the entire run** (warmup + all tokens + any lm_head), flushed
   at device destroy. A single-run capture is contaminated by cold start; always
   diff two token counts.
6. RADV path (`MESA_VK_TRACE=rgp`, `MESA_VK_TRACE_TRIGGER`/`_FRAME`/`_PER_SUBMIT`,
   `RADV_THREAD_TRACE_*`) is documented in Mesa env vars but **does not capture
   compute dispatch events** on this gfx1100/Mesa build - do not waste time on it
   for per-kernel timing.
7. Mode-3 (RGP SQTT) in file mode is slow and still writes only CSVs (no `.rgp`).
   For a real `.rgp` you must run `RadeonDeveloperService` (DevDriver) and trigger
   a capture - heavier, and not needed for sum-vs-iter analysis.
8. To map `PipelineHash` -> ggml shader name, add to `amdPalSettings.cfg`:
   `EnablePipelineDump,1` and `PipelineDumpDir,<dir>` (dumps `.pipe` + `.spv` per
   pipeline). Not required for the timing analysis.

---

## Environment variable quick reference

| Var | Where | Purpose |
|---|---|---|
| `VK_DRIVER_FILES=/opt/amdvlk/amdvlk_icd.json` | shell | force AMDVLK ICD |
| `AMD_VULKAN_ICD=AMDVLK` | shell | select AMDVLK when both ICDs present |
| `AMD_CONFIG_DIR=/tmp/amdcfg` | shell | dir holding `amdPalSettings.cfg` |
| `AMD_DEBUG_DIR=/tmp/amdprof` | shell | profiler output root |
| `GGML_VK_DEBUG_MARKERS=1` | shell | enables `VK_EXT_debug_utils` (needed for the frame label) |
| `GGML_VK_FRAME_MARKER=1` | shell | emit `AmdFrameEnd` per token (patch A) |
| `GGML_VK_ONE_SUBMIT=1` | shell | one vkQueueSubmit per token (patch B, RADV SQTT only) |
| `GGML_VK_PERF_LOGGER=1` | shell | llama.cpp's built-in timestamp logger (proxy; folds gaps) |
| `MESA_VK_TRACE=rgp` + `MESA_VK_TRACE_TRIGGER=/tmp/trig` | shell | RADV SQTT capture (broken for compute here) |
