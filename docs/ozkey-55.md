# ozkey-55: dashboards end to end. Every field traced from the lock to the screen

Status: DRAFT rev 2 2026-11-05 (OZKIE as the common exchange), ozkey-92 (firmware, doorlock team lead). For PM nexus-e3 and server dev
ozkey-f1. Companion to the dashboards brief (`nexus-lab-backups/ozkey_notes/2026-11-05_dashboards-commercial-brief.md`).

Operator directive (2026-11-05): as team lead, firmware makes sure the dashboards work with our bridges,
along the real chain:

```
DL (lock) --BLE--> BRIDGE --MQTT--> SERVER (ozlock | ozpms | ozlodge) --> NEXUS (registry, keys) --> APP
                                    SERVER <--MQTT--> APP          (depending on the architecture)
```

🔴 **Operator rules (2026-11-05):**
1. *"Regardless which doorlock, as long as the doorlock supports BLE, it should work with the bridge."*
2. *"There must be a common BLE data exchange, i.e. OZKIE, between DL, bridge and app."*

Together they mean: **OZKIE is the one common BLE exchange, and any lock that speaks it works.**
- **One protocol on the air, whoever made the lock.** Lock ⇄ bridge and lock ⇄ app both use OZKIE:
  the advertising format, the GATT service and frames (ozkey-48 §2), EDHOC sessions, OSCORE messages,
  and the shared vocabulary (lock/state, cred, events, info, time).
- **The bridge is lock-agnostic because the exchange is common.** It relays OZKIE frames MQTT ⇄ GATT
  without opening them, and has **no per-vendor code and no per-model profiles**. A new lock brand
  needs no bridge firmware change.
- **A vendor's lock becomes OZKIE-compatible by implementing OZKIE.** The reference is `ozcore`:
  portable C, with a small port layer per chip. AK1037 is the first port; the ESP32-C6 bench locks (F5) are
  the second, which show ozcore isn't tied to one chip. Add test vectors and a conformance suite
  (F6) so anyone else can prove it.
- **Model differences are capabilities, not protocol.** Fingerprint, RFID, keypad and auto-lock come
  from `info` and Nexus `device_models` (ozkey-26). They change what the app and dashboard offer,
  never how the bridge relays.
- **Health for every lock is part of OZKIE itself.** Presence, last heard and RSSI come from the
  bridge's view of the standard advertisement. Battery, firmware and clock come from D1, which (if
  approved) is a standard OZKIE resource, so every vendor reports them the same way.
- **v1 (ESP32 era) is legacy.** The bridge keeps relaying it until those locks retire, but no new
  lock targets v1.

**This is already proven on ESP32-C6 (operator, 2026-11-05: "we have done this with ESP32C6").** The
bench locks (`dlch584m_19`, `dlch584m_147`: ESP32-C6, BLE-only) speak **OZKIE v1** over BLE
(`blelock/CONTRACT.md`) to `bridgeBLE` and to the BANOI app. Owner pairing, member invites, the sealed
relay unlock, the digital passport (08-31) and multi-bridge failover with handback (09-02) all ran
live on that chain. AK1037 is **the same pattern with OZKIE v2**. Nothing in the chain's shape changes:
the lock speaks OZKIE v2, the bridge relays it, the app and server speak it end to end.

A dashboard field is only real if one link in this chain produces it. Every field below has a
producer and a status. "Sample data" in the mockup is replaced only by a row marked ✅.

## 1. The two paths, by product
- **OZLOCK** (hosted relay): app ⇄ MQTT ⇄ ozlockserv ⇄ bridge ⇄ lock. Nexus holds the models and
  public keys. The relay **cannot read** sealed (OSCORE) traffic and keeps no door log. Anything it
  shows about a lock has to come from the bridge's own plaintext observations or the registry.
- **OZPMS / OZLODGE** (self-hosted): the server holds a **`service` bond** on each lock (ozkey-48
  §4.1, §5.1). Through the bridge relay it can read `lock/state` (bolt, battery level key 4, last
  move) and Observe `events`. So these products can show lock health and door events; OZLOCK can't.

