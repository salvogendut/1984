#!/bin/bash
# One-command Albireo USB HID mouse trace capture for issue #122.
#
# Usage:
#   debug/issue-122-albireo-mouse/trace.sh [PATH_TO_ALBIREO_IMAGE]
#
# Boots 1984 with Albireo enabled (mouse capture engaged), streams every
# CH376 command + interrupt code to debug/issue-122-albireo-mouse/trace.log.
# Click in the window to capture the mouse, boot SymbOS, move the mouse,
# then quit (F12 or close window). The log gets saved alongside this script.
#
# What to look for in the log (see NOTES.md for full guidance):
#   - SET_USB_MODE → does SymbOS pick 0x06 (host, no SOF) or 0x07 (host, SOF)?
#   - SET_ADDRESS / SET_USB_ADDR / SET_CONFIG sequence — do all return SUCCESS?
#   - ISSUE_TKN_X polls — what endpoint byte is SymbOS sending?
#   - Any descriptor reads (CTRL_TRANSFER, GET_DESCRIPTOR) we may be skipping?

set -euo pipefail

cd "$(dirname "$0")/../.."

ALBIREO_IMG="${1:-${HOME}/Dev/1984-images/albireo-symbos.img}"
LOG="debug/issue-122-albireo-mouse/trace.log"

if [[ ! -f "$ALBIREO_IMG" ]]; then
  echo "error: Albireo image not found: $ALBIREO_IMG" >&2
  echo "       Pass the path as the first argument, or place it at the default." >&2
  exit 1
fi

if [[ ! -x ./1984 ]]; then
  echo "note: ./1984 not built — building inside distrobox" >&2
  distrobox enter my-distrobox -- make -j4
fi

echo "Tracing to $LOG"
echo "Click the window to engage mouse capture, boot SymbOS, then F12 or close."

./1984 \
  --trace-albireo \
  2> >(tee "$LOG" >&2)
