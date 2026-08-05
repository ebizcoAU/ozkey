#!/usr/bin/env bash
# flashwatch.sh — compile, upload, and resume the serial capture in one shot.
#
# The point is the ORDER. Handing the port back and forth by hand cost us most of
# a session: duallog holds the port so an upload fails, or the upload wins and
# duallog is left reading a dead fd while looking perfectly healthy. Worse, the
# board re-enumerates on every flash, so a capture started before the upload is
# guaranteed to be deaf afterwards — which is how "no touch registered" got
# reported when the instrument, not the lock, was the thing that was silent.
#
# Also: attaching AFTER the reset misses the boot banner, and the boot banner is
# now where [BOOT] reset reason lives. Starting the reader immediately after
# upload is the only way to catch why the board came up.
set -uo pipefail

SP="$(cd "$(dirname "$0")" && pwd)"
SKETCH="${1:-/Users/truongvietphan/Documents/Dev/ozkey/blelock/doorlock}"
PORT="${2:-/dev/cu.usbmodem1432301}"
OTHER="/dev/cu.usbmodem141201"
FQBN="esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB"

echo "=== 1/4 stop capture ==="
pkill -f "duallog.py /dev/cu.usbmodem" 2>/dev/null
sleep 1

echo "=== 2/4 compile ==="
if ! arduino-cli compile --fqbn "$FQBN" "$SKETCH" 2>&1 | tail -3; then
  echo "COMPILE FAILED — not flashing, capture NOT restarted"; exit 1
fi

echo "=== 3/4 upload -> $PORT ==="
if ! arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH" 2>&1 | tail -5; then
  echo "UPLOAD FAILED"; exit 1
fi

echo "=== 4/4 resume capture ==="
# The upload tool hard-resets via RTS, and the board re-enumerates AFTER that.
# Opening the port too early gets a descriptor for the node that is about to be
# destroyed — duallog then looks alive and reads nothing, which is the exact
# false-negative that had us reporting "no touch registered" all night.
# So: settle, start, then PROVE data is flowing before declaring success.
start_capture() {
  [ -f "$SP/serial.log" ] && mv "$SP/serial.log" "$SP/serial-$(date +%H%M%S).log"
  nohup python3 "$SP/duallog.py" "$OTHER" "$PORT" >> "$SP/serial.log" 2>&1 &
}
sleep 4
for attempt in 1 2 3; do
  for i in $(seq 1 20); do [ -e "$PORT" ] && break; sleep 0.5; done
  start_capture
  # a [MON] line lands every 10 s, so 14 s is a fair verdict
  for i in $(seq 1 14); do
    sleep 1
    if [ "$(wc -l < "$SP/serial.log")" -gt 1 ]; then
      echo "capture LIVE (attempt $attempt):"
      grep -E "FW\]|BOOT\]|bond #0:|advertising" "$SP/serial.log" | head -6 | cut -c1-115
      [ "$(grep -c . "$SP/serial.log")" -gt 1 ] && exit 0
    fi
  done
  echo "attempt $attempt: no data in 14 s — restarting reader"
  pkill -f "duallog.py /dev/cu.usbmodem" 2>/dev/null; sleep 1
done
echo "CAPTURE FAILED after 3 attempts — port may need a physical replug"
exit 1