## 2. Where we are today (checked in code, 2026-11-05)
- Bridge firmware is **`bridgeBLE-0.44`** (`4186b50`, 2026-09-04). It's built for **v1** locks: ESP32
  era, Thread child table and BLE central. **It has no OZKIE v2 relay.** No AK1037 lock can reach any
  dashboard through it yet.
- What 0.44 publishes over MQTT under `ozkie/<site>/…`:
  - `bridges/<id>/liveness`: role, authoritative, children, dup_suppressed, and per lock
    `{id, rssi, lqi, age_s, rx_on, child, ext, state}` (Thread only)
  - `bridges/<id>/peer_status`: failover peer down/up
  - the bridge's own LWT presence: `{state, role}`
  - `locks/<id>/presence` (retained online/offline + reason), `locks/<id>/heartbeat` (the v1 lock's
    beacon), `locks/<id>/uplink` (sealed bytes)
- Missing over MQTT: the bridge's **firmware version** (only on its BLE `info` characteristic), its
  **clock**, and its uptime.
- A v2 lock **advertises only** its service UUID, a busy byte and a 4-byte short id (`oz_adv.h`).
  There's **no battery or firmware in plaintext**, by design.

## 3. Field map: OZLOCK screens (first to build)
Legend: ✅ produced today · 🟡 produced for v1 only · ❌ missing · 🔒 design gap (no producer allowed yet)

| Screen / field | Producer | Status | Work |
|---|---|---|---|
| Locks: device id, last heard, reachable | bridge presence + liveness `age_s`, server reachability (XF-116/117) | ✅ v1 / ❌ v2 | F1 |
| Locks: protocol v1/v2 | bridge, from the advertised UUID → `ozkie_version` in presence (ozkey-48 §7) | ❌ | F1 |
| Locks: model | Nexus registry | ✅ | — |
| Locks: best bridge + backup | server, from liveness + failover (09-02) | 🟡 Thread only | F1 (BLE-central equivalent) |
| Locks: signal | bridge: Thread child RSSI (v1); **BLE RSSI of the lock's adv/link (v2)** | 🟡 | F1 |
| Locks: battery | v1 heartbeat. **v2: sealed in `lock/state`, the relay can't read it** | 🔒 v2 | D1 |
| Locks: firmware | v1 heartbeat. **v2: none in plaintext** | 🔒 v2 | D1 |
| Bridges: state, last seen, locks served, weakest signal | LWT + liveness | ✅ | — |
| Bridges: failover partner | peer_status + server | ✅ | — |
| Bridges: firmware, clock | bridge | ❌ (BLE `info` only) | F2 |
| Bridges: owner account | server / Nexus | ✅ | — |
| Pending writes | server queue. Reason "bridge offline / out of range" from presence | ✅ server | server |
| Device trust: revocation list delivered to bridges | bridge applies the list (ozkey-52) | ❌ | F3 |
| Device trust: refused devices | server (key ≠ Nexus directory) | server | server |
| Firmware: "v2 locks need bridge ≥ X", rollout rings | bridge/lock OTA (none exists) | ❌ | F4, later |
| Health: throughput, errors | server counters. "Lock not reachable" and "bridge timeouts" from presence | server | server |
| Health: "dropped as replays" | the lock's OSCORE layer. The relay sees only the 4.01 (+Echo) response code, which is plaintext CoAP outer code | server can count it | server |
| Map: lock position | installer's app at pairing → server (coarse ~1 km unless the owner opts in) | ❌ app + server | ftpos + server |

OZPMS/OZLODGE extra: battery, bolt state, last move and door events come over the **service bond**.
That needs the v2 relay (F1) and the server's service-bond code (ozkey-48 §7 server items, S3).

