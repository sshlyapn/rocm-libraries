# RDNA4 (gfx1201) dispatch-gap reproduction

AQL vs PM4 per-dispatch command-processor gap on RDNA4 (gfx1201), short
(dispatch-bound) to long (~3 ms) kernels.

## Environment

| item | value |
|---|---|
| Docker container | `hipvk-isolated-sshliapn` |
| GPU / arch | device 0, AMD Radeon AI PRO R9700, gfx1201 |
| Patched CLR | `~/code/rocm-libraries/projects/clr/build-gap/hipamd/lib` (libamdhip64.so.7.14.60850-6e806553ab5) |
| Sources / binaries | `~/code/rocm-libraries/vk_gap_test/` (`hipcc -O2 --offload-arch=gfx1201 <src>.cpp -o <src>.x`) |
| Benchmarks | `hip_gap_graph.x <spin> <n> <K>` (uniform chain); `hip_gap_mix.x <short> <long> <n> <K>` (short/long alternating); `hip_gap_hetero.x <spin> <n> <K>` (4 distinct kernels cycled), n=16384 |
| Pin script | `gpu_pin_freq.sh pin 0 profile_peak` / `unpin 0` |
| Locked clock | `profile_peak` = GFX ~2319 MHz (DPM table top, boost off) |
| Free clock | default DVFS, boosts ~2900-3200 MHz, drops to ~500 MHz in gaps (jittery) |
| rocprofv3 | 1.1.0 |

Method: `busy` = median kernel time from `rocprofv3 --kernel-trace` (forces AQL).
`e2e` = profiler-free whole-graph hipEvent wall clock. `gap = (e2e - busy*K)/(K-1)`;
`PM4 saves` = AQL gap - PM4 gap (us) and e2e reduction (%). Free-clock gaps are
omitted (busy profiled and e2e profiler-free land at different DVFS states).

## Uniform chain -- LOCKED (profile_peak, ~2319 MHz)

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

## Uniform chain -- FREE (default DVFS, e2e median of 5)

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

\* free busy is profiled at a different DVFS state than e2e (reference only).

## Short/long interleave -- LOCKED

K=400 = 200 short + 200 long, alternating.

| short | long | busy_s (us) | busy_l (us) | AQL e2e (ms) | PM4 e2e (ms) | AQL gap (us) | PM4 saves |
|---|---|---|---|---|---|---|---|
| 250  | 5000   | 4.40  | 98.40   | 21.84  | 20.39  | 3.21 | 3.64 us (6.6%) |
| 500  | 20000  | 7.84  | 335.20  | 69.79  | 68.63  | 2.97 | 2.92 us (1.7%) |
| 250  | 50000  | 4.40  | 809.23  | 165.61 | 162.56 | 7.23 | 7.65 us (1.8%) |
| 1000 | 100000 | 14.72 | 1597.57 | 324.76 | 321.99 | 5.76 | 6.93 us (0.9%) |

## Short/long interleave -- FREE (e2e median of 5)

| short | long | busy_s (us)* | busy_l (us)* | AQL e2e (ms) | PM4 e2e (ms) | PM4 saves (% of e2e) |
|---|---|---|---|---|---|---|
| 250  | 5000   | 3.72  | 134.52  | 26.90  | 25.68  | 4.5% |
| 500  | 20000  | 6.48  | 301.14  | 59.08  | 58.48  | 1.0% |
| 250  | 50000  | 3.76  | 647.28  | 127.33 | 124.44 | 2.3% |
| 1000 | 100000 | 11.60 | 1243.46 | 235.73 | 231.43 | 1.8% |

\* free busy profiled at a different DVFS state than e2e (reference only).

## Heterogeneous interleave -- 4 distinct kernels

`hip_gap_hetero.x` cycles 4 kernels (node k uses k%4; K=800 = 200 each = 800
total dispatches), each with a different block size and kernarg signature
(varying pointer count incl. unused, plus unused scalars); gid<n guard keeps work
similar.

| kernel | block | args | busy @ spin=500 | busy @ spin=2000 |
|---|---|---|---|---|
| k1 | 256 | 1 ptr, 2 scalars | 7.80 us | 47.20 us |
| k2 | 128 | 3 ptr (2 unused), 2 scalars | 7.84 us | 47.20 us |
| k3 | 64  | 5 ptr (4 unused), 4 scalars (2 unused) | 8.88 us | 50.64 us |
| k4 | 512 | 2 ptr (1 unused), 4 scalars (2 unused) | 7.80 us | 47.24 us |

LOCKED:

| spin | avg busy (us) | AQL e2e (ms) | PM4 e2e (ms) | AQL gap (us) | PM4 gap (us) | PM4 saves |
|---|---|---|---|---|---|---|
| 250  | 4.49  | 4.88  | 3.63  | 1.62 | 0.05  | 1.57 us (25.6%) |
| 500  | 8.07  | 7.76  | 6.49  | 1.64 | 0.04  | 1.59 us (16.4%) |
| 1000 | 19.14 | 18.46 | 16.20 | 3.93 | 1.12  | 2.82 us (12.2%) |
| 2000 | 48.16 | 42.38 | 38.08 | 4.83 | ~0.00 | 5.39 us (10.2%) |

FREE (e2e median of 5):

| spin | avg busy (us)* | AQL e2e (ms) | PM4 e2e (ms) | PM4 saves (% of e2e) |
|---|---|---|---|---|
| 250  | 3.75  | 3.83  | 2.75  | 28.0% |
| 500  | 10.72 | 5.88  | 4.83  | 18.0% |
| 1000 | 26.84 | 17.19 | 14.87 | 13.5% |
| 2000 | 77.87 | 75.29 | 70.85 | 5.9% |

\* free busy profiled at a different DVFS state than e2e (reference only).

## Notes

- PM4 removes essentially the whole per-dispatch CP gap (PM4 gap ~0): ~1.5 us in
  the launch-bound regime, 10-26% e2e on short kernels. The saving is per-DISPATCH
  (constant), so it amortizes away with kernel size (down to <1% at ~3 ms) and
  tracks dispatch COUNT, not size. Holds for uniform, short/long, and
  heterogeneous chains -- not limited to repeated identical kernels.
- Absolute gap above busy ~300 us is method noise: busy (profiled) vs e2e
  (profiler-free) differ by a few %, multiplied by K, so AQL/PM4 gaps grow in
  lockstep. Use the e2e-savings % there.
- spin~1000-1500 is contaminated by a microbenchmark artifact: the FMA loop takes
  a one-time ~25 us penalty past ~1150 iterations (iteration-bound; same at 2319
  and 477 MHz; geometry- and value-independent; not the CP). Pinned busy ~doubles
  (16->38 us); free ~5.4x because DVFS also drops the longer kernel to ~half clock.
  Prefer spin <=900 or >=2000 for clean gaps.
- Free PM4-slower at uniform spin=1000 (37 vs 27 ms) is a DVFS clock-state
  artifact: PM4 has fewer gaps so it can't be slower at equal clock -> it settled
  at a lower clock. PM4 replays a VRAM IB (no extra host work). Pinning removes it.
- Use LOCKED clocks for all comparisons; free DVFS swings 500-3200 MHz and is
  workload-dependent.
