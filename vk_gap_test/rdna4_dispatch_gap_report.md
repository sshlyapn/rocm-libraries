# RDNA4 (gfx1201) dispatch-gap reproduction

AQL vs PM4 per-dispatch command-processor gap on RDNA4 (gfx1201), swept from
short (dispatch-bound) to long (~3 ms) kernels.

## Environment

| item | value |
|---|---|
| Docker container | `hipvk-isolated-sshliapn` |
| GPU / arch | device 0, AMD Radeon AI PRO R9700, gfx1201 (verified via amd-smi) |
| Patched CLR runtime | `/home/sshliapn/code/rocm-libraries/projects/clr/build-gap/hipamd/lib` (libamdhip64.so.7.14.60850-6e806553ab5) |
| Sources / binaries | `~/code/rocm-libraries/vk_gap_test/` (build with `hipcc -O2 --offload-arch=gfx1201 <src>.cpp -o <src>.x`) |
| Benchmark | `hip_gap_graph.x <spin> <n> <K>`, n=16384 (64 blocks x 256), linear-chain HIP graph of K dependent nodes, interleaved `gap_kernel<1>`/`<3>`; short/long interleave via `hip_gap_mix.x <short> <long> <n> <K>` |
| Pin script | `/usr/local/sbin/gpu_pin_freq.sh pin 0 profile_peak` / `unpin 0` (passwordless `sudo -n`) |
| Locked clock | `profile_peak` = GFX ~2319 MHz under load (pins DPM table top, boost DISABLED) |
| Free clock | default DVFS, opportunistic boost ~2900-3200 MHz under sustained load (spec boost 2920 MHz); variable, drops to ~500 MHz in gaps |
| Note | `pp_dpm_sclk`/amd-smi `MAX_CLK` report only the 2350 MHz DPM table top; the fine-grained boost above it is not listed there and cannot be pinned (no OD table). So locked = stable ~2319 MHz, auto = faster but jittery. |
| rocprofv3 | 1.1.0 |

Method: `busy` = median kernel duration from `rocprofv3 --kernel-trace` (CSV,
End-Start; the patched CLR forces AQL while a kernel-trace profiler is armed, so
PM4 cannot be traced directly). `e2e` = profiler-free whole-graph hipEvent wall
clock (best-of-7). Per-dispatch `gap = (e2e - busy*K)/(K-1)`; `PM4 saves` = AQL
gap - PM4 gap (us) and the whole-graph e2e reduction (%).

## Results -- clocks LOCKED (profile_peak, GFX ~2319 MHz)

| spin | K | busy (us) | AQL e2e (ms) | PM4 e2e (ms) | AQL gap (us) | PM4 gap (us) | PM4 saves |
|---|---|---|---|---|---|---|---|
| 250    | 1500 | 4.72    | 9.39  | 7.01  | 1.54   | ~0.00  | 1.58 us (25.3%) |
| 500    | 1500 | 8.52    | 14.87 | 12.54 | 1.39   | ~0.00  | 1.56 us (15.7%) |
| 750    | 1000 | 12.44   | 13.63 | 12.04 | 1.20   | ~0.00  | 1.59 us (11.7%) |
| 1000   | 800  | 16.08   | 23.13 | 20.27 | 12.85  | 9.27   | 3.58 us (12.4%) |
| 1500   | 800  | 37.56   | 36.25 | 32.54 | 7.77   | 3.12   | 4.65 us (10.2%) |
| 2000   | 600  | 47.56   | 32.12 | 28.77 | 5.98   | 0.39   | 5.59 us (10.4%) |
| 3000   | 400  | 61.68   | 27.08 | 24.97 | 6.02   | 0.75   | 5.27 us (7.8%) |
| 5000   | 300  | 89.56   | 29.11 | 27.49 | 7.51   | 2.08   | 5.43 us (5.6%) |
| 10000  | 150  | 159.88  | 25.62 | 24.83 | 10.98  | 5.69   | 5.29 us (3.1%) |
| 25000  | 80   | 373.76  | 31.37 | 30.90 | 18.58  | 12.65  | 5.92 us (1.5%) |
| 50000  | 50   | 731.97  | 38.43 | 37.67 | 37.28  | 21.89  | 15.39 us (2.0%) |
| 100000 | 40   | 1442.33 | 60.14 | 59.60 | 62.85  | 48.81  | 14.05 us (0.9%) |
| 200000 | 30   | 2827.75 | 89.55 | 88.86 | 162.53 | 138.79 | 23.74 us (0.8%) |
| 210000 | 30   | 3002.90 | 93.81 | 93.05 | 128.41 | 102.10 | 26.31 us (0.8%) |