## 4. Decision needed (architecture → operator sign-off via PM)
**D1: lock health for v2 locks on OZLOCK.** The relay can't read sealed state, so the battery and
firmware columns have no producer for v2. Options:
- **(a) Plaintext in the scan response:** battery % (1 B) and firmware build (2 B) in the 8 spare
  bytes. Cheap, and works without any bond. Cost: anyone nearby learns the battery level and firmware
  build. The build can tell an attacker a lock is on a known-vulnerable version.
- **(b) The `bridge` bond can read health:** extend the bridge role from "time only" to "time + a
  read-only `health` resource" (battery, firmware build, uptime; never bolt state or events). The
  bridge reads it over its own OSCORE context and publishes it plaintext on MQTT. Nobody nearby
  learns anything; only our bridge and server see it. Cost: a small lock + bridge change, and the
  bridge becomes trusted for health readings (not for access).
- **(c) The owner's app reports it** whenever it connects. No lock change, but stale for weeks.
- **Recommendation: (b).** It keeps the broadcast minimal, is reported on the bridge's schedule, and
  can't open a door. ozkey-48 §6's bridge row changes from "time set only" to "time set + health
  read".

## 5. Firmware work list (mine; needs PM GO)
- **F1: bridgeBLE OZKIE relay for any OZKIE lock** (operator rules above).
  - GATT central to any device advertising the OZKIE v2 service; relay raw OZKIE frames MQTT ⇄ GATT
    unopened.
  - No vendor or model branches in the bridge. v1 relay kept as legacy, and its behaviour must not
    change (regression gate on today's bench).
  - `ozkie_version` 1|2 in presence (ozkey-48 §7).
  - Per-lock BLE RSSI and last-heard in liveness, in the same schema as the Thread entries, so the
    server and dashboards don't branch on transport.
  - Best/backup bridge for BLE locks (extend the 09-02 failover).
  - The bridge's own `bridge` bond (time set).
  - MQTT buffer 1024 → 2048, tested with a maximum-size message (ozkey-48 §7: the silent-drop cliff).
- **F2: bridge status over MQTT.** fw, clock (utc + drift), uptime, free heap, in the retained LWT
  presence payload. No new topic.
- **F3: revocation list on the bridge** (ozkey-52): receive, store, refuse listed devices, report
  the list version (Device trust "Delivered to bridges").
- **F4 (later): signed OTA** for bridges and locks, with rings (Firmware screen). Not before the kit.
- **F5: OZKIE v2 on the existing ESP32-C6 bench locks.** Add `ozcore/port/esp32c6` and run the same
  `ozcore` engine as AK1037 on the `dlch584m_*` boards, advertising the v2 service. Then bench lock
  → bridge → server → dashboard runs live on v2 before the kit (ETA ~11-12). v1 firmware stays
  available on the same boards, built from its own sketch, for the v1 regression gate.
- **F6: OZKIE conformance kit.** Test vectors (CBOR, EDHOC, OSCORE, advertising bytes) and a
  conformance runner that drives a lock over BLE through the bridge: claim, invite, unlock, cred
  write, events, time. A vendor's lock that passes it works with our bridges, apps and dashboards.
  The first two to pass: the F5 ESP32-C6 bench locks and the AK1037 kit.
- **D1 (b) if approved:** a `health` resource in `oz_lock` and a bridge poll.

## 6. Acceptance: what "works with our bridges" means
Each OZLOCK screen group is accepted only when it shows **live values from the bench**, not sample
data. The rig is F5 or the AK1037 kit, ≥ 2 bridges, the lab server:
1. Pair a v2 lock through bridge A. It appears on Locks within 60 s with protocol v2, best bridge A,
   and signal.
2. Power off bridge A. Within the failover window the lock shows bridge B as best, and Bridges
   shows A offline with its partner B.
3. A sealed PIN write while the lock is out of range shows on Pending writes and clears when the
   lock returns. An unlock is refused immediately, never queued.
4. Bridge firmware and clock show on Bridges (F2). The lock's battery and firmware show on Locks
   (D1).
5. I run each check on the bench, and the result goes in this doc before the PM merges the screen.
