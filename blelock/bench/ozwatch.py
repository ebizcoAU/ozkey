#!/usr/bin/env python3
"""
ONE view of the whole system. Serial from every board + MQTT, interleaved,
timestamped, filtered to what matters.

    python3 ozwatch.py                 # auto-detect boards, broker 10.1.1.20
    python3 ozwatch.py --all           # no filtering, full firehose
    python3 ozwatch.py --broker 10.1.1.20

WHY: every failure this system has had was visible in ONE log and invisible in
the others. The uplink that vanished was fine on the lock and absent on the
bridge. The liveness report looked perfect while the server matched nothing.
Reading them separately is how a broken chain looks healthy at every layer.

Sources are tagged and aligned:

    LOCK-A   the 1.9" doorlock          (native USB CDC)
    LOCK-B   the 1.47" doorlock         (native USB CDC)
    BRIDGE   the border router          (native USB CDC)
    MQTT     the broker — what the SERVER actually sees

Serial reading is lifted from duallog.py, including its re-enumeration
handling: every board reset re-creates the device node, and an fd held across
that goes silently dead.
"""
import os
import re
import select
import subprocess
import sys
import time

BROKER = "10.1.1.20"
SHOW_ALL = "--all" in sys.argv
if "--broker" in sys.argv:
    BROKER = sys.argv[sys.argv.index("--broker") + 1]

# Boards are identified by what they SAY, not by port number — ports move on
# every cable reseat and have already caused one lost capture tonight.
ROLE_HINTS = [
    (re.compile(r"OZBRIDGE|LIVENESS|BEACON\].*->|MQTT\] connected"), "BRIDGE"),
]

# Signal, not noise. Everything here is something that has actually mattered.
KEEP = re.compile(
    r"\[UPLINK\]|\[BEACON\]|\[LIVENESS\]|\[ROSTER\]|\[REVOKE\]|\[MEMBER\]"
    r"|\[STATUS\]|\[RESET\]|\[BLE\] window|\[TUYA|\[FWD\]|\[THREAD\]"
    r"|\[CRYPTO\] selftest (?!.*PASS)|panic|Guru|rst:0x|FAILED|DROPPED"
    r"|presence|liveness|heartbeat|uplink|command"
)


def detect_ports():
    out = []
    for p in sorted(os.listdir("/dev")):
        if p.startswith("cu.usbmodem"):
            out.append("/dev/" + p)
    return out


def try_open(p):
    try:
        subprocess.run(["stty", "-f", p, "115200", "raw", "-echo", "-hupcl"],
                       check=True, capture_output=True)
        return os.open(p, os.O_RDONLY | os.O_NONBLOCK)
    except (subprocess.CalledProcessError, OSError):
        return None


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"


C = {"BRIDGE": "\033[36m", "LOCK-A": "\033[32m", "LOCK-B": "\033[33m",
     "MQTT": "\033[35m", "?": "\033[37m"}


def colour(src):
    if src.startswith("BRIDGE"):
        return C["BRIDGE"]
    if src.startswith("LOCK-A") or src.endswith("2201"):
        return C["LOCK-A"]
    if src.startswith("LOCK"):
        return C["LOCK-B"]
    return C.get(src, C["?"])
R = "\033[0m"


def emit(src, line):
    if not SHOW_ALL and not KEEP.search(line):
        return
    print(f"{stamp()}  {colour(src)}{src:<9}{R} {line}", flush=True)


def main():
    ports = detect_ports()
    if not ports:
        print("No boards found on /dev/cu.usbmodem*")
    fds, paths, bufs, names = [], {}, {}, {}
    for p in ports:
        fd = try_open(p)
        if fd is None:
            continue
        fds.append(fd); paths[fd] = p; bufs[fd] = b""
        # Label by PORT immediately. A board that has not yet said anything
        # identifying still needs a name you can act on — "BOARD" told you it
        # was a lock and not which one, which is no better than nothing.
        # Upgraded to LOCK-A/LOCK-B/BRIDGE as soon as the traffic reveals it.
        names[fd] = "P" + os.path.basename(p).replace("cu.usbmodem", "")[-4:]

    mq = subprocess.Popen(
        ["mosquitto_sub", "-h", BROKER, "-v", "-t", "ozkie/#"],
        stdout=subprocess.PIPE, text=True, bufsize=1,
    )

    print(f"ozwatch — {len(fds)} board(s) + MQTT {BROKER}"
          f"{'' if SHOW_ALL else '   (filtered; --all for everything)'}\n")

    try:
        while True:
            ready, _, _ = select.select(fds + [mq.stdout], [], [], 0.2)
            for fd in ready:
                if fd is mq.stdout:
                    line = mq.stdout.readline()
                    if line:
                        topic, _, payload = line.strip().partition(" ")
                        short = topic.replace(f"ozkie/lab/", "")
                        # Sealed envelopes are ~700 chars of hex nobody reads.
                        if len(payload) > 160:
                            payload = payload[:157] + "..."
                        emit("MQTT", f"{short}  {payload}")
                    continue
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    chunk = b""
                if not chunk:
                    # Re-enumeration: the board reset. Reopen, do not spin.
                    p = paths[fd]
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    fds.remove(fd)
                    time.sleep(0.5)
                    nfd = try_open(p)
                    if nfd is not None:
                        fds.append(nfd); paths[nfd] = p; bufs[nfd] = b""; names[nfd] = names.get(fd, "?")
                    continue
                bufs[fd] += chunk
                while b"\n" in bufs[fd]:
                    raw, bufs[fd] = bufs[fd].split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip()
                    if not line:
                        continue
                    if not names[fd].startswith(("LOCK", "BRIDGE")):
                        for rx, role in ROLE_HINTS:
                            if rx.search(line):
                                names[fd] = role
                                break
                        else:
                            # A lock: name it by which one once it says so.
                            m = re.search(r"ozk-([0-9a-f]{12})", line)
                            if m:
                                names[fd] = "LOCK-A" if m.group(1).startswith("aceb") else "LOCK-B"

                    emit(names[fd], line)
    except KeyboardInterrupt:
        pass
    finally:
        mq.terminate()


if __name__ == "__main__":
    main()
