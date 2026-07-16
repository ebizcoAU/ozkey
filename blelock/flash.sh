#!/usr/bin/env bash
# flash.sh — compile + upload + monitor an ESP32-C6 sketch in one shot.
#
# Usage:
#   ./flash.sh [4M|8M|16M] [sketch-dir]
#
#   ./flash.sh                # 8M, blelock/blelock sketch
#   ./flash.sh 4M             # 4MB flash board
#   ./flash.sh 8M ./Touch     # another sketch in this folder
#
# Ctrl+C in the monitor exits the script.

set -euo pipefail

SIZE="${1:-8M}"
SKETCH="${2:-$(dirname "$0")/blelock}"
BAUD=115200

case "$SIZE" in
  4M|4MB)   FLASH=4M;  SCHEME=default ;;
  8M|8MB)   FLASH=8M;  SCHEME=default_8MB ;;
  16M|16MB) FLASH=16M; SCHEME=app3M_fat9M_16MB ;;   # C6 core has no default_16MB
  *) echo "Unknown flash size '$SIZE' (use 4M, 8M or 16M)" >&2; exit 1 ;;
esac

FQBN="esp32:esp32:esp32c6:FlashSize=${FLASH},PartitionScheme=${SCHEME}"

PORTS=(/dev/cu.usbmodem*)
if [[ ! -e "${PORTS[0]}" ]]; then
  echo "No /dev/cu.usbmodem* device found — is the board plugged in?" >&2
  exit 1
fi
PORT="${PORTS[0]}"
if (( ${#PORTS[@]} > 1 )); then
  echo "Multiple boards found (${PORTS[*]}), using $PORT"
fi

echo "== Sketch : $SKETCH"
echo "== FQBN   : $FQBN"
echo "== Port   : $PORT @ ${BAUD} baud"

arduino-cli compile --fqbn "$FQBN" "$SKETCH"
arduino-cli upload  --fqbn "$FQBN" -p "$PORT" "$SKETCH"
exec arduino-cli monitor -p "$PORT" -c "baudrate=${BAUD}"
