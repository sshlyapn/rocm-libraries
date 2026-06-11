#!/usr/bin/env python3
"""Parse AMDVLK PAL GpuProfiler per-frame engine CSV to separate pure kernel
(dispatch) GPU-busy time from inter-kernel barrier / cache-flush overhead.

Each row is one queue/cmdbuf call. Start/End Clock are in ticks of a fixed
frequency declared in the "Time (us) [Frequency: N]" column header. We recompute
durations from clocks so barriers (which have no precomputed Time) are included.
"""
import csv
import glob
import os
import re
import sys
from collections import defaultdict


def parse_freq(header_row):
    for col in header_row:
        m = re.search(r"Frequency:\s*(\d+)", col)
        if m:
            return int(m.group(1))
    return 100_000_000


def analyze(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    header = rows[0]
    freq = parse_freq(header)
    tick_us = 1e6 / freq

    idx = {name: i for i, name in enumerate(header)}
    c_call = idx["CmdBuffer Call"]
    c_start = idx["Start Clock"]
    c_end = idx["End Clock"]
    c_phash = idx["PipelineHash"]
    c_tg = idx["Verts/ThreadGroups"]

    def dur(r):
        try:
            return (int(r[c_end]) - int(r[c_start])) * tick_us
        except (ValueError, IndexError):
            return None

    disp_time = 0.0
    disp_n = 0
    barrier_time = 0.0
    barrier_n = 0
    other_time = 0.0
    span_start = None
    span_end = None
    per_pipeline = defaultdict(lambda: [0, 0.0])  # hash -> [count, total_us]

    for r in rows[1:]:
        if len(r) <= c_end:
            continue
        call = r[c_call].strip()
        d = dur(r)
        if d is None:
            continue
        # frame span tracking from any timed row
        s, e = int(r[c_start]), int(r[c_end])
        span_start = s if span_start is None else min(span_start, s)
        span_end = e if span_end is None else max(span_end, e)

        if call == "CmdDispatch()":
            disp_time += d
            disp_n += 1
            ph = r[c_phash]
            per_pipeline[ph][0] += 1
            per_pipeline[ph][1] += d
        elif call in ("CmdReleaseThenAcquire()", "CmdPipelineBarrier()",
                      "CmdReleaseThenAcquire", "CmdBarrier()"):
            barrier_time += d
            barrier_n += 1
        elif call in ("Begin()", "End()", ""):
            pass
        else:
            other_time += d

    span_us = (span_end - span_start) * tick_us if span_start is not None else 0.0

    print(f"file: {os.path.basename(path)}")
    print(f"  frame span (first->last clock) : {span_us:10.2f} us")
    print(f"  dispatches                     : {disp_n:6d} kernels, {disp_time:10.2f} us  ({100*disp_time/span_us:5.1f}% of span)")
    print(f"  barriers (ReleaseThenAcquire)  : {barrier_n:6d}      , {barrier_time:10.2f} us  ({100*barrier_time/span_us:5.1f}% of span)")
    print(f"  other timed calls              :        {'':6s}, {other_time:10.2f} us  ({100*other_time/span_us:5.1f}% of span)")
    accounted = disp_time + barrier_time + other_time
    print(f"  unaccounted (idle/CP/overlap)  :        {'':6s}, {span_us-accounted:10.2f} us  ({100*(span_us-accounted)/span_us:5.1f}% of span)")
    print(f"  avg per-barrier gap            : {barrier_time/max(barrier_n,1):8.3f} us")
    print()
    print("  top pipelines by total GPU time:")
    top = sorted(per_pipeline.items(), key=lambda kv: kv[1][1], reverse=True)[:12]
    for ph, (n, t) in top:
        print(f"    {ph:>22s}  x{n:<5d}  {t:9.2f} us total  {t/n:7.3f} us/call")
    return span_us, disp_time, barrier_time, disp_n, barrier_n


if __name__ == "__main__":
    pats = sys.argv[1:] or ["/tmp/amdprof/gpuprof/*/*EngAce0*.csv"]
    files = []
    for p in pats:
        files.extend(sorted(glob.glob(p)))
    if not files:
        print("no CSV files matched", pats)
        sys.exit(1)
    for fp in files:
        analyze(fp)
