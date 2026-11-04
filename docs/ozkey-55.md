# ozkey-55: dashboards end to end. Every field traced from the lock to the screen

Status: rev 3 2026-11-05 (OZKIE as the common exchange; any SoC; PM GO on F1/F2/F5), ozkey-92 (firmware, doorlock team lead). For PM nexus-e3 and server dev
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

3. *"The future doorlock can be any SoC, but to communicate with the bridge through BLE it must use
   the OZKIE protocol, like the Tuya protocol."*

So OZKIE plays the role the Tuya protocol plays for Tuya devices: **the chip is the lock maker's
choice; the protocol on the air is not.** There are two ways for a lock maker to get there (Tuya
offers both as well):
- **SoC integration:** port `ozcore` (portable C, one small `port/<soc>` layer for BLE, flash, AES
  and RNG) onto the lock's own BLE SoC. AK1037 is done; ESP32-C6 is F5.
- **Module integration (option, not scheduled):** a certified OZKIE BLE module does BLE and
  security, and the vendor's MCU talks to it over a documented UART command set (lock/unlock, state,
  credential and event callbacks). That fits vendors who won't touch BLE or crypto. It's for the
  operator to decide whether we offer it; it doesn't change the bridge.

Either way, the lock is accepted when it passes the OZKIE conformance kit (F6).

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
  available, built from its own sketch, for the v1 regression gate.
  - **v1 board kept (PM condition):** `dlch584m_147` (1.47") stays on v1, and the other C6 lock
    board gets v2. Both are identified by MAC on the day, not by name ([[bench-board-mac-map]];
    names have been reassigned before). There is **no third lock board**: the GEEKC6 boards are
    bridges, and no bridge is taken out of the ≥ 2-bridge failover set to make a lock (PM). OZKIT's
    `AC:EB:E6:0D:AF:BC` is never used.
  - Fallback if only one board works: both images in the board's two OTA app slots (default_8MB
    has ota_0/ota_1), switched with `otatool.py switch_ota_partition`, so v1 is testable again
    without a rebuild.
- **F6: OZKIE conformance kit.** Test vectors (CBOR, EDHOC, OSCORE, advertising bytes) and a
  conformance runner that drives a lock over BLE through the bridge: claim, invite, unlock, cred
  write, events, time. A vendor's lock that passes it works with our bridges, apps and dashboards.
  The first two to pass: the F5 ESP32-C6 bench locks and the AK1037 kit.
- **D1 (b) if approved:** a `health` resource in `oz_lock` and a bridge poll.

## 5a. The app's OZKIE v2 initiator: interface requirements (for ftpos, OZKIE-V2-APP)
The operator ruled 2026-11-05: **OZKIE v2 on every lock**. BANOI speaks v1 only today, so v2 end to
end with the real app needs this. Until then firmware tests v2 over BLE with `tools/ozapp.py`.

**Normative sources.** ozkey-48 §2 (GATT + frame), §3 (EDHOC), §3.3–3.4 (invite in EAD_3, keypad
pairing), §4 (OSCORE, contexts, freshness, epochs), §5 (verbs), §6 (roles) and **Appendix A** (wire
details the reference implementation fixed). Where the text is silent, `blelock/ozcore` behaviour
is the reference.

**What the app must implement.**
1. **BLE:**
   - scan for the v2 service UUID (`…4b32`), actively, to get the scan response;
   - read the busy byte and the 4-byte short id there;
   - connect; request MTU 247;
   - read `info`;
   - write frames to the OZKIE characteristic and receive notifications on it (ozkey-48 §2.1).
2. **EDHOC initiator:** RFC 9528, method 3 (STAT-STAT), cipher suite 0 (AES-CCM-16-64-128,
   SHA-256, MAC 8, X25519).
   - `C_I` is 1 byte.
   - Credentials are the 44-byte canonical CCS (App. A).
   - The lock's `ID_CRED_R = {4: h'00'}`.
   - A bonded app sends `ID_CRED_I = {4: h'<slot>'}` (compact); an unbonded one sends `{14: CCS}`.
   - Invites ride in EAD_3 as the CBOR array of §3.3.
   - message_4 is not used: the first OSCORE response is key confirmation.
3. **OSCORE client:** RFC 8613 over CoAP (RFC 7252).
   - Master Secret 16 B / Salt 8 B from the EDHOC exporter (RFC 9528 App. A.1).
   - Handle `4.01 + Echo` by resending with the Echo option (B.1.2).
4. **Pairing (owner):** after message_3 on an unclaimed lock, `POST pair`. The lock answers only
   after the owner types the 6-digit code on the keypad: `2.04 {0: BOND_OK, 1: 0}`, or
   `4.01 {0: PAIR_FAILED}`. Show the code-entry prompt meanwhile.
5. **Verbs:** lock/state, unlock/lock, cred, bond, events, info, time (§5). Every state-changing
   request carries the epoch; handle `STALE_EPOCH` (it returns the current epoch at key 1).
   - **PINs are ≥ 6 digits** (PM ruling). Refuse shorter ones in the UI.
   - Result codes are language-neutral integers (App. A table); the app translates them.
6. **Invites:** byte-identical OZINV1 (unchanged, ftpos's frozen vectors). For guest-bound invites,
   use OZINV2 / canonical v3 with `bind` (first 16 B of SHA-256 of the guest app's public key) and
   `iss` (§3.3). Refuse labels containing `|`.

**What the app must store (secure storage / keychain):**
- **Its own X25519 static key pair.** The private key never leaves the device. This is the app's
  identity on every lock.
- **Per lock:**
  - the device_id and the lock's public key / CCS (from `info`, or the QR later);
  - its bond slot and role;
  - the last epoch it saw.
- **Owner only, per lock:** the persistent **relay** OSCORE context (Master Secret/Salt,
  Sender/Recipient IDs, sender sequence persisted **in blocks** per RFC 8613 B.1.1, the replay
  window) **if** the app ever sends through the relay. Re-key with a fresh EDHOC at least every 30
  days or at seq 2^32. A member never gets one (members are on premises only, §4.1).
- **Never stored:** session contexts. They die with the BLE connection.

**Test vectors and an oracle:**
- RFC 9529 §3 (EDHOC method 3 trace) and RFC 8613 Appendix C.4–C.6 (OSCORE). Both are checked
  by `ozcore` tests and by `tools/ozapp.py rfc`.
- The invite MACs: ftpos's frozen `member_invite_test.dart` vectors, reproduced by `ozcore`
  (test_invite).
- **An interop oracle:** `tools/hostlock.c` builds a host lock (`ozhostlock`) on the Mac or Linux,
  and `python3 -I tools/ozapp.py interop <path>/ozhostlock` is an independent v2 app (written
  from the RFCs) that runs a scenario against it: claim with the keypad code, owner unlock, set the clock,
  member invite + door unlock, invite replay refused, PIN add + stale epoch refused, service bond,
  owner relay context with Echo after a reboot, and service-cannot-unlock. **`events` Observe isn't
  in the scenario yet**; `ozcore`'s test_events covers it on the lock side. The Dart initiator should pass the same scenario against `ozhostlock` before any real
  lock, then against the F5 C6 lock over BLE.

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