## Results -- clocks FREE (default DVFS)

The hybrid gap is invalid here (busy is profiled, e2e is profiler-free, and the
two processes land on different DVFS points), so only the measured columns are
shown. e2e is the median of 5 reps.

| spin | K | busy (us)* | AQL e2e (ms) | PM4 e2e (ms) | PM4 saves (% of e2e) |
|---|---|---|---|---|---|
| 250    | 1500 | 4.04    | 7.34  | 5.31  | 27.6% |
| 500    | 1500 | 7.16    | 11.37 | 9.33  | 18.0% |
| 750    | 1000 | 10.40   | 10.30 | 8.91  | 13.4% |
| 1000   | 800  | 13.56   | 26.67 | 37.16 | -39.3% (PM4 slower) |
| 1500   | 800  | 74.00   | 71.62 | 61.55 | 14.1% |
| 2000   | 600  | 79.72   | 56.96 | 53.68 | 5.8% |
| 3000   | 400  | 103.44  | 42.49 | 40.27 | 5.2% |
| 5000   | 300  | 126.00  | 38.06 | 36.62 | 3.8% |
| 10000  | 150  | 181.80  | 26.60 | 25.69 | 3.4% |
| 25000  | 80   | 324.16  | 24.98 | 25.08 | -0.4% |
| 50000  | 50   | 585.93  | 29.91 | 28.02 | 6.3% |
| 100000 | 40   | 1519.61 | 45.40 | 45.30 | 0.2% |
| 200000 | 30   | 2254.35 | 66.27 | 65.89 | 0.6% |
| 210000 | 30   | 2364.26 | 69.72 | 69.17 | 0.8% |

\* busy measured under the profiler at a different DVFS state than the e2e run.

## Results -- short/long interleave (LOCKED, profile_peak)

`hip_gap_mix.x <short> <long> 16384 K`: a K-node chain whose nodes ALTERNATE
short-spin and long-spin kernels (K=400 = 200 short + 200 long). busy is the
median of each cluster from rocprofv3; e2e is profiler-free. gap and saves as
above (over K-1 dispatches).

| short | long | K | busy_s (us) | busy_l (us) | AQL e2e (ms) | PM4 e2e (ms) | AQL gap (us) | PM4 saves |
|---|---|---|---|---|---|---|---|---|
| 250  | 5000   | 400 | 4.40  | 98.40   | 21.84  | 20.39  | 3.21 | 3.64 us (6.6%) |
| 500  | 20000  | 400 | 7.84  | 335.20  | 69.79  | 68.63  | 2.97 | 2.92 us (1.7%) |
| 250  | 50000  | 400 | 4.40  | 809.23  | 165.61 | 162.56 | 7.23 | 7.65 us (1.8%) |
| 1000 | 100000 | 400 | 14.72 | 1597.57 | 324.76 | 321.99 | 5.76 | 6.93 us (0.9%) |

Interleaving short and long kernels does not change the conclusion: PM4 still
removes essentially the whole per-dispatch gap (PM4 gap ~0), saving ~3-8 us per
dispatch. Because the saving is per-DISPATCH (not per-microsecond-of-work), its
% of wall clock is set by how much the long kernels dominate the total: ~6.6%
when the long kernel is 5000 spin, falling to <1% once the long kernel is
100000 spin. So the benefit tracks dispatch COUNT, not kernel size.

