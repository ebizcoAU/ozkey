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
- A v2 lock **advertises only** its service UUID, a busy byte and a 4-byte short id, SHA-256(device_id)[0..3] (`oz_adv.h`, ozkey-48 §2.2).
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
  - **v1 board kept (PM condition):** there are **three** C6 lock boards ([[bench-board-mac-map]],
    2026-09-04): LockA `AC:EB:E6:39:F8:C4` (1.9"), LockB `AC:EB:E6:3A:CA:B8` (1.9") and LockC
    `B0:A6:04:8B:5F:D8` (1.47", `dlch584m_147`). **LockC stays on v1.** v2 goes on one 1.9" board,
    LockB by default (LockA has the 08-31 POWERON fault). The other 1.9" stays v1 as a second
    regression lock, or becomes a second v2 lock if a test needs two. Boards are always picked by
    MAC, never by name (names have been reassigned). Bridges (`98:A3:16:A7:E6:38`,
    `20:6E:F1:16:F3:90`) stay bridges: none leaves the ≥ 2-bridge failover set (PM). OZKIT's
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
   - read the busy byte and the 4-byte short id there. The short id is SHA-256(device_id)[0..3],
     not hex from the id (ozkey-48 §2.2, test vectors there);
   - connect; request MTU 247;
   - read `info`, check that key 1 equals the full device_id, and disconnect on a mismatch
     (ozkey-48 §2.2: the short id is only a filter);
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

## 5b. F1 relay wire: three gaps found before coding (DRAFT 2026-11-05, needs PM sign-off)
F1 code hasn't started. Reading ozkey-48 §2/§7, `oz_lock.h`, the C6 port and the v1 bridge showed
three things the spec doesn't settle. Each one is a wire contract shared with the server or the lock,
so I'm not choosing them alone.

**G1. The lock can't tell the bridge's link from an app's.** `oz_lock` has two link kinds: a
session link (`relay = 0`, contexts gone on disconnect) and the relay link (`relay = 1`, contexts
persisted per bond, Echo after a reboot). `ozhostlock` gets this from two TCP ports. A real lock
has one GATT service, and the C6 port opens every central as a session link. A bridge on
`ozkie` would therefore lose every relay context at each reconnect, and the server's stored
contexts would stop working.
- **Proposal:** a third characteristic, `relay` `4f5a4b32-0004-4c4f-434b-000000000001`, write and
  notify. The lock feeds writes to it into the relay link, framed exactly like `ozkie` (§2.1).
  The app keeps using `ozkie`. It needs no auth of its own: everything on it is EDHOC or OSCORE
  under a stored bond, and a stranger who writes to it gets what they would get on `ozkie`.
- Rejected: treating every BLE link as the relay. A direct app session would then persist
  contexts and require Echo, which is the wrong security model at the door.
- Lock-side cost: one characteristic in each port (C6, AK1037) plus a second `oz_ble` that is
  bound to the relay link. The engine is unchanged.

**G2. The MQTT payload has no message type.** §2.1 says the payload is the raw reassembled
message, but the type (EDHOC, OSCORE or error) is in the frame header, which isn't sent. The
receiver can't tell EDHOC message_1 from an OSCORE request.
- **Proposal:** payload = `type (1 B) ‖ message`, binary. This is `ozhostlock`'s wire without the
  2-byte length, since MQTT already carries the length. The bridge still never opens the message.

**G3. Topics and routing.** v1 sends downlink to the bridge's own JSON `command` topic and
uplink to the lock's `uplink` topic. A binary payload can't carry a JSON `target`.
- **Proposal:**
  - Downlink: `ozkie/<site>/bridges/<bridge_id>/ozkie/<lock_id>`. It is addressed to a bridge,
    so with ≥ 2 bridges only the chosen one writes and the lock never gets a duplicate. The
    server already picks the bridge by `bridge_id`.
  - Uplink: `ozkie/<site>/locks/<lock_id>/ozkie/<bridge_id>`. It is addressed to the lock,
    which is v1's U3 rule, so a failover changes nothing for the server. The server subscribes
    with `locks/+/ozkie/+`. The trailing `bridge_id` replaces the relaying-bridge field that v1
    carried in JSON. With it, two bridges holding one lock (XF-151) show up as duplicate uplinks
    from two `bridge_id`s (server ozkey-97).
  - QoS 0 both ways, not retained. OSCORE replay protection and Echo make a lost or repeated
    message safe.
  - The bridge doesn't correlate requests and replies. **The server correlates OSCORE
    responses by (lock_id, CoAP token). The `kid` identifies the context on requests only**: an
    OSCORE response omits it (RFC 8613 §6.1).
    - ozlock records token → {sender, kid, expiry} for each forwarded request and decrypts nothing.
    - Observe notifications reuse the registration token, so that entry lives until
      deregistration.
    - Every sender (the app via ozlock, ozpms, ozlodge) uses a random token of at least 4 bytes.
      ozlock refuses a second in-flight request on the same (lock_id, token). A response that
      matches no entry is dropped and logged.
  - **EDHOC on the relay is raw EDHOC (frame type 0), not CoAP**, so it has no token. Checked in
    `oz_lock.c` `rx_edhoc()`: the relay link holds **one** EDHOC handshake. A message_1 that
    arrives while another handshake is active is treated as that handshake's message_3, fails, and
    wipes both. So **ozlock allows at most one EDHOC handshake in flight per lock** across all
    senders. Every type 0 uplink belongs to that one handshake until it completes, fails or times
    out. The lock never sends type 2 today.
  - **Lock-side timeout and restart (added 2026-11-05).** Before this change a half-done
    handshake had **no timeout**. It lasted until message_3, a peer EDHOC error (type 2) or a link
    drop, so no server lease could safely outlast it. `oz_lock` now:
    - drops a handshake waiting for message_3 after **`OZ_LOCK_EDHOC_TIMEOUT_MS` = 30 000 ms**,
      on the rx path and in `oz_lock_tick()`;
    - treats a **message_1 that arrives mid-handshake as abort-and-restart**. The first byte tells
      them apart: message_1 opens with METHOD, a CBOR integer (< 0x40), while message_3 is one
      byte string (0x40–0x5b). The old handshake's late message_3 is then refused;
    - still resets on a failed message_3 (it sends EDHOC error 1 and clears the handshake), as
      before.
    - **Server lease: 35 s (30 s + 5 s margin)**, one per lock across all senders. A second
      message_1 is refused as busy, never queued (ozkey-97, 2026-11-05).
      With the restart rule, a server that releases early costs one failed handshake, never
      both. The one-in-flight rule stays the contract.
    - Tests are in `test_lock` `edhoc_restart`. A mutation check showed they fail without each half
      of the fix. Gate `f1a_edhoc_restart` rc=0. dlc6v2 compiles (800,447 B).
  - Failover mid-exchange: the server pins the bridge for one exchange (EDHOC m1→m3, or request →
    4.01 + Echo → retry). If the bridge changes, the server aborts the exchange and restarts it on
    the new bridge. On the lock (with G1): when the bridge's link drops, the port calls
    `oz_link_close()` on the relay link. That wipes any half-done EDHOC and the link's Echo
    state. Completed relay contexts persist (`L->relay[]`), so an unfinished exchange can't
    survive a bridge change and a finished one isn't lost.
  - The server never auto-retries `lock/unlock`, `lock/lock` or `dev/reset`. A lost response is
    reported as outcome "unknown" (the "unlock never queued" rule). v1's 60 s unlock queue
    (operator Q2) doesn't carry over to v2.
- The bridge's own `bridge` bond (time) is a **session on `ozkie`**, on the same BLE connection as the
  relay. It is not a relay client: the relay accepts admin and service bonds only (`oz_lock.c`). It
  is handled in the bridge and never reaches MQTT. Corrected in ozkey-48 rev 6 §2.3.
- MQTT buffer: 2048, tested with a 1024 B message plus the topic (§7 cliff).

**Impact:** the server (ozkey-97) needs a per-lock v2 path on these topics. The lock needs G1 in
`port/esp32c6` and `port/ak1037`. ozkey-48 needs a rev 6 for G1 and G2. F5's bench run over direct
BLE (`ozkie`) is unaffected.

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
