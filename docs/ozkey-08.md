# OZKEY-08 — ESP32-C6 Doorlock Emulator & BLE Bootstrap Contract

> **DRAFT 2026-07-13.** The ozkey-team response to the four firmware asks in
> `XFtposDecisions-43.md` §7.5 (BLE Wi-Fi-provisioning courier — the
> real-hardware unblock). Target hardware is on the bench: **Waveshare
> ESP32-C6 Touch LCD 1.47″** (operator's board, vendor bring-up in progress).
> Status: **contract drafted; server-side mDNS addressing SHIPPED + verified
> 2026-07-13; firmware phases start after the operator's board test.**
> Depends on: ozkey-02 §4 (Tuya 55AA / DPID codec — the hardware truth),
> ozkey-04 §3/§9 (device identity, device-scoped topics), ozkey-06 (§8-v2
> encrypted envelope), ozkey-07 (§5/§6/§10 hotel contract), XF-42
> (`ozkey_commissioner`), XF-43 §7.5.
>
> **⚡ OPERATOR DIRECTIVE 2026-07-16 — build order pivots to §10 "blelock v0":
> Mode 3 (OZLOCK residential, BANOI) ships FIRST** — BLE broadcast "OZLOCK" →
> BANOI banner-connect from Hồ sơ CN ⇄ Khoá cửa → exchange SSID/pass/server/
> name → enroll on ozlockserv :4200 → KEYPIN → on-screen 3×4 keypad unlock.
> The §7 phase table's hotel-first ordering (phase 1 = Wi-Fi/MQTT hotel mode)
> is superseded: hotel/MAOI becomes **v1**, same firmware, payload
> `mode=ozkey-local`. §10 is the canonical v0 design.

---

## 1. Purpose — the wall LockSim can't close

LockSim is a browser app: born network-resident (WebSocket→MQTT), it proves
the server / PIN / pairing / device-scoped-topic logic but **cannot be a BLE
peripheral** — so the field bootstrap (a boxed lock with no SSID, no broker
address, no Wi-Fi) is invisible to it. The ESP32-C6 emulator is a **real BLE
peripheral + real Wi-Fi client**, closing exactly that gap: MAOI walks up as
the BLE courier (XF-43 §7.5), hands over network credentials, and the lock
proceeds through the *already-verified* discovery→pair→operate path.

**Whitepaper alignment** (`docs/sovereign_edge_whitepaper.docx`): the
production consumer daughterboard is **ESP32-H2 — Thread + BLE only, no Wi-Fi
radio**; the commercial Mode A stack (ozkey-07) is Wi-Fi/MQTT. The C6 is the
one lab chip that speaks **both** (Wi-Fi 6 + BLE 5 + 802.15.4/Thread), so the
emulator covers the commercial hotel path now (phases 1–3) and the
consumer Matter-over-Thread tier later (phase 4) without changing boards.
Protocol work should stay portable across that split: BLE provisioning,
identity, and the DPID codec are radio-agnostic by design.

## 2. Hardware & framework

- **Board:** Waveshare ESP32-C6 Touch LCD 1.47″ — ESP32-C6 (RISC-V @160 MHz,
  Wi-Fi 6 2.4 GHz, BLE 5.3, 802.15.4), 1.47″ touch LCD, USB-C. (Panel/touch
  controller specifics to be confirmed at bring-up from the vendor demo —
  phase 0 is the operator's current step.)
- **Framework:** ESP-IDF v5.x + NimBLE for BLE; the vendor's LVGL demo as the
  display starting point. (Arduino-ESP32 acceptable for phase 1 speed if the
  vendor demo is Arduino-based — decide at phase 0 exit.)
- **Display duties** (XF-43 §7.5): the `ozk-…` device_id (what the operator
  types into MAOI's "Gắn khoá"), the factory-pubkey **QR trust anchor**,
  lock/PIN state, and live provisioning status (`BLE ✓ / WiFi ✓ / broker ✓`).
- **Touch duties:** simulated keypad (PIN entry → DPID verify → door
  granted/denied events on the log topic) and a long-press gesture to
  (re)open the BLE provisioning window (§4.4).

## 3. Identity & trust anchor

- **device_id:** production = derived from a P-256 keypair in eFuse
  (ozkey-04 §3); **emulator interim = `ozk-<machex>`** from the factory MAC —
  identical to the LockSim/ozkeyserv lab convention, so cockpit/MAOI flows
  are unchanged.
- **Trust anchor (XF-43 §7.5 ask 2):** a factory keypair; the **public key
  rendered as a QR on the display** (production: printed label). MAOI scans
  it before the BLE session so the encrypted handshake is pinned to the
  physical lock in hand — no remote MITM can complete it. Emulator: keypair
  generated at first boot, persisted in NVS, QR on demand.

## 4. BLE provisioning GATT profile — draft v1 (ask 1; confirm with FtposPM)

> ⚠ **SUPERSEDED (2026-07-14) by [`blelock/CONTRACT.md`](../blelock/CONTRACT.md)**
> — the operator's firmware repo carries the canonical profile (service
> `4f5a4b31-0001-4c4f-434b-000000000001`, `provision`/`status`/`info`
> characteristics, flat payload with `mode=ozkey-local` + `heartbeat_s`,
> `WIFI_FAIL`/`BROKER_FAIL` statuses; validation authority =
> `ozkey_commissioner/lib/src/provision_payload.dart`). Board bring-up is
> **done** (`blelock/HARDWARE.md`: verified ST7789 pin map + AXS5106L touch
> wake; toolchain = **Arduino core 3.x**), so §7 phase 0 is complete and the
> §2 "ESP-IDF vs Arduino" question is decided. This section is retained for
> the rationale (session security §4.1, re-provisioning window §4.4); where
> the two disagree, blelock/CONTRACT.md wins.

Advertised name: `OZK-<last 4 of device_id>`. One primary service, three
characteristics. UUIDs (canonical once FtposPM confirms — `ozkey_commissioner`
consumes these verbatim):

| Item | UUID | Props | Content |
|---|---|---|---|
| Provisioning service | `4f5a4b45-5900-4f01-a000-6f7a6b657631` | — | — |
| `SESSION` | `4f5a4b45-5900-4f02-a000-6f7a6b657631` | read | device ephemeral X25519 pubkey + 16-byte nonce + device_id (plaintext bootstrap of the §8-v2 session) |
| `PROV` | `4f5a4b45-5900-4f03-a000-6f7a6b657631` | write | the encrypted provisioning envelope (§4.2) |
| `STATUS` | `4f5a4b45-5900-4f04-a000-6f7a6b657631` | notify | closed-loop state machine (§4.3) |

### 4.1 Session security
`ozkey_commissioner`'s §8-v2 envelope, reused verbatim (ozkey-06 / XF-42):
X25519 ECDH (commissioner ephemeral ↔ device ephemeral from `SESSION`) →
HKDF → AES-256-GCM. The commissioner verifies the device's `SESSION` key is
signed by the factory key scanned from the QR (§3) before writing `PROV`.
Lab phase 2 may run the envelope in plaintext to prove plumbing; phase 3
turns encryption on — **production is never plaintext**.

### 4.2 Provisioning payload (inside the envelope)
```json
{
  "v": 1,
  "mode": "OZKEY",                     // double duty: exits Matter fabric (ask 3)
  "wifi": { "ssid": "…", "psk": "…" },
  "server": { "mdns": "_ozkey._tcp" },  // OR { "host": "10.1.1.21", "port": 1883 }
  "site_id": "hotel",
  "room_no": "101"                     // OPTIONAL — collapses pair into one door visit
}
```
- BLE carries **only** Wi-Fi + server + site + mode (XF-43 §7.5): the token
  and binding are minted server-side at `/locks/pair`, reusing the verified
  path. The optional `room_no` lets MAOI collapse steps 1+2 of the §7.5
  split into a single visit (the lock then auto-requests pairing on connect).
- `mode` values: `OZKEY` (Mode A commercial — this doc), `OZLOCK` (market A
  personal cloud, ozkey-04/05). **Matter takeover semantics (ask 3):** on a
  production lock, accepting a `mode=OZKEY` payload **leaves the Matter
  fabric** and stops Matter advertising — commercial commissioning is
  Matter-exclusive (a guest must never add room 101 to their personal Home).
  Emulator: phase 4 demonstrates the takeover on Thread.

### 4.3 Closed-loop confirm (no fire-and-forget)
`STATUS` notifies: `BLE_OK → WIFI_JOINING → WIFI_OK → BROKER_CONNECTING →
BROKER_OK → READY` (or `ERR_WIFI_AUTH / ERR_WIFI_TIMEOUT / ERR_BROKER /
ERR_PAYLOAD`). The commissioner shows success **only at `BROKER_OK`+** —
the lock proved the credentials work while the courier is still at the door.

### 4.4 Re-provisionable, not one-shot
Wi-Fi passwords rotate; server IPs change. The BLE provisioning service stays
present but **gated**: it accepts `PROV` writes only during a provisioning
window opened by a physical action at the lock (emulator: display long-press;
production: reset-hole tap) — walk-up re-provisioning without factory reset,
but never silently writable from radio range.

## 5. Network bootstrap flow (end-to-end)

```
Phase 0 (boxed lock)     BLE only — advertises OZK-xxxx, QR on display
  │  MAOI scans QR → BLE session → writes PROV (wifi + server + site + mode)
  ▼
Wi-Fi join               STATUS: WIFI_OK
  │  resolve server: mDNS _ozkey._tcp  ──►  SHIPPED server-side 2026-07-13:
  │  ozkeyserv advertises "ozkeyserv-<site>" _ozkey._tcp with
  │  txt {site, api, broker} — verified via dns-sd browse+resolve.
  │  (fallback: pinned host:port from the payload)
  ▼
MQTT connect             STATUS: BROKER_OK   → commissioner shows success
  │  publishes hotel/locks/unpaired/heartbeat (ozkey-02 §3.1)
  ▼
Pair                     MAOI "Ghép khoá vật lý" (or cockpit) → /locks/pair
  │  provision_assign carries room_no + site_id + device_id + mac_token
  ▼
Operate                  device-scoped topics (ozkey-07 §10): heartbeat/log/
                         command on ozkey/<site>/locks/<device_id>/… —
                         identical to LockSim conformance from here on.
```

## 6. Emulator behavior spec — LockSim parity

The C6 emulator mirrors LockSim's verified behavior (LockSim's decoder is the
hardware truth, ozkey-02 §4): Tuya 55AA frames, DPID 21 (temp PIN write) /
DPID 22 (delete), heartbeat cadence, door-transaction publishes on the log
topic, legacy-room-copy drop once device-scoped (ozkey-07 §10). Touch keypad
entries verify against stored credentials and publish granted/denied with
the same payload shape LockSim emits. Conformance check: run the ozkey-02
frame vectors against the C6 decoder before phase 1 exit.

## 7. Build phases (the programming plan, post-bring-up)

| Phase | Scope | Exit criterion |
|---|---|---|
| **0 — bring-up** (operator, NOW) | vendor demo: LCD, touch, USB flash | board confirmed working; panel/touch/framework facts recorded |
| **1 — Wi-Fi/MQTT emulator** | display + Wi-Fi + MQTT, hotel mode, no BLE; device_id on screen; keypad → DPID verify → log | passes the §6 parity checks against live ozkeyserv + cockpit; pairs via "Ghép khoá vật lý" |
| **2 — BLE provisioning** | §4 GATT service (plaintext envelope); provisioning window; MAOI `flutter_blue_plus` counterpart lands app-side | boxed-lock → BLE → Wi-Fi → BROKER_OK closed loop, end to end with MAOI |
| **3 — security on** | §8-v2 encryption, QR trust anchor render + scan, signed SESSION key | encrypted commissioning with pinned trust anchor; plaintext path removed |
| **4 — Matter/Thread exploration** | Matter-over-Thread consumer mode; `mode=OZKEY` fabric takeover demo | whitepaper consumer-tier path demonstrated on the same board |

Each phase ends with a bench verify against the live ozkeyserv/cockpit —
same discipline as the LockSim milestones.

## 8. Deliverable map to XF-43 §7.5 asks

| XF-43 ask | Where answered |
|---|---|
| 1. `ProvisionPayload` GATT profile + fields (broker/site_id/mode) | §4 (draft v1 — FtposPM to confirm UUIDs + fields, then canonical) |
| 2. Out-of-box trust anchor (factory pubkey → MAOI) | §3 + §4.1 (QR on display; production = printed label) |
| 3. Matter takeover semantics | §4.2 `mode=OZKEY` = fabric exit; demoed phase 4 |
| 4. ESP32-C6 reference peripheral | §2/§6/§7 — phases 1–3 on the Waveshare board |

Plus the addressing prerequisite XF-43 §7.5 flagged: **mDNS `_ozkey._tcp`
advertising is SHIPPED in ozkeyserv** (verified browse+resolve 2026-07-13);
static-IP/DHCP-reservation remains the fallback documented in the payload.

## 9. Open items

1. FtposPM sign-off on §4 UUIDs + payload fields (then they're canonical for
   `ozkey_commissioner`).
2. Phase 0 facts: panel/touch drivers, vendor demo framework → framework
   decision (ESP-IDF vs Arduino start).
3. eFuse P-256 identity + factory-key signing flow for production (emulator
   uses NVS + MAC-derived id).
4. Whether `room_no` in the payload (§4.2 one-visit collapse) ships in v1 or
   stays a v2 option — MAOI UX call.

---

## 10. blelock v0 — OZLOCK/BANOI first light (operator directive 2026-07-16)

The four operator requirements: (1) lock **broadcasts BLE name "OZLOCK"**,
BANOI detects it via a banner and connects from the Profile tab (Hồ sơ CN ⇄
Khoá cửa); (2) over BLE the pair exchange **SSID, Wi-Fi password, OZLOCK
server URL, doorlock name** (lock → app: device_id/MAC/fw); (3) when the lock
reaches ozlockserv, **BANOI shows connected**; (4) BANOI adds a **KEYPIN** →
entered on the lock's **on-screen 3×4 keypad** → unlock. Display: during
commissioning show **doorlock name + IP address**; once connected show
**name + keypad**.

### 10.1 Proven baseline (blelock/ test sketches, 2026-07-14)

| Capability | Sketch | Fact carried into firmware |
|---|---|---|
| BLE server advertising **"OZLOCK"** + GATT write | `BLE/BLE.ino` | Bluedroid `BLEDevice`; auto re-advertise on disconnect |
| **BLE + Wi-Fi concurrent** (the closed-loop coex risk) | `Wifi/Wifi.ino` | **PROVEN** — BLE server + `WiFi.begin()` together; IP renders on screen |
| Display ST7789 172×320 | `DisplayTest`, `color` | **panel is BGR**: IPS flag `false`, R/B color codes swapped, rotation 5 |
| Touch @0x3B (wake seq, 12-byte read) | `Touch/Touch.ino` | transform `X = 320 − rawY; Y = rawX` |
| Touch-driven grid UI | `TicTacToe` | keypad hit-testing precedent |

### 10.2 Identity — lock-minted, LockSim-identical

`device_id = "ozk-" + hex(MAC)` (§3 interim rule). v0's one change from the
LockSim bench: **BLE replaces the human typing the id into BANOI** — the app
*reads* it from the `info` characteristic. Same ID-exchange semantics XF-42 P2
verified; ozlockserv is untouched.

### 10.3 GATT (v0 amendments to blelock/CONTRACT.md — canonical)

- **Advertised name: `OZLOCK`** (operator requirement; supersedes both §4's
  `OZK-<last4>` and CONTRACT.md's `OZKEY-<last4>`). Multiple unprovisioned
  locks disambiguate by `info.device_id`.
- Service + `provision`(write) / `status`(notify) / `info`(read) as in
  CONTRACT.md. `info` gains `"name"` (current doorlock name).
- Payload = flat `ozkey_commissioner ProvisionPayload` with **one new optional
  field `name`** (doorlock display name; LockSim ignores it):

```json
{ "v": 1, "mode": "ozkey-cloud",
  "ssid": "…", "password": "…",
  "broker_host": "10.1.1.21", "broker_tcp_port": 1883,
  "server_ip": "10.1.1.21", "server_port": 4200,
  "device_id": "ozk-<machex>",      // echo of info.device_id — firmware validates match
  "site_id": "lab", "name": "Cửa trước", "heartbeat_s": 60 }
```

- Status ladder terminal for mode 3 = **ENROLLED** (mode 2 stops at BROKER_OK).
  v1 plaintext (bench); v2 = §4.1 envelope on the same characteristic.

### 10.4 End-to-end flow (server wire pre-existing, verified vs LockSim)

```
0  boot → advertise "OZLOCK" → screen: device_id + "chờ ứng dụng"
1  BANOI Khoá cửa: BLE scan → banner "Phát hiện khoá OZLOCK" → connect →
   read info{device_id,mac} → subscribe status
2  BANOI: POST /pairings {device_id, app_id, label}   (existing XF-42 P2 call)
   BANOI: write provision JSON (ssid/pass/broker/name)
3  lock: WIFI_JOINING → WIFI_OK (screen: name + IP) → MQTT :1883 →
   BROKER_OK → publish ozkey/lab/locks/<id>/enroll {device_id, mac, fw}
4  ozlockserv handleEnroll: matches pre-registered pairing → status='enrolled'
   → enrollment_ack {label, broker_username/secret, heartbeat_s} on …/command
   → lock notifies ENROLLED, persists all to NVS → keypad screen
5  BANOI watchStatus(device_id) → 'enrolled' → "Đã kết nối" ✓ (existing poll;
   enrolled lock joins the Khoá cửa list via keyring_store.addEnrolledLock)
6  KEYPIN: BANOI Cấp mã (existing grant flow) → ozlockserv pending_queue →
   flushed on heartbeat: {msg_id, device_id, action, grant_id, payload_hex,…}
   → lock parses DPID frame (payload_hex; vectors = ozkey_commissioner
   DpidFrames / LockSim tuya.ts) → stores PIN in NVS → keypad entry →
   UNLOCK (5s auto-relock, proven) / DENIED → publish …/log → BANOI event feed
```

New code = firmware + BANOI's BLE leg only; steps 2/5/6 app-side and 4/6
server-side already run live against LockSim.

### 10.5 Firmware design (Arduino core 3.x)

State machine (NVS-persisted):
```
BOOT ─not provisioned→ ADVERTISING ─BLE write→ JOINING(WIFI→BROKER→ENROLL)
  └─provisioned→ RECONNECT (creds from NVS) → OPERATIONAL (keypad)
factory reset (long-press '#' 5 s): wipe NVS → ADVERTISING
```
Screens (BGR palette, rotation 5): ADVERTISING = "OZLOCK" + device_id + BLE
state · JOINING = **name + ladder + IP** · OPERATIONAL = **name header (+ MQTT
status dot) + 3×4 keypad** (1-9,*,0,#; *=clear #=submit, masked dots) +
UNLOCKED(blue,5 s)/DENIED(red) full-screens. Modules: `gatt` / `provision`
(JSON+NVS) / `net` (WiFi+PubSubClient) / `wire` (enroll·heartbeat·log +
command parse) / `dpid` (payload_hex → add/revoke PIN, ≤16 slots) / `ui` /
`touch`. Serial log every event.

### 10.6 BANOI app-side work (the only FTPOS changes)

1. `flutter_blue_plus` dep + iOS `NSBluetoothAlwaysUsageDescription` + Android
   `BLUETOOTH_SCAN/CONNECT` (first BLE dep — XF-42 §5 anticipated).
2. `FlutterBlueOzkeyTransport` implementing the **existing `OzkeyBleTransport`
   port** (built app-side for XF-43 §7.5, shared by design): scan filter =
   service UUID / name "OZLOCK"; read `info`; write `provision`; map `status`
   notifies onto `OzkeyStatus`.
3. Khoá cửa sub-page: background scan while open → **banner** "Phát hiện khoá
   OZLOCK — Kết nối ›" → wizard: name + SSID/password (server prefilled from
   build config) → POST /pairings → write → live ladder → ENROLLED joins the
   existing lock list. (Same banner grammar as MAOI's unpaired-lock banner.)
4. KEYPIN: zero new code — existing Cấp mã grant flow.

### 10.7 Milestones (each independently demoable)

> **BUILD LOG 2026-07-16:** B1–B3 firmware **written + compiling clean** —
> `blelock/blelock/blelock.ino` (state machine, GATT ×3 chars w/ chunked-write
> buffer, WiFi→MQTT→enroll→ack, DPID 21/22/1 parser vs frame law, NVS config +
> ≤64 PIN slots w/ validity windows (NTP), 3×4 touch keypad, 5 s auto-relock,
> wrong-PIN lockout 5→60 s, '#'-hold factory reset, heartbeat + log publishes).
> 1.48 MB → needs **FlashSize=8M + PartitionScheme=default_8MB** (44% of 3 MB
> app). Deps vendored to ~/Documents/Arduino/libraries: PubSubClient 2.8,
> ArduinoJson 7.4.2. Flash + bench steps: `blelock/blelock/TESTING.md`.
> **Awaiting on-device verify (operator flashes).** B4 (BANOI) next.

| # | Deliverable | Proof |
|---|---|---|
| **B1** | ADVERTISING + GATT (info/status/provision→NVS) + screens | nRF Connect: read info, write payload, watch ladder |
| **B2** | WiFi+MQTT+enroll+ack+heartbeat | ozlockserv log `ENROLLED ozk-… site 'lab'`; screen shows name+IP |
| **B3** | command envelope + DPID parse + PIN store + keypad unlock + log | grant (curl/BANOI) → PIN opens lock; log row lands |
| **B4** | BANOI BLE transport + banner + wizard | end-to-end from the app, no curl |
| **B5** | conformance: reboot persistence, factory reset, wrong-PIN lockout (5→60 s), re-provision after WIFI_FAIL | scripted checklist |

### 10.8 v0 decisions

1. device_id lock-minted from MAC — ✅ §10.2. 2. Advertised name plain
"OZLOCK" — ✅ operator. 3. `name` added to ProvisionPayload (optional) — ✅.
4. Keypad = on-screen 3×4 grid — ✅ operator. 5. OPEN: SSID autofill on iOS
(NEHotspot entitlement) — fallback manual entry.
