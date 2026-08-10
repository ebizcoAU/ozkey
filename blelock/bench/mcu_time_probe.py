#!/usr/bin/env python3
"""
ozkey-21 §2.3 — does our ESP32 serve time to the lock MCU?

Stands in for the Tuya lock MCU on the CP2102 UART (GPIO16/17, 9600 8N1) and
asks the question a real MCU asks: 0x1C "what is the local time?".

WHY THIS EXISTS
---------------
Tuya's architecture makes the MODULE the time source and the MCU its client:

    "the module comes with a software real-time clock (RTC) that provides time
     for the MCU, and therefore, even when the module is offline, the MCU can
     also get the time."

We replaced the Tuya module with our own ESP32 and never implemented that
service. If that is true, DP 21/23 temp PIN/RFID windows have never been
enforceable, because the MCU was never told what time it is.

A REPLY means the service exists. SILENCE means it does not, and every
time-limited credential we have ever issued is effectively permanent.

Stdlib only — no pyserial on this machine.

Usage:  python3 mcu_time_probe.py [/dev/cu.usbserial-0001] [seconds]
"""
import os
import subprocess
import sys
import termios
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"
LISTEN_S = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
BAUD = 9600

HDR = b"\x55\xAA"
CMD_NAMES = {
    0x00: "HEARTBEAT",
    0x01: "PRODUCT_INFO",
    0x02: "WORKING_MODE",
    0x03: "NET_STATUS",
    0x06: "DP_COMMAND (module->MCU)",
    0x07: "DP_REPORT (MCU->module)",
    0x0C: "GET_GMT_TIME",
    0x1C: "GET_LOCAL_TIME",
    0x34: "TIME_NOTIFY",
}


def frame(cmd: int, payload: bytes = b"") -> bytes:
    body = HDR + bytes([0x00, cmd, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) + payload
    return body + bytes([sum(body) & 0xFF])


def parse(buf: bytes):
    """Yield (cmd, payload, raw) for every complete, checksum-valid frame."""
    out, i = [], 0
    while i + 7 <= len(buf):
        if buf[i : i + 2] != HDR:
            i += 1
            continue
        ln = (buf[i + 4] << 8) | buf[i + 5]
        end = i + 6 + ln + 1
        if end > len(buf):
            break
        raw = buf[i:end]
        if (sum(raw[:-1]) & 0xFF) == raw[-1]:
            out.append((raw[3], raw[6 : 6 + ln], raw))
            i = end
        else:
            i += 1
    return out, buf[i:]


def open_port(path: str) -> int:
    # `stty` does the line discipline; termios then clears the flags that would
    # otherwise mangle binary (ICANON/ECHO/translation).
    subprocess.run(
        ["stty", "-f", path, str(BAUD), "cs8", "-cstopb", "-parenb", "raw", "-echo"],
        check=True,
    )
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[3] = 0  # iflag, lflag
    a[2] = (a[2] | termios.CREAD | termios.CLOCAL) & ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def main() -> int:
    if not os.path.exists(PORT):
        print(f"NO PORT {PORT}")
        return 2
    fd = open_port(PORT)
    print(f"MCU EMULATOR on {PORT} @ {BAUD} 8N1 — listening {LISTEN_S:g}s\n")

    req = frame(0x1C)
    print(f"TX  0x1C GET_LOCAL_TIME  {req.hex(' ').upper()}")
    os.write(fd, req)

    deadline = time.time() + LISTEN_S
    buf, seen, time_replies = b"", [], 0
    resent = False
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 512)
            if chunk:
                buf += chunk
        except BlockingIOError:
            pass
        frames, buf = parse(buf)
        for cmd, payload, raw in frames:
            name = CMD_NAMES.get(cmd, f"0x{cmd:02X} (unknown)")
            print(f"RX  {name}  {raw.hex(' ').upper()}")
            seen.append(cmd)
            if cmd in (0x0C, 0x1C, 0x34) and payload:
                time_replies += 1
                if cmd in (0x0C, 0x1C) and len(payload) >= 7:
                    ok = payload[0]
                    if ok:
                        print(
                            f"    -> TIME SERVED: 20{payload[1]:02d}-{payload[2]:02d}-"
                            f"{payload[3]:02d} {payload[4]:02d}:{payload[5]:02d}:{payload[6]:02d}"
                        )
                    else:
                        print("    -> module answered but reports NO VALID TIME")
        # Ask once more halfway, in case the first landed while it was asleep.
        if not resent and time.time() > deadline - LISTEN_S / 2:
            resent = True
            print(f"TX  0x1C retry           {req.hex(' ').upper()}")
            os.write(fd, req)
        time.sleep(0.05)

    os.close(fd)
    print("\n" + "=" * 62)
    if time_replies:
        print("RESULT: time service ANSWERS. ozkey-21 §2.3 suspicion NOT confirmed.")
        return 0
    if seen:
        others = ", ".join(sorted({CMD_NAMES.get(c, hex(c)) for c in seen}))
        print("RESULT: 🔴 NO TIME REPLY. The UART is alive — we saw: " + others)
        print("        So the link works and the time service is simply absent.")
    else:
        print("RESULT: ⚠ SILENCE — no frames at all. Cannot distinguish 'no time")
        print("        service' from 'wiring/baud wrong'. Inconclusive.")
    print("        DP 21/23 windows are unenforceable if the MCU is never told")
    print("        the time. See docs/ozkey-21.md §2.3.")
    print("=" * 62)
    return 1


if __name__ == "__main__":
    sys.exit(main())
