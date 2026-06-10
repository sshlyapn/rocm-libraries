#!/usr/bin/env bash
# Pin the AMD Radeon PRO W7900 (gfx1100 / Navi 31) to a stable, below-boost clock
# for reproducible benchmarking.
#
# WHY: the GPU autosuspends when idle (power/control=auto, runtime_status=suspended),
# then wakes and ramps clocks unpredictably on each run. That is the source of the
# bimodal timing noise -- NOT boost-vs-base. This script keeps the device awake and
# pins a fixed performance level.
#
# MUST be run as root on the HOST (sysfs is read-only inside the unprivileged
# container). Companion: gpu_restore_freq.sh reverts to the saved original state.
#
# Usage:
#   sudo ./gpu_pin_freq.sh [level]
#     level (optional, default profile_standard):
#       profile_standard  fixed mid clock, below boost   <- recommended (stable)
#       profile_peak      highest stable clock (no opportunistic boost)
#       profile_min_sclk  lowest sclk
#       manual            then set pp_dpm_sclk/pp_dpm_mclk by hand (see note below)
#       auto              stock dynamic (boosty)
#   Override card detection with: CARD=cardN sudo ./gpu_pin_freq.sh
#
# NOTE (manual specific level): after running with 'manual', do e.g.:
#   cat /sys/class/drm/card1/device/pp_dpm_sclk   # list levels
#   echo 1 > /sys/class/drm/card1/device/pp_dpm_sclk
#   echo 1 > /sys/class/drm/card1/device/pp_dpm_mclk
#
# WARNING: this is GLOBAL to the card -- it affects every container/user on this
# GPU. profile_standard lowers the clock and will slow others' workloads. Check
# 'amd-smi process' before running.
set -euo pipefail

LEVEL="${1:-profile_standard}"
PCI_ID="1002:7448"                        # Radeon PRO W7900 (Navi 31)
STATE_FILE="/var/tmp/gpu_freq_state.W7900"

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: must run as root, e.g. sudo $0" >&2
  exit 1
fi

# Validate LEVEL against an allowlist (this runs as root; no arbitrary writes).
case "$LEVEL" in
  auto|low|high|manual|profile_standard|profile_peak|profile_min_sclk|profile_min_mclk) ;;
  *) echo "ERROR: invalid level '$LEVEL'" >&2; exit 1 ;;
esac

# Locate the W7900 card (or use CARD=cardN override; validated against cardN form).
CARD_DEV=""
if [ -n "${CARD:-}" ]; then
  case "$CARD" in
    card[0-9]|card[0-9][0-9]) ;;
    *) echo "ERROR: invalid CARD '$CARD' (expected cardN)" >&2; exit 1 ;;
  esac
  CARD_DEV="/sys/class/drm/${CARD}/device"
else
  for d in /sys/class/drm/card*/device; do
    [ -f "$d/uevent" ] || continue
    if grep -q "PCI_ID=${PCI_ID}" "$d/uevent" 2>/dev/null; then CARD_DEV="$d"; break; fi
  done
fi
if [ -z "$CARD_DEV" ] || [ ! -d "$CARD_DEV" ]; then
  echo "ERROR: W7900 (${PCI_ID}) not found; pass CARD=cardN explicitly." >&2
  exit 1
fi

PC="$CARD_DEV/power/control"
FPL="$CARD_DEV/power_dpm_force_performance_level"
echo "Target card : $CARD_DEV"

# Warn about other GPU users.
if command -v amd-smi >/dev/null 2>&1; then
  echo "--- amd-smi process (verify no other users before pinning) ---"
  amd-smi process 2>/dev/null | grep -iE 'GPU:|PID:|NAME:' | head -20 || true
  echo "-------------------------------------------------------------"
fi

# Save the ORIGINAL state once (do not clobber on re-run).
if [ ! -f "$STATE_FILE" ]; then
  orig_pc="$(cat "$PC" 2>/dev/null || echo auto)"
  orig_fpl="$(cat "$FPL" 2>/dev/null || echo auto)"
  [ -n "$orig_fpl" ] || orig_fpl="auto"
  printf 'CARD_DEV=%s\nPC=%s\nFPL=%s\n' "$CARD_DEV" "$orig_pc" "$orig_fpl" > "$STATE_FILE"
  echo "Saved original state -> $STATE_FILE (power/control=$orig_pc force_perf=$orig_fpl)"
else
  echo "Original state already saved at $STATE_FILE (kept as-is)."
fi

# 1) Keep the GPU awake so clocks are stable and the level file is writable.
echo on > "$PC"
sleep 1
echo "runtime_status -> $(cat "$CARD_DEV/power/runtime_status" 2>/dev/null || echo n/a)"

# 2) Pin the performance level.
echo "$LEVEL" > "$FPL"
sleep 1
echo "force_performance_level -> $(cat "$FPL" 2>/dev/null || echo n/a)"

echo "--- pp_dpm_sclk ---"; cat "$CARD_DEV/pp_dpm_sclk" 2>/dev/null || true
echo "--- pp_dpm_mclk ---"; cat "$CARD_DEV/pp_dpm_mclk" 2>/dev/null || true
echo
echo "Pinned. Restore the original state with: sudo ./gpu_restore_freq.sh"
