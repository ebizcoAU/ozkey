#!/usr/bin/env python3
"""
Live view of the ozkey-20 chain, for running the two manual failure tests.

    python3 ozkey20_watch.py [broker]

Prints one line per event, plus a state table whenever anything changes, so
you can watch a lock leave and come back without reading JSON.

WHAT TO EXPECT
--------------
TEST A — power off a lock:
    that lock stops beaconing, then within ~2 sweeps (60 s) DISAPPEARS from
    the bridge's report. Absence IS the signal: the bridge only reports what
    is in its child table, and `state:"lost"` is never sent — deciding a lock
    is lost needs to know which locks should exist, which only the server
    knows. On power-up it must come back NAMED, not as a bare ext — that is
    what proves the identity join survived (it is persisted in the bridge's
    NVS, so it should survive a bridge reboot too).

TEST B — pull the bridge's power:
    PRESENCE flips to offline within about a second. That is the broker
    publishing the bridge's retained Last Will, not a timeout — no polling
    involved. The server must then aggregate ONE bridge_offline across every
    lock behind it, which you check in the server log, not here.
"""
import json
import subprocess
import sys
import time

BROKER = sys.argv[1] if len(sys.argv) > 1 else "10.1.1.20"
SITE = "lab"

state = {"presence": "?", "locks": {}, "beacons": {}}
last_render = ""


def render():
    global last_render
    rows = []
    ids = sorted(set(state["locks"]) | set(state["beacons"]))
    for i in ids:
        lv = state["locks"].get(i)
        bc = state["beacons"].get(i)
        seen = "IN REPORT" if lv else "-- ABSENT --"
        age = f"age={lv['age_s']}s" if lv else ""
        rssi = f"rssi={lv['rssi']}" if lv else ""
        named = "" if (lv and lv.get("named")) else ("  <-- UNNAMED (ext only)" if lv else "")
        ep = f"epoch={bc['epoch']}" if bc else "no beacon"
        mcu = ("mcu=UP" if bc["mcu"] else "mcu=DOWN") if bc else ""
        age_b = f"{int(time.time() - bc['t'])}s ago" if bc else ""
        rows.append(f"  {i}  {seen:<12} {age:<10} {rssi:<10} | {ep:<10} {mcu:<9} {age_b}{named}")
    out = (f"\n  BRIDGE PRESENCE: {state['presence'].upper()}\n" + "\n".join(rows) + "\n")
    if out != last_render:
        print(out, flush=True)
        last_render = out


def main():
    print(f"Watching {BROKER}  (Ctrl-C to stop)\n")
    proc = subprocess.Popen(
        ["mosquitto_sub", "-h", BROKER, "-v",
         "-t", f"ozkie/{SITE}/bridges/+/liveness",
         "-t", f"ozkie/{SITE}/bridges/+/presence",
         "-t", f"ozkie/{SITE}/locks/+/heartbeat"],
        stdout=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        for line in proc.stdout:
            topic, _, payload = line.strip().partition(" ")
            ts = time.strftime("%H:%M:%S")
            try:
                obj = json.loads(payload)
            except json.JSONDecodeError:
                continue

            if topic.endswith("/presence"):
                st = obj.get("state", "?")
                was = state["presence"]
                state["presence"] = st
                if st != was:
                    flag = "  <<< LWT — bridge died" if st == "offline" else ""
                    print(f"{ts}  PRESENCE  {st.upper()}{flag}")
                render()

            elif topic.endswith("/liveness"):
                if not obj.get("authoritative"):
                    print(f"{ts}  LIVENESS  role={obj.get('role')} NOT AUTHORITATIVE "
                          f"— ignore, tells us nothing about any lock")
                    continue
                seen = {}
                for l in obj.get("locks", []):
                    key = l.get("id") or f"ext:{l.get('ext')}"
                    seen[key] = {"age_s": l.get("age_s"), "rssi": l.get("rssi"),
                                 "named": bool(l.get("id"))}
                gone = set(state["locks"]) - set(seen)
                new = set(seen) - set(state["locks"])
                for g in gone:
                    print(f"{ts}  LOCK GONE from report: {g}   <<< this is the 'lost' signal")
                for n in new:
                    tag = "" if seen[n]["named"] else "   <<< UNNAMED"
                    print(f"{ts}  LOCK BACK in report: {n}{tag}")
                state["locks"] = seen
                render()

            elif topic.endswith("/heartbeat"):
                who = obj.get("from")
                if not who:
                    continue
                state["beacons"][who] = {"epoch": obj.get("roster_epoch"),
                                         "mcu": obj.get("mcu_link_up"), "t": time.time()}
                print(f"{ts}  BEACON    {who} epoch={obj.get('roster_epoch')} "
                      f"mcu={'up' if obj.get('mcu_link_up') else 'DOWN'}")
                render()
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()


if __name__ == "__main__":
    main()
