# OZKEY-02 — OZKEYSERV ⇄ LockSim Handshake Contract (Mode A)

> The authoritative wire contract between **OZKEYSERV** (this repo — gateway
> :3200, MySQL, TalkPOS broker `mqtt://10.1.1.21:1883`) and **LockSim**
> (`~/Documents/Dev/locksim`, github.com/ebizcoAU/locksim) running in
> **Mode A — Pure Software Emulation**: LockSim simulates the *entire* door
> lock (motherboard MCU + Wi-Fi module) in the browser. No ESP32, no Web
> Serial wire. Written 2026-07-06 after auditing both codebases
> (`ozkeyserv/server.js` vs `locksim/lib/provisioning.ts`, `lib/tuya.ts`,
> `docs/locksim-01.md`). Where the two disagree today, this doc decides the
> canonical form and lists who changes what (§6).

---

## 1. Mode A transport model

LockSim has **no MQTT client** — it's a browser app. In Mode A the "radio
link" between the simulated lock and OZKEYSERV is **HTTP against the gateway**
(CORS is already open on :3200), with manual console copy-paste as the
fallback that works today:

| Direction | Today (manual) | Target (automated) |
|---|---|---|
| Lock announces MAC | copy LockSim TX broadcast → `POST /ozkeyserv/api/sim/unpaired-heartbeat` (or just curl it) | LockSim's "Broadcast Hardware MAC ID" button POSTs it directly |
| Server assigns room | copy the §3.2 JSON into LockSim's "OZKEYSERV/ Onboarding Handshake" panel → Publish to Lock | LockSim polls the gateway for its handshake (§3.5 proposed endpoint) |
| Lock heartbeat | `POST /ozkeyserv/api/sim/room-heartbeat {room_no}` | LockSim's 10-min heartbeat timer fires the POST itself |
| Server → lock commands | copy `payload_hex` from cockpit terminal / `GET /queue` into LockSim's hex injector | LockSim polls and auto-feeds `payload_hex` into its Tuya parser |

The real MQTT broker still matters server-side (real hardware will use it),
but **nothing in the Mode A loop requires it** — the `/sim/*` endpoints stand
in for both broker directions.

## 2. Registration / onboarding sequence (how LockSim gets a room)

> **Phase 0 precedes all of this:** a factory-fresh lock has no Wi-Fi. The
> OZKEY mobile app delivers `{ssid, password, server_ip, server_port}` over
> BLE (Service "OZKEY") — contract in **`ozkey-03.md`**. Everything below
> assumes Phase 0 is complete and the lock is targeting the BLE-received
> gateway address.

```
 LockSim (browser)                      OZKEYSERV :3200                Operator (cockpit :3300)
 ─────────────────                      ───────────────                ────────────────────────
 1. BLE Provisioning ON
    state=UNPROVISIONED, LED blue
 2. "Broadcast Hardware MAC ID"
    JSON {mac:"AA:BB:CC:11:22:33",...}
      ── POST /sim/unpaired-heartbeat ──► unpairedCache (TTL 120 s;
                                          re-announce to stay visible)
                                              │  GET /locks/unpaired
                                              ▼
                                        3. MAC chip appears in the
                                           cockpit UNPAIRED HW strip
                                        4. Operator picks MAC + room →
                                           PAIR LOCK TO ROOM
                                           POST /locks/pair
                                              │ rooms.mac_address = MAC
                                              │ rooms.status = 'Available'
 5. §3.2 handshake JSON ◄──────────────── emitted (event log / MQTT /
    pasted or polled into LockSim         proposed poll endpoint)
    parseOnboardingPayload() checks
    topic filter + mac + room_no + server_ip
 6. display "PAIRED - ROOM X", LED green ×3,
    persists {assigned_room_no, server_ip,
    mac_token, mac} → locksim.provisioning.v1
 7. Steady state: heartbeat → POST /sim/room-heartbeat
    → server flushes pending_queue → lock ingests payload_hex frames
```