## Results -- short/long interleave (FREE, default DVFS)

Same scenarios with clocks free. The hybrid gap is invalid (busy profiled vs e2e
profiler-free at different DVFS points), so only measured columns + e2e saving %
are shown; e2e is the median of 5 reps.

| short | long | K | busy_s (us)* | busy_l (us)* | AQL e2e (ms) | PM4 e2e (ms) | PM4 saves (% of e2e) |
|---|---|---|---|---|---|---|---|
| 250  | 5000   | 400 | 3.72  | 134.52  | 26.90  | 25.68  | 4.5% |
| 500  | 20000  | 400 | 6.48  | 301.14  | 59.08  | 58.48  | 1.0% |
| 250  | 50000  | 400 | 3.76  | 647.28  | 127.33 | 124.44 | 2.3% |
| 1000 | 100000 | 400 | 11.60 | 1243.46 | 235.73 | 231.43 | 1.8% |

\* busy under the profiler at a different DVFS state than the e2e run.

PM4 still saves on every scenario, but DVFS makes the numbers noisy and not
comparable to the pinned table: the short cluster runs FASTER than pinned (boost,
e.g. 3.72 vs 4.40 us) while the long cluster runs either slower (134.5 vs 98.4 us
at long=5000) or faster (1243 vs 1598 us at long=100000) depending on which clock
state DVFS settles into for that workload mix. This is exactly why the LOCKED
interleave table is the canonical one.

## Results -- heterogeneous interleave (4 distinct kernels, LOCKED)

`hip_gap_hetero.x <spin> 16384 K` cycles four DIFFERENT kernels per node, each
with a different block size AND a different kernarg signature (varying pointer
count incl. unused pointers, plus extra unused scalar args) -- closer to a real
workload where successive dispatches differ in launch dims and kernarg
layout/size. All do the same dependent-FMA spin on the shared buffer with a
gid<n guard, so the active thread count (and thus busy) is similar:

| kernel | block | args | busy @ spin=500 | busy @ spin=2000 |
|---|---|---|---|---|
| k1 | 256 | 1 ptr, 2 scalars              | 7.80 us | 47.20 us |
| k2 | 128 | 3 ptr (2 unused), 2 scalars  | 7.84 us | 47.20 us |
| k3 | 64  | 5 ptr (4 unused), 4 scalars (2 unused int)   | 8.88 us | 50.64 us |
| k4 | 512 | 2 ptr (1 unused), 4 scalars (2 unused float) | 7.80 us | 47.24 us |

K=800 = 200 of each kernel = 800 TOTAL dispatches (the four kernels cycle, node
k uses kernel k%4); it is NOT 800x4. avg busy = mean of the four per-kernel
medians.

Whole-graph e2e -- clocks LOCKED (profile_peak, GFX ~2319 MHz):

| spin | avg busy (us) | AQL e2e (ms) | PM4 e2e (ms) | AQL gap (us) | PM4 gap (us) | PM4 saves |
|---|---|---|---|---|---|---|
| 250  | 4.49  | 4.88  | 3.63  | 1.62 | 0.05  | 1.57 us (25.6%) |
| 500  | 8.07  | 7.76  | 6.49  | 1.64 | 0.04  | 1.59 us (16.4%) |
| 1000 | 19.14 | 18.46 | 16.20 | 3.93 | 1.12  | 2.82 us (12.2%) |
| 2000 | 48.16 | 42.38 | 38.08 | 4.83 | ~0.00 | 5.39 us (10.2%) |

Whole-graph e2e -- clocks FREE (default DVFS, e2e median of 5):

