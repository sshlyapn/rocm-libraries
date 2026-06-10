#!/usr/bin/env bash
# Restore the AMD Radeon PRO W7900 power/clock state that gpu_pin_freq.sh changed.
#
# Reads the saved original values from /var/tmp/gpu_freq_state.W7900 and reverts
# both power_dpm_force_performance_level and power/control. If the state file is
# missing, falls back to the stock defaults (auto / auto).
#
# MUST be run as root on the HOST. Usage: sudo ./gpu_restore_freq.sh
set -euo pipefail

PCI_ID="1002:7448"                        # Radeon PRO W7900 (Navi 31)
STATE_FILE="/var/tmp/gpu_freq_state.W7900"

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: must run as root, e.g. sudo $0" >&2
  exit 1
fi

CARD_DEV=""; PC_VAL="auto"; FPL_VAL="auto"
if [ -f "$STATE_FILE" ]; then
  # shellcheck disable=SC1090
  . "$STATE_FILE"
  CARD_DEV="${CARD_DEV:-}"
  PC_VAL="${PC:-auto}"
  FPL_VAL="${FPL:-auto}"
  echo "Loaded saved state from $STATE_FILE"
else
  echo "No saved state file; falling back to stock defaults (auto/auto)."
fi

# Fall back to detecting the card if the saved path is missing/invalid.
if [ -z "$CARD_DEV" ] || [ ! -d "$CARD_DEV" ]; then
  for d in /sys/class/drm/card*/device; do
    [ -f "$d/uevent" ] || continue
    if grep -q "PCI_ID=${PCI_ID}" "$d/uevent" 2>/dev/null; then CARD_DEV="$d"; break; fi
  done
fi
if [ -z "$CARD_DEV" ] || [ ! -d "$CARD_DEV" ]; then
  echo "ERROR: W7900 (${PCI_ID}) not found." >&2
  exit 1
fi

[ -n "$FPL_VAL" ] || FPL_VAL="auto"
[ -n "$PC_VAL" ]  || PC_VAL="auto"
echo "Target card : $CARD_DEV"
echo "Restoring   : force_perf=$FPL_VAL  power/control=$PC_VAL"

# Restore performance level first (device is awake from the pin), then power control.
echo "$FPL_VAL" > "$CARD_DEV/power_dpm_force_performance_level" 2>/dev/null \
  || echo auto > "$CARD_DEV/power_dpm_force_performance_level"
echo "$PC_VAL" > "$CARD_DEV/power/control" 2>/dev/null \
  || echo auto > "$CARD_DEV/power/control"

echo "force_performance_level -> $(cat "$CARD_DEV/power_dpm_force_performance_level" 2>/dev/null || echo n/a)"
echo "power/control          -> $(cat "$CARD_DEV/power/control" 2>/dev/null || echo n/a)"
echo "runtime_status         -> $(cat "$CARD_DEV/power/runtime_status" 2>/dev/null || echo n/a)"

rm -f "$STATE_FILE" && echo "Removed $STATE_FILE"
echo "Restored."