## 3. Canonical channels & payloads

### 3.1 Discovery announce (lock → server)

MQTT topic (for real hardware): `hotel/locks/unpaired/heartbeat`.
Mode A equivalent: `POST /ozkeyserv/api/sim/unpaired-heartbeat`.

LockSim's `BROADCAST_TOPIC` constant (`OZKEYSERV/provision/announce`) is a
legacy label only — the canonical destination is the above. Payload
(LockSim's `buildBroadcastPayload`; server ignores extra fields):

```json
{ "mac": "AA:BB:CC:11:22:33", "device": "tuya-lock-zs-mb", "fw": "1.4.2",
  "capabilities": ["ble","matter","tuya-mcu"], "rssi": -47, "ts": 1782... }
```

Key is `mac` (or `mac_address`); any separator format; server normalizes to
uppercase colon form. Server also accepts a bare MAC string over MQTT.

### 3.2 Provisioning handshake (server → lock)

Delivered on MQTT topic `hotel/rooms/<room_no>/lock/command` for real
hardware; in Mode A it is the JSON pasted into LockSim's **OZKEYSERV/
Onboarding Handshake** panel (or fetched via §3.5). LockSim's
`parseOnboardingPayload()` is the validator, so the payload MUST contain:

| Field | Req | Notes |
|---|---|---|
| `topic` | ✅ | Must match filter `hotel/rooms/+/lock/command` — LockSim validates this **embedded field** (that's how Mode A works without a broker) |
| `mac` | ✅ | Must equal the device MAC (case-insensitive). **Key name `mac`, not `mac_address`** |
| `room_no` | ✅ | String; falls back to topic segment 3 |
| `server_ip` | ✅* | LockSim's validator currently requires it, but since ozkey-03 the BLE payload is the source of truth — treat this as a **consistency check** (warn on mismatch), and relax the validator to optional. Include `server_port` alongside for the same check |
| `mac_token` | ○ | `OZK-XXXX-XXXX-XXXX`; if absent LockSim mints its own — server SHOULD issue and record it |
| `op` | ○ | `"provision_assign"` (LockSim sample convention) |

Canonical example — `POST /locks/pair` must emit exactly this shape:

```json
{
  "topic": "hotel/rooms/101/lock/command",
  "op": "provision_assign",
  "mac": "AA:BB:CC:11:22:33",
  "room_no": "101",
  "server_ip": "10.1.1.21",
  "mac_token": "OZK-7F3A-C210-9E4D",
  "issued_by": "OZKEYSERV/"
}
```

The server's current `hotel/locks/<mac>/pair/confirm` publish is a
debug-only side channel; LockSim never sees it.

### 3.3 Steady-state heartbeat (lock → server)

MQTT: `hotel/rooms/<room_no>/lock/heartbeat`. Mode A:
`POST /ozkeyserv/api/sim/room-heartbeat {"room_no":"101"}` — the response
returns `{flushed: N}` so LockSim knows commands were released.

Cadence: LockSim's deep-sleep loop is **600 s**; the old ozkey spec said 30 s.
**The server is cadence-agnostic** (any heartbeat flushes the queue), so
cadence is the lock's choice; shorten LockSim's timer on the bench if waiting
10 minutes is annoying. Payload is ignored by the server — only the room
identity matters.

### 3.4 Credential/command delivery (server → lock)

Server wraps every queued action in a JSON envelope:

```json
{ "msg_id": "oz-12-...", "room_no": "101", "action": "issue-key",
  "credential_id": 7, "payload_hex": "55 AA 00 06 ...", "issued_at": "...",
  "source": "ozkeyserv" }
```

**LockSim routing rule:** JSON without `payload_hex` but with `mac`+`room_no`
→ provisioning parser (§3.2). JSON **with** `payload_hex` → strip the envelope
and feed the hex into the Tuya frame parser. (Gap today: LockSim's `{`-router
only knows the provisioning path — §6 #5.) Manual path: paste just the
`payload_hex` value into LockSim's hex injector.

### 3.5 Proposed Mode A pull channel (to remove all copy-paste)

New gateway endpoint — **not built yet** (§6 #7):

`GET /ozkeyserv/api/locks/poll?mac=AA:BB:CC:11:22:33`
→ `{ handshake: <§3.2 object>|null, commands: [<§3.4 envelope>...] }`,
marking returned queue rows `sent` / credentials `synced`, exactly like the
MQTT flush. LockSim (browser) can then run the entire lifecycle unattended
against `http://localhost:3200`.

## 4. Tuya frame contract (the `payload_hex`)

Both sides agree on framing:
`[55 AA][ver 00][cmd 1B][len 2B BE][payload][checksum = sum % 256]`.

**LockSim's decoder is the hardware truth** — the server must emit what it
parses: **DP_REPORT frames (cmd 0x06) with the DPID codec, not custom
command IDs**. DP wrapper inside the frame payload:
`[dpid 1B][type 1B][len 2B BE][data]` (RAW=0x00, BOOL=0x01, VALUE=0x02,
STRING=0x03, ENUM=0x04).

| Action | DPID | DP data layout |
|---|---|---|
| Add temp PIN | **21** (RAW) | `[Slot 2B BE][PIN ascii var][Start unix 4B BE][End unix 4B BE]` |
| Add temp RFID | **23** (RAW) | same layout, value = card UID |
| Delete PIN | **22** | `[Slot 2B BE]` |
| Delete RFID | **24** | `[Slot 2B BE]` |
| Remote unlock | **1** (BOOL) | `01` |
| Access result (lock → up) | **8** (ENUM) | `00` success / `01` denied / `02` expired |
| Heartbeat frame | — | cmd `0x00`, empty payload |

> ❌ **Current server mismatch:** `buildCredentialFrame()` in `server.js`
> emits a custom cmd `0x65` with layout
> `[type 1B][slot 1B][len 1B][value][from 4B][to 4B]`. LockSim cannot parse
> this. Server must switch to DPID 21/23 DP_REPORT frames. Conformance test
> vector: `SAMPLE_ADD_TEMP_PIN_FRAME` in `locksim/lib/tuya.ts` (slot 14,
> PIN 482915) — the server's frame for the same inputs must byte-match.

**Fingerprint gap:** ozkey's `credentials.type` ENUM includes `fingerprint`,
but LockSim has no temp-fingerprint DPID (only 21–24 pin/rfid). Server should
reject/hold `fingerprint` issues destined for LockSim benches.

## 5. Identity & data-shape conventions

- **MAC:** uppercase colon form `AA:BB:CC:11:22:33` everywhere. Server
  normalizes inbound liberally. LockSim's factory MAC is exactly
  `AA:BB:CC:11:22:33` — the same string was used for manual testing in ozkey
  on 2026-07-05, so wipe stale pairings when in doubt
  (`POST /locks/unpair {room_no}`).
- **Slots:** LockSim slots are 2-byte; ozkey injector caps at 255 — fine.
- **Validity window:** unix seconds, u32 BE, inclusive. LockSim checks
  against its **Virtual Master Clock** (warpable) — bench "expired" ≠ real
  expired.
- **room_no:** plain string (`"101"`), no slashes (appears in topics).

## 6. Sync gaps found 2026-07-06 — who fixes what

| # | Gap | Owner | Fix |
|---|---|---|---|
| 1 | Announce label: LockSim's `BROADCAST_TOPIC` says `OZKEYSERV/provision/announce`; canonical channel is `hotel/locks/unpaired/heartbeat` / `POST /sim/unpaired-heartbeat` | **LockSim** | Rename constant; ideally POST the broadcast to the gateway directly (Mode A) |
| 2 | Handshake shape: server's pair payload sends `mac_address`, omits `server_ip`/`mac_token`/embedded `topic`, and goes to the wrong topic — LockSim's validator rejects it | **OZKEYSERV** | `/locks/pair` must emit the §3.2 JSON on `hotel/rooms/<room_no>/lock/command` (and expose it for Mode A pickup) |
| 3 | Credential frames: server's custom cmd `0x65` vs LockSim's DPID 21/23 DP_REPORT codec | **OZKEYSERV** | Rewrite frame builder per §4; verify against `SAMPLE_ADD_TEMP_PIN_FRAME` |
| 4 | Heartbeat: LockSim's 10-min timer only writes a TX log line — nothing reaches the gateway in Mode A | **LockSim** | Heartbeat tick → `POST /sim/room-heartbeat` when provisioned |
| 5 | LockSim's `{`-router can't unwrap the §3.4 envelope (`payload_hex`) | **LockSim** | If parsed JSON has `payload_hex`, feed the hex to the Tuya parser |
| 6 | Fingerprint type unsupported lock-side | both | Server blocks/holds fingerprint issues; or LockSim adds a DPID pair |
| 7 | No pull channel for a browser-only lock (browser can't subscribe MQTT-TCP) | **OZKEYSERV** | Add `GET /locks/poll?mac=` per §3.5 |
| 8 | Revoke: LockSim fires DPID 22/24 deletes, ozkey has no revoke endpoint | **OZKEYSERV** | Add `/pms/revoke-key` → DPID 22/24 frame, action_type `revoke-key` |

Until these land, the full Mode A loop works **manually**: LockSim console ⇄
cockpit terminal copy-paste, with `/sim/*` curls standing in for the radio.

## 7. Conformance checklist (Mode A bench acceptance)

1. LockSim broadcast reaches the gateway → MAC chip visible in the cockpit
   UNPAIRED HW strip within 3 s.
2. PAIR LOCK TO ROOM → LockSim (given the §3.2 JSON) shows
   `PAIRED - ROOM <X>`, LED green ×3, `locksim.provisioning.v1` persisted.
3. Issue PIN `482915`, slot 14, validity spanning today → LockSim RX terminal
   annotates "Add Temporary PIN, Slot: 14, Value: 482915"; registry row ACTIVE.
4. Keypad `482915#` unlocks; warp Virtual Clock past the window → EXPIRED +
   DPID 8 = `02` frame on TX.
5. Heartbeat (`/sim/room-heartbeat`) → ozkey queue rows flip `queued→sent`,
   credential `pending→synced`, room tile red→blue in the cockpit.

---

## 8. LockSim team response (2026-07-06) — agree / disagree / DONE

> Written from the LockSim side after wiring a real MQTT client. This section
> supersedes the transport assumptions in §1 and §3.5 where they conflict.

### 8.1 Transport decision — MQTT-over-WebSocket, not gateway HTTP (supersedes §1)

The §1 premise "LockSim has no MQTT client — it's a browser app" is **wrong for
WebSockets**. A browser cannot open MQTT over raw TCP, but it *can* over WS.
LockSim now connects **`ws://<host>:9001/mqtt`** with mqtt.js (Mosquitto
`listener 9001 / protocol websockets`, already live on 10.1.1.21 — verified
round-trip 2026-07-06). This is the same broker OZKEYSERV uses on TCP :1883.

**Consequences:**
- The **canonical Mode A transport is the broker**, not gateway HTTP. LockSim
  publishes/subscribes the §3.1–§3.4 topics directly — no copy-paste, no polling.
- **The `/sim/*` endpoints are no longer LockSim's transport** — keep them only
  as optional manual test hooks. LockSim now hits the *real* topics:
  `hotel/locks/unpaired/heartbeat` (announce) and `hotel/rooms/<room>/lock/heartbeat`.
- **§3.5 `GET /locks/poll` (gap #7): DROP.** WS subscribe gives real-time push;
  a browser-only pull channel is unnecessary. Do not build it.

### 8.2 The gateway :3200 API is control-plane, not the lock's data path

The doorlock **server** is two co-located services, and the distinction matters:

| Service | Endpoint | Role for the lock |
|---|---|---|
| MQTT broker (Mosquitto) | `10.1.1.21:1883` TCP · **`:9001` WS** | **Lock's data path** — all announce/handshake/heartbeat/command traffic |
| OZKEYSERV gateway (Express) | **`10.1.1.21:3200`** `/ozkeyserv/api` | Control-plane — cockpit, PMS, `/health`. LockSim only calls `GET /health`. |

LockSim's Settings dialog now has both: MQTT broker (host + WS port 9001 + path)
as the data path, and the gateway API (port 3200 + base path) used solely for a
reachability `GET /health` probe. **No credential ever flows over :3200.**

### 8.3 DONE on LockSim (was gaps #1, #4, #5)

- **#1** — "Register Doorlock" publishes `{mac, device, fw, capabilities, rssi, ts}`
  to `hotel/locks/unpaired/heartbeat`. The legacy `OZKEYSERV/provision/announce`
  label is retired.
- **#4** — the heartbeat tick publishes `hotel/rooms/<room_no>/lock/heartbeat`
  (payload `{mac, room_no, ts}`) so your existing subscriber flushes the queue.
- **#5** — inbound JSON containing `payload_hex` is unwrapped from the §3.4
  envelope and fed straight to the Tuya frame parser.

### 8.4 Still needed from OZKEYSERV (now over the live broker)

- **#2 (BLOCKER)** — on `POST /locks/pair`, **publish the §3.2 handshake JSON to
  `hotel/rooms/<room_no>/lock/command`**. Key must be **`mac`** (not
  `mac_address`), with `room_no`. **Relaxation:** LockSim now injects the actual
  MQTT topic into the payload before validating, so the **embedded `topic` field
  is optional** — you no longer need to embed it. `server_ip` is still validated;
  keep sending it (it's the gateway address, used as a consistency value).
- **#3** — credential frames must be **DPID 21/23 DP_REPORT (cmd 0x06)** inside
  the envelope's `payload_hex`. Byte-match `SAMPLE_ADD_TEMP_PIN_FRAME`.
- **#8** — add `/pms/revoke-key` → DPID 22/24 delete frame.

### 8.5 Revised gap ownership

| # | Status |
|---|---|
| 1 announce topic | **DONE (LockSim)** — publishes canonical topic over MQTT-WS |
| 2 handshake shape/topic | **DONE (OZKEYSERV, 2026-07-06)** — `/locks/pair` publishes `provision_assign` (`mac`, `room_no`, `server_ip`, `server_port`, `mac_token`, no `payload_hex`) to `hotel/rooms/<room>/lock/command`; token persisted in `rooms.mac_token`. Verified live over the broker against LockSim's validator rules |
| 3 DPID frames | **DONE (OZKEYSERV, 2026-07-06)** — DP_REPORT (cmd 0x06) DPID 21/23 add + 22/24 delete builders; byte-matches `SAMPLE_ADD_TEMP_PIN_FRAME` and `SAMPLE_ADD_TEMP_RFID_FRAME` exactly. PIN validated digits-only, RFID even-length hex |
| 4 heartbeat | **DONE (LockSim)** |
| 5 payload_hex unwrap | **DONE (LockSim)** |
| 6 fingerprint | **HELD (OZKEYSERV, 2026-07-06)** — `/pms/issue-key` returns 422 for `fingerprint` until a DPID pair exists |
| 7 poll channel | **DROPPED** — WS subscribe replaces it |
| 8 revoke endpoint | **OPEN (OZKEYSERV)** — `buildDeleteFrame()` (DPID 22/24) already exists server-side; endpoint still to add |
