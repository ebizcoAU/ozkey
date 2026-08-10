#!/usr/bin/env python3
"""
ozkey-20 acceptance test — proves the Thread liveness chain end to end.

WHY THIS EXISTS
---------------
Every failure this chain has had looked fine from one layer. The liveness
sweep published happily for hours while the server matched nothing; the lock
sent uplinks that were never relayed; the bridge reported 2 children it could
not name. Watching any single log said "working".

So this asserts INVARIANTS ACROSS LAYERS, from MQTT only — the same view the
server has. Every check below corresponds to a bug that actually shipped
tonight; none of them are hypothetical.

    python3 ozkey20_verify.py [broker] [seconds]

Exit 0 = all pass. Non-zero = the count of failures.

Needs mosquitto_sub (same dependency as mqttlog.py). No pip installs.
"""
import json
import subprocess
import sys
import time
from collections import defaultdict

BROKER = sys.argv[1] if len(sys.argv) > 1 else "10.1.1.20"
WINDOW = int(sys.argv[2]) if len(sys.argv) > 2 else 90
SITE = "lab"

# A beacon is due every heartbeat interval (60 s default), a liveness sweep
# every 30 s. 90 s is the smallest window that must contain at least one of
# each, with margin for a sweep landing just before we start.
LIVENESS_T = f"ozkie/{SITE}/bridges/+/liveness"
PRESENCE_T = f"ozkie/{SITE}/bridges/+/presence"
BEACON_T = f"ozkie/{SITE}/locks/+/heartbeat"

liveness, presence, beacons = [], [], []


def collect():
    proc = subprocess.Popen(
        ["mosquitto_sub", "-h", BROKER, "-v",
         "-t", LIVENESS_T, "-t", PRESENCE_T, "-t", BEACON_T],
        stdout=subprocess.PIPE, text=True, bufsize=1,
    )
    deadline = time.time() + WINDOW
    try:
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            topic, _, payload = line.strip().partition(" ")
            try:
                obj = json.loads(payload)
            except json.JSONDecodeError:
                continue
            if topic.endswith("/liveness"):
                liveness.append(obj)
                print(f"  liveness  role={obj.get('role')} "
                      f"auth={obj.get('authoritative')} "
                      f"children={obj.get('children')} "
                      f"named={sum(1 for l in obj.get('locks', []) if l.get('id'))}")
            elif topic.endswith("/presence"):
                presence.append(obj)
                print(f"  presence  {obj.get('state')}")
            elif topic.endswith("/heartbeat"):
                beacons.append(obj)
                print(f"  beacon    {obj.get('from')} epoch={obj.get('roster_epoch')} "
                      f"mcu={obj.get('mcu_link_up')}")
    finally:
        proc.terminate()


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ""))


def main():
    print(f"Listening {WINDOW}s on {BROKER}\n")
    collect()
    print()

    # ── 1. The bridge is publishing at all ─────────────────────────────────
    check("bridge publishes liveness", len(liveness) > 0,
          f"{len(liveness)} report(s)")
    if not liveness:
        print("\nNothing received — is the bridge powered and on the broker?")
        return 1

    # ── 2. Authoritative. A Child bridge sees no child table, and reporting
    #      that as fact marked every lock unreachable (ozkey-20 §15.3).
    bad = [r for r in liveness if not r.get("authoritative")]
    check("every report is authoritative", not bad,
          f"role={liveness[-1].get('role')}")

    # ── 3. THE BUG THAT LASTED LONGEST: locks reported but not NAMED.
    #      Without `id` the server matched nothing and inferred 3 locks lost
    #      from a healthy mesh, every 30 s.
    unnamed = defaultdict(int)
    total = 0
    for r in liveness:
        for l in r.get("locks", []):
            total += 1
            if not l.get("id"):
                unnamed[l.get("ext", "?")] += 1
    check("every reported lock is identified", not unnamed,
          f"{total - sum(unnamed.values())}/{total} named"
          + (f" — unnamed ext: {list(unnamed)}" if unnamed else ""))

    # ── 4. Thread locks emit presence at all. They never did until R3 —
    #      publishHeartbeat() is gated on mqtt.connected(), which a Thread
    #      lock never is (ozkey-20 §2.1).
    ids = {b.get("from") for b in beacons if b.get("from")}
    check("Thread locks beacon", len(ids) > 0, f"{len(ids)} lock(s): {sorted(ids)}")

    # ── 5. CROSS-LAYER: everything the bridge names must also be beaconing
    #      for itself. Catches a stale NVS join map naming a lock that is
    #      gone, which would look perfect in the liveness report alone.
    named = {l["id"] for r in liveness for l in r.get("locks", []) if l.get("id")}
    if named and ids:
        check("named locks match beaconing locks", named == ids,
              f"liveness={sorted(named)} beacons={sorted(ids)}")
    else:
        check("named locks match beaconing locks", False,
              "no overlap to compare")

    # ── 6. roster_epoch actually present — ftpos's reconciliation reads this
    #      field and silently does nothing without it (XF-89 §8.1).
    noepoch = [b.get("from") for b in beacons if b.get("roster_epoch") is None]
    check("beacons carry roster_epoch", not noepoch,
          f"e.g. {beacons[-1].get('roster_epoch')}" if beacons else "")

    # ── 7. Liveness is FRESH, not a stuck retained value. age_s must move.
    if len(liveness) >= 2:
        ages = [l.get("age_s") for r in liveness for l in r.get("locks", [])]
        check("age_s is live (varies across sweeps)", len(set(ages)) > 1,
              f"observed {sorted(set(ages))[:6]}")
    else:
        check("age_s is live (varies across sweeps)", False,
              "need >=2 sweeps; run a longer window")

    # ── 8. mcu_link_up present — ozkey-20 §5a, the hop that decides whether
    #      the door can open at all.
    nomcu = [b.get("from") for b in beacons if "mcu_link_up" not in b]
    check("beacons report MCU link state", not nomcu,
          f"{ {b['from']: b.get('mcu_link_up') for b in beacons if b.get('from')} }")

    fails = sum(1 for _, ok, _ in results if not ok)
    print(f"\n{len(results) - fails}/{len(results)} passed")
    if fails:
        print("\nFAILED:")
        for n, ok, d in results:
            if not ok:
                print(f"  - {n}  {d}")
    else:
        print("""
All automated checks pass. Two MANUAL tests remain — they prove the system
reports FAILURE correctly, which is the half no passive observation covers:

  A. Power off one lock. Within ~2 sweeps it should DISAPPEAR from locks[]
     (absence is the lost signal — `state:"lost"` never arrives by design).
     Power it back on; it must return NAMED, not as bare ext.

  B. Pull the bridge's power. The broker publishes its retained LWT and the
     server must aggregate ONE bridge_offline across every lock behind it,
     not one alert per lock.
""")
    return fails


if __name__ == "__main__":
    sys.exit(main())
