#!/usr/bin/env python3
"""Timestamp mosquitto_sub output at local-receipt time, same HH:MM:SS.mmm
format as duallog.py, so MQTT and serial lines can be correlated by eye for
a latency measurement (2026-08-07, Thread-relay-latency investigation)."""
import subprocess, sys, time

BROKER = sys.argv[1] if len(sys.argv) > 1 else "10.1.1.20"
TOPIC = sys.argv[2] if len(sys.argv) > 2 else "ozkey/lab/#"

proc = subprocess.Popen(
    ["mosquitto_sub", "-h", BROKER, "-t", TOPIC, "-v"],
    stdout=subprocess.PIPE, text=True, bufsize=1,
)
for line in proc.stdout:
    t = time.time()
    stamp = time.strftime('%H:%M:%S', time.localtime(t)) + f'.{int(t % 1 * 1000):03d}'
    print(f'{stamp}  {line.rstrip()}', flush=True)
