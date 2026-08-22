#!/bin/bash
# uartcheck.sh — is this USB-TTL adapter alive?
#
# Jumper the adapter's TXD to its own RXD, unplug everything else, then run.
# With no argument it tests every USB-serial bridge it can find.
#
#   ./uartcheck.sh                       # test all
#   ./uartcheck.sh /dev/cu.usbserial-0001
#
# Why this exists (2026-08-22): an adapter that enumerates, opens and accepts
# writes without error can still pass no data at all. That failure looks
# identical to bad lock wiring, a LockSim bug, or dead firmware — and cost a
# bench session to find. A loopback settles it in two seconds.
#
# NOTE: a loopback cannot tell "TX dead" from "RX dead" — it fails the same way
# for both. It only answers "does data traverse this adapter, yes or no".

test_one() {
  local DEV="$1"
  [ -e "$DEV" ] || { printf "  %-32s ABSENT\n" "$DEV"; return; }
  local HOLDER
  HOLDER=$(lsof "$DEV" 2>/dev/null | tail -n +2 | awk '{print $1}' | head -1)
  [ -n "$HOLDER" ] && { printf "  %-32s BUSY (held by %s)\n" "$DEV" "$HOLDER"; return; }

  stty -f "$DEV" 9600 cs8 -cstopb -parenb raw 2>/dev/null
  python3 - "$DEV" <<'PY'
import os, sys, select, time, fcntl, struct
p = sys.argv[1]
fd = os.open(p, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    # Some bridges keep the transceiver off until DTR/RTS are high.
    fcntl.ioctl(fd, 0x8004746c, struct.pack('I', 0x0002 | 0x0004))
    time.sleep(0.3)
    while select.select([fd], [], [], 0.2)[0]:          # drain stale bytes
        try: os.read(fd, 4096)
        except BlockingIOError: break
    msg = b"UARTCHECK\n"
    os.write(fd, msg)
    got, end = b"", time.time() + 2.5
    while time.time() < end:
        if select.select([fd], [], [], 0.3)[0]:
            try: got += os.read(fd, 4096)
            except BlockingIOError: pass
    ok = "PASS" if got.strip() else "FAIL  (no jumper? or dead adapter)"
    print(f"  {p:32} wrote {len(msg)}, read {len(got):3}  {ok}")
finally:
    os.close(fd)
PY
}

echo "USB-serial bridges on the bus:"
ioreg -p IOUSB -l -w 0 2>/dev/null | awk '
/"USB Product Name"/ {n=$0; sub(/.*= "/,"",n); sub(/".*/,"",n)}
/"idVendor"/  {v=$NF}
/"idProduct"/ {p=$NF}
/"USB Serial Number"/ {s=$0; sub(/.*= "/,"",s); sub(/".*/,"",s);
  if (n ~ /Serial|UART|CP210|Single/) printf "  %-40s vid=0x%04X pid=0x%04X serial=%s\n", n, v, p, s}' | sort -u
echo

if [ -n "$1" ]; then
  test_one "$1"
else
  found=0
  for d in /dev/cu.usbserial-* /dev/cu.usbmodem[0-9A-Z]* /dev/cu.SLAB_* /dev/cu.wchusbserial*; do
    # skip the ESP32s' own native-USB consoles: those are boards, not adapters
    case "$d" in *usbmodem14*|*usbmodem142*|*usbmodem143*) continue;; esac
    [ -e "$d" ] || continue
    test_one "$d"; found=1
  done
  [ "$found" = 0 ] && echo "  no candidate adapters found"
fi

exit 0
