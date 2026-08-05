#!/usr/bin/env python3
"""Merge two serial ports into one millisecond-timestamped stream.

Reads the device files directly (no pyserial). Only touches the two native-USB
CDC ports — deliberately NOT the CP2102, whose DTR/RTS lines would reset the
board attached to it on open.
"""
import os, select, sys, time, subprocess

PORTS = sys.argv[1:]
if not PORTS:
    sys.exit("usage: duallog.py /dev/cu.x /dev/cu.y")

def label(p):
    return os.path.basename(p).replace('cu.usbmodem', 'port')

def try_open(p):
    """Open + configure a port. Returns fd, or None if the device isn't there
    right now (unplugged / mid-re-enumeration)."""
    try:
        subprocess.run(['stty', '-f', p, '115200', 'raw', '-echo', '-hupcl'],
                        check=True, capture_output=True)
        return os.open(p, os.O_RDONLY | os.O_NONBLOCK)
    except (subprocess.CalledProcessError, OSError):
        return None

paths, fds, bufs = {}, [], {}
for p in PORTS:
    fd = try_open(p)
    if fd is None:
        sys.exit(f"cannot open {p} at startup — check the device is connected")
    paths[fd] = p
    fds.append(fd)
    bufs[fd] = b''

sys.stderr.write(f"listening: {', '.join(label(paths[fd]) for fd in fds)}\n")
sys.stderr.flush()

# BUG (found 2026-08-05, cost most of a bench session): when the underlying
# USB-CDC device re-enumerates — which every board reset does — the old fd can
# keep reporting "ready" from select() while os.read() returns b'' (EOF)
# forever, with no exception. The old code treated that as "no data yet" and
# looped straight back to select() with no wait, pinning a core at ~100% CPU
# while producing a permanently empty log — indistinguishable from a dead
# board unless you happen to check `ps`. Fix: EOF means the fd is dead. Close
# it and back off into periodic reopen attempts on the same path until the
# device comes back (which also covers "was never plugged in yet").
RETRY_S = 1.0

while True:
    if not fds:
        time.sleep(RETRY_S)
        # nothing open; try every known path
        for p in PORTS:
            fd = try_open(p)
            if fd is not None:
                paths[fd] = p
                fds.append(fd)
                bufs[fd] = b''
                sys.stderr.write(f"reconnected: {label(p)}\n")
                sys.stderr.flush()
        continue

    r, _, _ = select.select(fds, [], [], 1.0)
    for fd in r:
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        except OSError:
            data = b''  # treat like EOF — fall through to the dead-fd path
        if not data:
            p = paths.pop(fd, '?')
            sys.stderr.write(f"port gone (EOF): {label(p)} — will retry every {RETRY_S:.0f}s\n")
            sys.stderr.flush()
            try:
                os.close(fd)
            except OSError:
                pass
            fds.remove(fd)
            bufs.pop(fd, None)
            continue
        bufs[fd] += data
        while b'\n' in bufs[fd]:
            line, bufs[fd] = bufs[fd].split(b'\n', 1)
            text = line.decode('utf-8', 'replace').rstrip('\r')
            if not text.strip():
                continue
            t = time.time()
            stamp = time.strftime('%H:%M:%S', time.localtime(t)) + f'.{int(t % 1 * 1000):03d}'
            print(f'{stamp}  {label(paths[fd]):>10}  {text}', flush=True)
