#!/bin/bash
# Benchmark persistent vs non-persistent GEMM kernel across multiple sizes.
# Usage: bash bench_persistent.sh [path_to_binary]
#
# Run inside docker:
#   docker exec -it sshliapn-rocm-7.2.0 bash
#   cd /home/sshliapn/code/rocm-libraries/projects/composablekernel/build
#   bash ../example/ck_tile/03_gemm_universal_minimal/bench_persistent.sh

set -euo pipefail

BIN="${1:-./bin/tile_example_gemm_universal_persistent}"

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: binary not found or not executable: $BIN"
    exit 1
fi

WARMUP=50
REPEAT=1000

# ── Size matrix ──────────────────────────────────────────────────────────────
# Square sizes
SQUARE_SIZES="256 512 1024 2048 4096 8192 16384"

# Tall-skinny (large M, small N)
TALL_SKINNY=(
    "8192 128 4096"
    "8192 256 4096"
    "16384 128 4096"
    "16384 256 4096"
)

# Short-wide (small M, large N)
SHORT_WIDE=(
    "128 8192 4096"
    "256 8192 4096"
    "128 16384 4096"
    "256 16384 4096"
)

# LLM-like shapes (from common transformer FFN / attention dims)
LLM_SHAPES=(
    "1 4096 4096"
    "4 4096 4096"
    "16 4096 4096"
    "32 4096 4096"
    "128 4096 4096"
    "256 4096 4096"
    "512 4096 4096"
    "1024 4096 4096"
    "2048 4096 4096"
    "4096 4096 4096"
    "1 4096 11008"
    "32 4096 11008"
    "128 4096 11008"
    "256 4096 11008"
    "1024 4096 11008"
    "4096 4096 11008"
)

# ── Output formatting ────────────────────────────────────────────────────────
SEP="$(printf '%0.s─' {1..110})"
FMT="%-8s %-8s %-8s │ %12s %12s │ %12s %12s │ %8s\n"

print_header() {
    echo "$SEP"
    printf "$FMT" "M" "N" "K" "persist_ms" "persist_TF" "nonpers_ms" "nonpers_TF" "speedup"
    echo "$SEP"
}

run_one() {
    local m=$1 n=$2 k=$3
    local out
    out=$("$BIN" --m "$m" --n "$n" --k "$k" \
                 --warmup "$WARMUP" --repeat "$REPEAT" --cu_scale 0.5 \
                 --verify 0 --compare 1 2>&1)

    # Parse persistent results
    local p_ms p_tf np_ms np_tf speedup
    p_ms=$(echo "$out"  | grep '\[persistent\]'     | awk '{print $2}' | cut -d= -f2)
    p_tf=$(echo "$out"  | grep '\[persistent\]'     | awk '{print $3}' | cut -d= -f2)
    np_ms=$(echo "$out" | grep '\[non-persistent\]' | awk '{print $2}' | cut -d= -f2)
    np_tf=$(echo "$out" | grep '\[non-persistent\]' | awk '{print $3}' | cut -d= -f2)
    speedup=$(echo "$out" | grep 'speedup' | awk '{print $NF}')

    # Fallback if parsing failed
    p_ms=${p_ms:-N/A}
    p_tf=${p_tf:-N/A}
    np_ms=${np_ms:-N/A}
    np_tf=${np_tf:-N/A}
    speedup=${speedup:-N/A}

    printf "$FMT" "$m" "$n" "$k" "$p_ms" "$p_tf" "$np_ms" "$np_tf" "$speedup"
}

# ── Run benchmarks ───────────────────────────────────────────────────────────
echo "=================================================================="
echo "  Persistent vs Non-Persistent GEMM Kernel Benchmark"
echo "  Binary: $BIN"
echo "  Warmup: $WARMUP   Repeat: $REPEAT"
echo "=================================================================="
echo ""

# --- Square sizes ---
echo "▶ Square matrices (M=N=K)"
print_header
for sz in $SQUARE_SIZES; do
    run_one "$sz" "$sz" "$sz"
done
echo "$SEP"
echo ""

# --- Tall-skinny ---
echo "▶ Tall-skinny matrices (large M, small N)"
print_header
for shape in "${TALL_SKINNY[@]}"; do
    read -r m n k <<< "$shape"
    run_one "$m" "$n" "$k"
done
echo "$SEP"
echo ""

# --- Short-wide ---
echo "▶ Short-wide matrices (small M, large N)"
print_header
for shape in "${SHORT_WIDE[@]}"; do
    read -r m n k <<< "$shape"
    run_one "$m" "$n" "$k"
done
echo "$SEP"
echo ""

# --- LLM-like shapes ---
echo "▶ LLM-like shapes (batch × hidden × FFN)"
print_header
for shape in "${LLM_SHAPES[@]}"; do
    read -r m n k <<< "$shape"
    run_one "$m" "$n" "$k"
done
echo "$SEP"
echo ""

echo "Done."

