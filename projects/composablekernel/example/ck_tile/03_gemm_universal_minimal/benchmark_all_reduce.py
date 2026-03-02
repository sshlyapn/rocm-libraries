#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
# SPDX-License-Identifier: MIT
#
# Benchmark script for fused vs reference GEMM + AllReduce kernels.
#
# Tested executables:
#   1. tile_example_gemm_universal_persistent_all_reduce          (fused: GEMM + in-kernel reduce + rocSHMEM)
#   2. tile_example_gemm_universal_persistent_all_reduce_ref_default  (ref: GEMM + separate reduce + NCCL)
#   3. tile_example_gemm_universal_persistent_all_reduce_ref_rdc      (ref: same, built with -fgpu-rdc)
#
# Outputs:
#   - benchmark_results.csv   — all measurements in one flat CSV
#   - benchmark_all_logs.txt  — concatenated stdout/stderr from every run
#
# Usage:
#   cd <build_dir>
#   python3 ../example/ck_tile/03_gemm_universal_minimal/benchmark_all_reduce.py
#
# Resume after Ctrl+C:
#   python3 ../example/ck_tile/03_gemm_universal_minimal/benchmark_all_reduce.py \
#       --resume benchmark_results_20260302_143025.csv
#
# The script expects to be run from the build directory (or set BUILD_DIR below).

import argparse
import subprocess
import csv
import os
import re
import glob
import signal
import sys
from datetime import datetime
from itertools import product

# =====================================================================
#                         CONFIGURATION
# =====================================================================

# Build directory (where bin/ lives). "." assumes CWD is the build dir.
BUILD_DIR = "."

# Executable names (relative to BUILD_DIR/bin/)
FUSED_EXE       = "tile_example_gemm_universal_persistent_all_reduce"
REF_DEFAULT_EXE = "tile_example_gemm_universal_persistent_all_reduce_ref_default"
REF_RDC_EXE     = "tile_example_gemm_universal_persistent_all_reduce_ref_rdc"

# MPI launcher for multi-GPU runs
MPI_LAUNCHER = "mpiexec"
MPI_FLAGS    = ["--allow-run-as-root"]
NUM_RANKS    = 8            # number of GPUs / PEs

# Kernel configuration.
# NOTE: --timing 1 (fused) only supports config 0 and 1.
#   0: 128x128x64,  2x2 warp, 16x16 XDL  (MPerBlock=128, NPerBlock=128)
#   1: 256x256x64,  2x2 warp, 32x32 XDL  (MPerBlock=256, NPerBlock=256)
CONFIG_ID = 1

# ---- Problem sizes ----
# M: key values with ±256 neighbours; dense step for small, 4k step for large.
# All values are multiples of 256 (required for config 1, MPerBlock=256).
M_VALUES_FULL = [
    # Small values (step 256)
    256, 512, 768, 1024, 1280, 1536, 1792, 2048,
    # Around 4096 (±256)
    3840, 4096, 4352,
    # Around 8192 (±256)
    7936, 8192, 8448,
    # Around 12288 (±256)
    12032, 12288, 12544,
    # Around 16384 (±256)
    16128, 16384, 16640
]

M_VALUES_SHORT = [
    8448
]

M_VALUES = M_VALUES_FULL

# N: several options (must be multiples of NPerBlock=256 for config 1)
N_VALUES = [4096, 8192, 16384]

# K: several options (must be multiples of KPerBlock=64 for config 1)
K_VALUES = [2048, 4096, 8192]

# ---- Fused-kernel specific ----
# Number of N-chunks for chunked all-reduce signalling.
# tiles_n = N / NPerBlock must be divisible by each value.
#   N=4096  → tiles_n=16  → divisible by 1,2,4,8,16
#   N=8192  → tiles_n=32  → divisible by 1,2,4,8,16
#   N=16384 → tiles_n=64  → divisible by 1,2,4,8,16
NUM_CHUNKS_VALUES_FULL = [2, 4, 8, 16]
NUM_CHUNKS_VALUES_SHORT = [4, 8]
NUM_CHUNKS_VALUES_SINGLE = [8]

