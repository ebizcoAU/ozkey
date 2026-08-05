#!/usr/bin/env bash
# M4 DP-dispatch-split bench test (doorlock-1.8).
# Expect on the LOCK serial: REJECTED for 101/102/99, [TUYA->] only for DP 1.
# Expect on LOCKSIM: exactly ONE frame — the DP 1 unlock.

# DP 101 bond_revoke  (43 B)
mosquitto_pub -h 10.1.1.20 -t 'ozkey/lab/locks/ozk-b0a6048b5fd8/command' -m '{"payload_hex":"55 AA 00 06 00 24 65 00 00 20 A0 A1 A2 A3 A4 A5 A6 A7 A8 A9 AA AB AC AD AE AF B0 B1 B2 B3 B4 B5 B6 B7 B8 B9 BA BB BC BD BE BF 9E"}'

# DP 102 invite_cancel  (27 B)
mosquitto_pub -h 10.1.1.20 -t 'ozkey/lab/locks/ozk-b0a6048b5fd8/command' -m '{"payload_hex":"55 AA 00 06 00 14 66 00 00 10 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 07"}'

# DP 99 unknown  (12 B)
mosquitto_pub -h 10.1.1.20 -t 'ozkey/lab/locks/ozk-b0a6048b5fd8/command' -m '{"payload_hex":"55 AA 00 06 00 05 63 00 00 01 01 6F"}'

# DP 1 unlock  (CONTROL — must pass)  (12 B)
mosquitto_pub -h 10.1.1.20 -t 'ozkey/lab/locks/ozk-b0a6048b5fd8/command' -m '{"payload_hex":"55 AA 00 06 00 05 01 01 00 01 01 0E"}'