| spin | AQL e2e (ms) | PM4 e2e (ms) | PM4 saves (% of e2e) |
|---|---|---|---|
| 250  | 3.83  | 2.75  | 28.0% |
| 500  | 5.88  | 4.83  | 18.0% |
| 1000 | 17.19 | 14.87 | 13.5% |
| 2000 | 75.29 | 70.85 | 5.9% |

Takeaway: varying launch dims and kernarg layout/size per dispatch does NOT
change the result. PM4 IB replay is faster in EVERY case, locked and free,
removing essentially the whole per-dispatch gap (PM4 gap ~0; 10-26% e2e savings
on these short/mid kernels). Note this heterogeneous mix does NOT reproduce the
PM4-slower-under-free anomaly seen for the minimal homogeneous spin=1000 kernel:
the extra per-dispatch host work (distinct kernels, larger kernargs) keeps the
clock boosted, so PM4 stays ahead. PM4's advantage is not limited to repeated
identical kernels. (Free spin=2000 e2e is ~75 ms vs ~42 ms locked -- the usual
DVFS clock drop for the longer post-step kernel.)

## Why busy jumps so much between spin=1000 and spin=1500 (esp. free)

Two independent effects stack at this exact spot:

1. Kernel-internal step (clock-independent). The dependent FMA loop takes a
   one-time penalty once it exceeds ~1150 iterations (see key findings). spin=1000
   is just below it, spin=1500 just above, so the work-time alone roughly doubles
   (pinned busy 16.1 -> 37.6 us, ~2.3x). This is a property of the microbenchmark
   loop, not the command processor or the dispatch path.

2. A DVFS clock switch (free mode only). Measured under rocprof, free-clock busy
   is stable within each spin but jumps disproportionately across the step:
   spin=1000 = 14.1 us (FASTER than pinned -> boost ~2650 MHz), spin=1500 =
   75.5 us (2x SLOWER than pinned -> clock ~1150 MHz). So once the kernel grows
   past the ~1150-iter step into the longer regime, DVFS drops it from a high
   boost state to roughly half clock.

Net: pinned shows ~2.3x (effect 1 only); free shows ~5.4x because the ~2x DVFS
clock drop (effect 2) multiplies the ~2.3x kernel step. Both are stable/repeatable
(<1% run-to-run). Neither is a CP/dispatch phenomenon -- it is the spin loop
crossing its ~1150-iteration threshold, with DVFS amplifying it when clocks are
free. Use pinned clocks and spin away from ~1150 for clean dispatch-gap numbers.

## Key findings

- PM4 IB replay is never slower than AQL (pinned) and removes the per-dispatch
  CP cost: ~1.5 us in the launch-bound regime (PM4 gap ~0), driving 11-25% e2e
  savings on short kernels.
- The benefit is a per-dispatch CONSTANT, so it amortizes away: pinned e2e
  savings fall from ~25% (busy ~5 us) to ~0.8% at ~3 ms/kernel.
- Absolute gap columns above busy ~300 us are method noise: busy (profiled) and
  e2e (profiler-free) differ by a few percent that gets multiplied by K, so the
  AQL and PM4 gap columns grow in lockstep (not a real CP gap, which is a fixed
  few us). Use the e2e-savings % for large kernels.
- The spin=1000-1500 region is contaminated by a kernel-internal step: the
  dependent FMA loop takes a one-time ~25 us penalty once it exceeds ~1150
  iterations. Verified iteration-bound (same threshold at GFX 2319 MHz and
  477 MHz), geometry-independent (n=256..16384), value-independent, and not a
  compiler/DVFS effect -- it is a property of the microbenchmark kernel, not the
  command processor. Prefer spin <= 900 or >= 2000 for clean CP-gap numbers.
- Free clocks are unusable for this measurement: DVFS distorts busy (spin=1500
  busy 74 us free vs 37.6 us pinned), and PM4 can even appear slower than AQL
  (spin=1000) because its lighter host workload fails to trigger clock boost.