NUM_CHUNKS_VALUES = NUM_CHUNKS_VALUES_SINGLE

# Rows per reduction group. Must be <= warp_size (64).
# Creates num_post_work = ceil(M / tokens_per_reduction) groups.
TOKENS_PER_REDUCTION_VALUES_FULL = [1, 2, 3, 8]
TOKENS_PER_REDUCTION_VALUES_SHORT = [1, 2]

TOKENS_PER_REDUCTION_VALUES = TOKENS_PER_REDUCTION_VALUES_SHORT

# Number of WGs dedicated to GEMM compute (fused kernel).
# MaxOccupancyGridSize = 256 on MI350X with config 1 (kBlockPerCu=1, 256 CUs).
# Remaining WGs (256 - num_compute_wgs) become post-processing (reduce + put).
# Sweep several values to find the best compute / PP WG balance.
NUM_COMPUTE_WGS_FUSED_VALUES_FULL = [128, 160, 192, 224, 248]
NUM_COMPUTE_WGS_FUSED_VALUES_SHORT = [192, 224]

NUM_COMPUTE_WGS_FUSED_VALUES = NUM_COMPUTE_WGS_FUSED_VALUES_SHORT

# ---- Ref-kernel specific ----
# All WGs do GEMM (no PP WGs). 256 = full occupancy on MI350X.
NUM_COMPUTE_WGS_REF = 256

# ---- Benchmark settings ----
WARMUP  = 50
REPEAT  = 100
TIMEOUT = 300   # seconds per run

# ---- Output files ----
_timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
RESULT_CSV = f"benchmark_results_{_timestamp}.csv"
TIMING_CSV = f"benchmark_timing_{_timestamp}.csv"    # fused --timing 1 (separate)
LOG_FILE   = f"benchmark_all_logs_{_timestamp}.txt"

# =====================================================================
#                         IMPLEMENTATION
# =====================================================================

def exe_path(name):
    """Resolve executable path."""
    return os.path.join(BUILD_DIR, "bin", name)


def estimate_rocshmem_heap_bytes(M, N, tpr, num_ranks=NUM_RANKS, elem_bytes=2):
    """Estimate the rocSHMEM symmetric heap size needed for the AllGather buffer.

    The fused kernel allocates:
        ag_total_bytes = num_ranks × ceil(M / tpr) × N × elem_bytes
    on the symmetric heap via rocshmem_malloc.

    Returns the required heap size in bytes, rounded UP to the nearest 1 GiB.
    Always returns at least 1 GiB.
    """
    if tpr <= 0:
        return 1 << 30  # 1 GiB default
    num_post_work = (M + tpr - 1) // tpr
    ag_total_bytes = num_ranks * num_post_work * N * elem_bytes
    one_gib = 1 << 30
    heap_gib = (ag_total_bytes + one_gib - 1) // one_gib
    heap_gib = max(heap_gib, 1)
    return heap_gib * one_gib


def run_cmd(cmd, timeout=TIMEOUT, env=None):
    """Run command, return (stdout, stderr, returncode)."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           env=env)
        return r.stdout, r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        return "", "TIMEOUT after {}s".format(timeout), -1
    except Exception as e:
        return "", str(e), -1


def parse_persistent_ms(stdout):
    """Extract avg_ms and tflops from the [persistent...] output line."""
    m = re.search(
        r'\[persistent[^\]]*\]\s+avg_ms=([0-9.eE+-]+)\s+tflops=([0-9.eE+-]+)',
        stdout)
    if m:
        return float(m.group(1)), float(m.group(2))
    return None, None


def parse_grid_size(stdout):
    """Extract grid_size from persistent kernel launch info."""
    m = re.search(r'grid_size=(\d+)', stdout)
    if m:
        return int(m.group(1))
    return None


def parse_num_post_processing_wgs(stdout):
    """Extract num_post_processing_wgs from fused kernel output."""
    m = re.search(r'num_post_processing_wgs=(\d+)', stdout)
    if m:
        return int(m.group(1))
    return None


def cleanup_timing_csvs():
    """Remove any leftover timing CSV files from previous fused runs."""
    for pattern in ["rank*_*_summary.csv", "rank*_*_perf.csv"]:
        for f in glob.glob(pattern):
            try:
                os.remove(f)
            except OSError:
                pass


def parse_summary_csvs(M, K, N):
    """
    Parse rank0 _summary.csv produced by fused --timing 1.
    Returns dict with aggregated PP timing metrics (summed across chunks).
    Cleans up all produced CSV files afterwards.
    """
    # Pattern: rank0_m{M}_k{K}_n{N}_cwg*_ppwg*_chunk*_reduction*_summary.csv
    pattern = "rank0_m{}_k{}_n{}_*_summary.csv".format(M, K, N)
    files = sorted(glob.glob(pattern))

    data = {}
    if files:
        try:
            sum_wait_avg = 0.0
            sum_reduce_avg = 0.0
            max_wait_max = 0.0
            max_reduce_max = 0.0
            sum_total_avg = 0.0
            max_total_max = 0.0
            n_chunks_found = 0

            with open(files[0]) as f:
                reader = csv.DictReader(f)
                for row in reader:
                    sum_wait_avg   += float(row['wait_avg_us'])
                    sum_reduce_avg += float(row['reduce_avg_us'])
                    max_wait_max    = max(max_wait_max, float(row['wait_max_us']))
                    max_reduce_max  = max(max_reduce_max, float(row['reduce_max_us']))
                    sum_total_avg  += float(row['total_avg_us'])
                    max_total_max   = max(max_total_max, float(row['total_max_us']))
                    n_chunks_found += 1

            data = {
                'pp_wait_sum_avg_us':   round(sum_wait_avg, 2),
                'pp_reduce_sum_avg_us': round(sum_reduce_avg, 2),
                'pp_wait_max_us':       round(max_wait_max, 2),
                'pp_reduce_max_us':     round(max_reduce_max, 2),
                'pp_total_sum_avg_us':  round(sum_total_avg, 2),
                'pp_total_max_us':      round(max_total_max, 2),
            }
        except Exception as e:
            print(f"  WARNING: failed to parse summary CSV: {e}")

    # Clean up all summary + perf CSVs (all ranks)
    for pat in [
        "rank*_m{}_k{}_n{}_*_summary.csv".format(M, K, N),
        "rank*_m{}_k{}_n{}_*_perf.csv".format(M, K, N),
    ]:
        for f in glob.glob(pat):
            try:
                os.remove(f)
            except OSError:
                pass

    return data


def case_key(variant, M, N, K, num_chunks, tpr, cwg):
    """Build a hashable key that uniquely identifies a benchmark case."""
    return (str(variant), str(M), str(N), str(K), str(num_chunks), str(tpr), str(cwg))


def load_completed_keys(csv_path):
    """Read an existing CSV and return a set of case_key tuples already present."""
    keys = set()
    if not csv_path or not os.path.isfile(csv_path):
        return keys
    try:
        with open(csv_path, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                keys.add(case_key(
                    row.get('variant', ''),
                    row.get('M', ''),
                    row.get('N', ''),
                    row.get('K', ''),
                    row.get('num_chunks', ''),
                    row.get('tokens_per_reduction', ''),
                    row.get('num_compute_wgs', ''),
                ))
    except Exception as e:
        print(f"WARNING: could not read {csv_path}: {e}")
    return keys


def derive_sibling_path(results_csv, old_prefix, new_prefix):
    """Derive timing CSV or log path from the results CSV path.
    E.g. benchmark_results_20260302.csv -> benchmark_timing_20260302.csv"""
    d = os.path.dirname(results_csv)
    base = os.path.basename(results_csv)
    sibling = base.replace(old_prefix, new_prefix, 1)
    return os.path.join(d, sibling) if d else sibling


def main():
    # ---- CLI ----
    parser = argparse.ArgumentParser(
        description="Benchmark fused vs ref GEMM + AllReduce kernels.")
    parser.add_argument("--resume", metavar="RESULTS_CSV", default=None,
                        help="Resume from an interrupted run. Pass the path to the "
                             "existing benchmark_results_*.csv; the timing CSV and "
                             "log file are derived automatically. Already-completed "
                             "cases are skipped.")
    args = parser.parse_args()

    # ---- Resolve output file paths ----
    global RESULT_CSV, TIMING_CSV, LOG_FILE
    resuming = False
    if args.resume:
        if not os.path.isfile(args.resume):
            print(f"ERROR: --resume file not found: {args.resume}")
            sys.exit(1)
        RESULT_CSV = args.resume
        TIMING_CSV = derive_sibling_path(args.resume, "benchmark_results_", "benchmark_timing_")
        LOG_FILE   = derive_sibling_path(
            args.resume.replace('.csv', '.txt'), "benchmark_results_", "benchmark_all_logs_")
        # Fix extension for log: results CSV ends in .csv, log ends in .txt
        if LOG_FILE.endswith('.csv'):
            LOG_FILE = LOG_FILE.rsplit('.csv', 1)[0] + '.txt'
        resuming = True

    # Resolve executable paths
    fused_path       = exe_path(FUSED_EXE)
    ref_default_path = exe_path(REF_DEFAULT_EXE)
    ref_rdc_path     = exe_path(REF_RDC_EXE)

    # Check executables exist
    for name, path in [("fused", fused_path), ("ref_default", ref_default_path), ("ref_rdc", ref_rdc_path)]:
        if not os.path.isfile(path):
            print(f"ERROR: executable not found: {path}")
            print(f"  Make sure BUILD_DIR is set correctly (currently: '{BUILD_DIR}')")
            sys.exit(1)

    # ---- Build test cases ----
    cases = []

    # Fused kernel: M × N × K × num_chunks × tokens_per_reduction × num_compute_wgs
    for M, N, K, nc, tpr, cwg in product(M_VALUES, N_VALUES, K_VALUES,
                                          NUM_CHUNKS_VALUES,
                                          TOKENS_PER_REDUCTION_VALUES,
                                          NUM_COMPUTE_WGS_FUSED_VALUES):
        cases.append({
            'variant': 'fused',
            'exe': fused_path,
            'M': M, 'N': N, 'K': K,
            'num_chunks': nc,
            'tpr': tpr,
            'cwg': cwg,
        })

    # Ref kernels (default + rdc): M × N × K × tokens_per_reduction
    for tag, path in [('ref_default', ref_default_path), ('ref_rdc', ref_rdc_path)]:
        for M, N, K, tpr in product(M_VALUES, N_VALUES, K_VALUES,
                                     TOKENS_PER_REDUCTION_VALUES):
            cases.append({
                'variant': tag,
                'exe': path,
                'M': M, 'N': N, 'K': K,
                'num_chunks': 0,
                'tpr': tpr,
                'cwg': NUM_COMPUTE_WGS_REF,
            })

    total = len(cases)
    n_fused = sum(1 for c in cases if c['variant'] == 'fused')
    n_ref   = total - n_fused

    print(f"\nTest matrix:")
    print(f"  M values:  {len(M_VALUES)}  {M_VALUES}")
    print(f"  N values:  {N_VALUES}")
    print(f"  K values:  {K_VALUES}")
    print(f"  Chunks:    {NUM_CHUNKS_VALUES} (fused only)")
    print(f"  tokens_per_reduction: {TOKENS_PER_REDUCTION_VALUES}")
    print(f"  num_compute_wgs (fused): {NUM_COMPUTE_WGS_FUSED_VALUES}")
    print(f"  num_compute_wgs (ref):   {NUM_COMPUTE_WGS_REF}")
    print(f"  Config:    {CONFIG_ID}")
    print(f"\n  Fused cases:       {n_fused}")
    print(f"  Ref cases:         {n_ref} ({n_ref//2} default + {n_ref//2} rdc)")
    print(f"  Total cases:       {total}")
    # ---- Resume: load already-completed keys ----
    done_perf   = set()
    done_timing = set()
    if resuming:
        done_perf   = load_completed_keys(RESULT_CSV)
        done_timing = load_completed_keys(TIMING_CSV)
        n_skip_perf   = sum(1 for c in cases
                            if case_key(c['variant'], c['M'], c['N'], c['K'],
                                        c['num_chunks'] if c['variant'] == 'fused' else '',
                                        c['tpr'], c['cwg']) in done_perf)
        n_skip_timing = sum(1 for c in cases if c['variant'] == 'fused'
                            and case_key(c['variant'], c['M'], c['N'], c['K'],
                                         c['num_chunks'], c['tpr'], c['cwg']) in done_timing)
        print(f"  *** RESUMING from {RESULT_CSV}")
        print(f"  *** Perf cases already done:   {n_skip_perf}/{total}")
        print(f"  *** Timing cases already done:  {n_skip_timing}/{n_fused}")
    print(f"\n  Results:  {RESULT_CSV}")
    print(f"  Timing:   {TIMING_CSV}")
    print(f"  Logs:     {LOG_FILE}")
    print()

    # ---- CSV columns ----
    perf_fieldnames = [
        'variant', 'M', 'N', 'K', 'num_chunks',
        'tokens_per_reduction', 'num_compute_wgs', 'config',
        'avg_ms', 'tflops',
    ]
    timing_fieldnames = [
        'variant', 'M', 'N', 'K', 'num_chunks',
        'tokens_per_reduction', 'num_compute_wgs', 'config',
        'timing_avg_ms', 'timing_tflops',
        'pp_wait_sum_avg_us', 'pp_reduce_sum_avg_us',
        'pp_wait_max_us', 'pp_reduce_max_us',
        'pp_total_sum_avg_us', 'pp_total_max_us',
    ]

    succeeded = 0
    failed = 0
    timing_succeeded = 0
    timing_failed = 0
    skipped_perf = 0
    skipped_timing = 0
    interrupted = False

    def print_summary(completed_idx):
        """Print summary (called on normal exit or Ctrl+C)."""
        tag = "INTERRUPTED" if interrupted else "complete"
        print("\n" + "=" * 50)
        print("Benchmark {}!  ({}/{} cases)".format(tag, completed_idx, total))
        if skipped_perf or skipped_timing:
            print("  Skipped (resume): {} perf, {} timing".format(
                skipped_perf, skipped_timing))
        print("  Perf runs:    {} total, {} ok, {} failed".format(
            succeeded + failed, succeeded, failed))
        print("  Timing runs:  {} total, {} ok, {} failed".format(
            timing_succeeded + timing_failed, timing_succeeded, timing_failed))
        print("  Results:  {}".format(RESULT_CSV))
        print("  Timing:   {}".format(TIMING_CSV))
        print("  Logs:     {}".format(LOG_FILE))
        print("=" * 50)

    # Open in append mode when resuming (headers already exist), write mode otherwise.
    file_mode = 'a' if resuming else 'w'

    with open(LOG_FILE, file_mode) as log_fp, \
         open(RESULT_CSV, file_mode, newline='') as csv_fp, \
         open(TIMING_CSV, file_mode, newline='') as timing_fp:

        writer = csv.DictWriter(csv_fp, fieldnames=perf_fieldnames)
        timing_writer = csv.DictWriter(timing_fp, fieldnames=timing_fieldnames)
        if not resuming:
            writer.writeheader()
            timing_writer.writeheader()

        if resuming:
            log_fp.write("\n\n{}\n".format("=" * 70))
            log_fp.write("=== RESUMED at {} ===\n".format(
                datetime.now().strftime('%Y-%m-%d %H:%M:%S')))
            log_fp.write("{}\n\n".format("=" * 70))

        last_completed = 0

        try:
            for idx, case in enumerate(cases, 1):
                # ---- Case key ----
                nc_str = case['num_chunks'] if case['variant'] == 'fused' else ''
                ck = case_key(case['variant'], case['M'], case['N'], case['K'],
                              nc_str, case['tpr'], case['cwg'])

                perf_done   = ck in done_perf
                timing_done = ck in done_timing  # only relevant for fused

                # Skip entirely if both perf and timing (if applicable) are done
                if perf_done and (case['variant'] != 'fused' or timing_done):
                    skipped_perf += 1
                    if case['variant'] == 'fused':
                        skipped_timing += 1
                    last_completed = idx
                    continue

                # ---- Progress ----
                desc = "{} M={} N={} K={} tpr={} cwg={}".format(
                    case['variant'], case['M'], case['N'], case['K'],
                    case['tpr'], case['cwg'])
                if case['variant'] == 'fused':
                    desc += " chunks={}".format(case['num_chunks'])
                parts = []
                if perf_done:
                    parts.append("perf:skip")
                if case['variant'] == 'fused' and timing_done:
                    parts.append("timing:skip")
                suffix = "  ({})".format(", ".join(parts)) if parts else ""
                print("Running {}/{}: {}{}".format(idx, total, desc, suffix), flush=True)

                # ---- Build base command (no --timing) ----
                cmd = [MPI_LAUNCHER] + MPI_FLAGS + ["-np", str(NUM_RANKS), case['exe']]
                cmd += ["--m", str(case['M']),
                        "--n", str(case['N']),
                        "--k", str(case['K'])]
                cmd += ["--config", str(CONFIG_ID)]
                cmd += ["--warmup", str(WARMUP), "--repeat", str(REPEAT)]
                cmd += ["--verify", "0", "--compare", "0"]
                cmd += ["--tokens_per_reduction", str(case['tpr'])]

                if case['variant'] == 'fused':
                    cmd += ["--num_chunks", str(case['num_chunks'])]
                    cmd += ["--num_compute_wgs", str(case['cwg'])]
                else:
                    cmd += ["--forced_num_compute_wgs", str(case['cwg'])]

                # ---- Set ROCSHMEM_HEAP_SIZE for fused kernel ----
                run_env = None
                if case['variant'] == 'fused':
                    heap_bytes = estimate_rocshmem_heap_bytes(
                        case['M'], case['N'], case['tpr'])
                    heap_gib = heap_bytes >> 30
                    run_env = os.environ.copy()
                    run_env['ROCSHMEM_HEAP_SIZE'] = str(heap_bytes)
                    print("  ROCSHMEM_HEAP_SIZE={}G ({} bytes)".format(
                        heap_gib, heap_bytes), flush=True)

                # ---- Run perf (unless already done) ----
                if not perf_done:
                    stdout, stderr, rc = run_cmd(cmd, env=run_env)

                    # Write log
                    log_fp.write("\n{}\n".format("=" * 70))
                    log_fp.write("[{}/{}] PERF {}\n".format(idx, total, desc))
                    log_fp.write("CMD: {}\n".format(" ".join(cmd)))
                    log_fp.write("Return code: {}\n".format(rc))
                    log_fp.write("{}\n".format("=" * 70))
                    log_fp.write(stdout)
                    if stderr.strip():
                        log_fp.write("\n--- STDERR ---\n{}\n".format(stderr))
                    log_fp.flush()

                    # Parse perf timing
                    avg_ms, tflops = (None, None)
                    if rc == 0:
                        avg_ms, tflops = parse_persistent_ms(stdout)

                    ok = avg_ms is not None
                    if ok:
                        succeeded += 1
                    else:
                        failed += 1

                    # Write perf row
                    perf_row = {
                        'variant':              case['variant'],
                        'M':                    case['M'],
                        'N':                    case['N'],
                        'K':                    case['K'],
                        'num_chunks':           nc_str,
                        'tokens_per_reduction': case['tpr'],
                        'num_compute_wgs':      case['cwg'],
                        'config':               CONFIG_ID,
                        'avg_ms':               avg_ms if avg_ms is not None else '',
                        'tflops':               tflops if tflops is not None else '',
                    }
                    writer.writerow(perf_row)
                    csv_fp.flush()
                else:
                    skipped_perf += 1

                # ---- Separate timing run for fused kernel ----
                if case['variant'] == 'fused' and not timing_done:
                    cleanup_timing_csvs()

                    timing_cmd = cmd + ["--timing", "1"]
                    print("  + timing run...", flush=True)
                    t_stdout, t_stderr, t_rc = run_cmd(timing_cmd, env=run_env)

                    # Log timing run
                    log_fp.write("\n{}\n".format("-" * 70))
                    log_fp.write("[{}/{}] TIMING {}\n".format(idx, total, desc))
                    log_fp.write("CMD: {}\n".format(" ".join(timing_cmd)))
                    log_fp.write("Return code: {}\n".format(t_rc))
                    log_fp.write("{}\n".format("-" * 70))
                    log_fp.write(t_stdout)
                    if t_stderr.strip():
                        log_fp.write("\n--- STDERR ---\n{}\n".format(t_stderr))
                    log_fp.flush()

                    # Parse timing results
                    t_avg_ms, t_tflops = (None, None)
                    if t_rc == 0:
                        t_avg_ms, t_tflops = parse_persistent_ms(t_stdout)

                    t_ok = t_avg_ms is not None
                    if t_ok:
                        timing_succeeded += 1
                    else:
                        timing_failed += 1

                    # Parse PP summary CSVs
                    summary = {}
                    if t_ok:
                        summary = parse_summary_csvs(case['M'], case['K'], case['N'])

                    timing_row = {
                        'variant':              case['variant'],
                        'M':                    case['M'],
                        'N':                    case['N'],
                        'K':                    case['K'],
                        'num_chunks':           case['num_chunks'],
                        'tokens_per_reduction': case['tpr'],
                        'num_compute_wgs':      case['cwg'],
                        'config':               CONFIG_ID,
                        'timing_avg_ms':        t_avg_ms if t_avg_ms is not None else '',
                        'timing_tflops':        t_tflops if t_tflops is not None else '',
                        'pp_wait_sum_avg_us':   summary.get('pp_wait_sum_avg_us', ''),
                        'pp_reduce_sum_avg_us': summary.get('pp_reduce_sum_avg_us', ''),
                        'pp_wait_max_us':       summary.get('pp_wait_max_us', ''),
                        'pp_reduce_max_us':     summary.get('pp_reduce_max_us', ''),
                        'pp_total_sum_avg_us':  summary.get('pp_total_sum_avg_us', ''),
                        'pp_total_max_us':      summary.get('pp_total_max_us', ''),
                    }
                    timing_writer.writerow(timing_row)
                    timing_fp.flush()
                elif case['variant'] == 'fused' and timing_done:
                    skipped_timing += 1

                last_completed = idx

        except KeyboardInterrupt:
            interrupted = True
            print("\n\n*** Ctrl+C received — stopping after {}/{} cases. ***".format(
                last_completed, total), flush=True)
            print("*** Partial results are saved (CSVs flushed after every row). ***",
                  flush=True)
            print("*** Resume with:  python3 {} --resume {} ***\n".format(
                sys.argv[0], RESULT_CSV), flush=True)

    # ---- Final summary (always printed) ----
    print_summary(last_completed)


if __name__ == '__main__':
    main()

