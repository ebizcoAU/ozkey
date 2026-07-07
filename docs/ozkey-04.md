# OZKEY-04 — Universal Commissioning & Identity Contract (BANOI / OZKEY / Matter)

> **DRAFT 2026-07-07 — for circulation to the BANOI app team.**
> Defines the single commissioning flow that serves all three target markets
> with one lock SKU, and the identity/security layer that must precede any
> real door. Consumers: **BANOI** (residential commissioner app), the
> **OZKEY Flutter app** (commercial commissioner + front end), **lock
> firmware** (ESP32-C6), **OZKEYSERV / OZLOCK cloud** (same codebase, two
> deployments), and **LockSim** (browser test bench). Supersedes ozkey-03
> §1–§4 for real hardware; ozkey-03 remains valid as the *LockSim transport
> variant* (see §2.2). Network contract after commissioning remains ozkey-02
> as amended by §9 here.

---

## 1. The three markets — one lock, three personalities

| | **A — Residential, no hub** | **B — Residential, has hub** | **C — Commercial** |
|---|---|---|---|
| Commissioner | **BANOI** | Apple Home / Google Home / Alexa | **OZKEY app** |
| Post-setup transport | Wi-Fi → MQTT → **OZLOCK cloud** | Thread/**Matter** → their hub | Wi-Fi → MQTT → **on-prem OZKEYSERV** |
| Remote access via | OZLOCK cloud (broker relay) | Their hub (AppleTV etc.) | Customer's server / VPN |
| Credential UX | BANOI | Ecosystem app | OZKEY app (PMS-style) |
| Our server sees the lock | yes (multi-tenant cloud) | **never** | yes (single-tenant on-prem) |

Consequences this contract enforces:

- **One hardware SKU: ESP32-C6** (Wi-Fi + BLE + 802.15.4/Thread). Locked
  decision — market B must never require a board respin.
- Markets **A and C run the identical OZKEY protocol**; the *only* difference
  is which broker/gateway the provision payload points at (§5).
- Market **B bypasses this contract entirely** after factory state: the
  ecosystem commissions the lock as a standard Matter Door Lock. Our flow
  only defines how the lock *offers* both paths (§6) and how factory reset
  returns to the dual-offer state.

## 2. Roles

### 2.1 Real hardware (canonical)

| Party | BLE role |
|---|---|
| **Doorlock** (ESP32-C6 firmware) | **Peripheral** — advertises, hosts the GATT service |
| **BANOI / OZKEY app** (Flutter) | **Central** — scans, connects, writes provisioning |

This **un-inverts ozkey-03 §1** (which flipped roles because Web Bluetooth
cannot advertise). ozkey-03's inverted layout stays valid *only* as the
browser-sim variant and is otherwise historical.

### 2.2 Transport equivalence (LockSim clause)

LockSim cannot do BLE (ozkey-03 §10.1) and stays network-native. Therefore:
**the §5 provision payload is transport-independent.** Real hardware receives
it over GATT; LockSim receives the *identical JSON* pasted into its console
or published to its command topic. A payload that provisions a real lock MUST
provision LockSim unmodified. Test benches exercise the payload, not the radio.

### 2.3 Shared commissioner package (deliverable)

BANOI and the OZKEY app MUST share one Flutter package —
**`ozkey_commissioner`** — owning: BLE scan/connect (§4), the §8 secure
session, payload build/validation (§5), and the §7 status stream. Two apps,
one commissioning implementation. BANOI team owns the package repo; OZKEY app
consumes it.

## 3. Device identity

| Layer | Value | Role |
|---|---|---|
| `device_id` | `ozk-` + base32( SHA-256(device_pubkey)[0..9] ) | **The identity.** Stable, unforgeable, topic-safe |
| Device keypair | P-256, generated on first boot, private key in ESP32-C6 eFuse/HMAC block (never leaves the chip) | Proves `device_id`; signs enrollment + (v2) command acks |
| `mac` | Wi-Fi STA MAC, colon form | **Display label only** — shown to humans, never trusted as identity |
| `mac_token` (`OZK-XXXX-…`) | ozkey-02 §3.2 | **Deprecated to lab-only.** Replaced by enrollment token + device key (§6) |

Rationale: MACs are readable and spoofable by anyone in radio range. Matter
independently requires device attestation certificates (DAC), so per-device
keys must exist in the factory story anyway — one provisioning step serves
both markets. The lab bench may keep using bare MACs until firmware exists.

## 4. Advertising & GATT (lock-hosted)

Same UUID family as ozkey-03 §2 — the service UUID is unchanged and remains
the only discovery filter:

```
service   4f5a4b45-5900-0001-0000-6f7a6b657900   (adv name "OZKEY-<last4 of MAC>")
provision 4f5a4b45-5900-0002-0000-6f7a6b657900   write        app → lock  (§5 JSON)
status    4f5a4b45-5900-0003-0000-6f7a6b657900   notify       lock → app  (§7 strings)
```

Identical UUIDs and payloads as ozkey-03 §3, with GATT ownership swapped to
the lock — exactly the mirror that ozkey-03 §3 anticipated. The lock
advertises only while **uncommissioned** or in **pairing-window mode**
(button-triggered, 5-min timeout) — never while operational.

## 5. Provision payload v1 (canonical)

UTF-8 JSON, single atomic write to `provision`. Adopts ozkey-03 §10.2's
broker fields as canonical and adds `mode` + enrollment. **`room_no` never
appears** — locks are room-agnostic; rooms/labels live server-side only.

```json
{
  "v": 1,
  "mode": "ozkey-cloud",
  "ssid": "HomeWifi",
  "password": "wifi-secret",
  "broker_host": "broker.ozlock.io",
  "broker_tcp_port": 8883,
  "broker_ws_port": 443,
  "broker_ws_path": "/mqtt",
  "server_ip": "api.ozlock.io",
  "server_port": 443,
  "site_id": "st_9f2c1a",
  "enrollment_token": "enr_1c9e4d7f2ab0",
  "owner_hint": "banoi:usr_84d2",
  "heartbeat_s": 600
}
```

| Field | Req | Notes |
|---|---|---|
| `v` | ✅ | `1` = this shape, plaintext (lab/bench only). `2` = §8 encrypted envelope — **mandatory for production** |
| `mode` | ✅ | `ozkey-cloud` (market A) \| `ozkey-local` (market C). Matter is not a mode here — it's the other commissioning path (§6) |
| `ssid` / `password` | ✅ | Wi-Fi credentials |
| `broker_host` / `broker_tcp_port` | ✅ | What the lock actually connects to (TLS on 8883 in production). Per ozkey-03 §10.2 |
| `broker_ws_port` / `broker_ws_path` | ○ | Browser-sim transport only; hardware ignores |
| `server_ip` / `server_port` | ✅ | Gateway REST control-plane; health probe + consistency check only. **No credential ever flows here** (ozkey-02 §8.2) |
| `site_id` | ✅ | Tenant on the target server. Cloud: issued at enrollment. Local: the installation's fixed id |
| `enrollment_token` | ✅ | Single-use, short-TTL token the commissioner obtained from the target server (§6). The lock presents it on first broker contact |
| `owner_hint` | ○ | Opaque commissioner-side owner reference, echoed in enrollment for audit |
| `heartbeat_s` | ○ | Timer-wake interval, default 600, floor 5 (already implemented in LockSim System Settings, 2026-07-07) |

## 6. Enrollment & owner binding sequence

The missing identity layer: ozkey-02's pairing binds *lock → room*; consumer
locks need *lock → owner/site*, with rooms/labels as server-side decoration.

```
 BANOI / OZKEY app            Server (OZLOCK cloud | local)          Lock
 ─────────────────            ─────────────────────────────          ────
 1. signed-in user taps "Add lock"
    POST /enroll/begin {owner, site}
                              2. mints enrollment_token
                                 (single-use, 10-min TTL)
 3. BLE scan → connect → (v2: ECDH session §8)
    write §5 payload ────────────────────────────────────────────►
                                                                   4. joins Wi-Fi
                                                                   5. MQTT CONNECT
                                                                      user = device_id
                                                                      (TLS; v2: per-device creds)
                                                                   6. publishes enrollment:
                                                                      {device_id, mac, pubkey,
                                                                       enrollment_token, fw}
                              7. verifies token, binds
                                 device_id → site/owner,
                                 issues per-device broker ACL
                              8. enrollment_ack ─────────────────► persists; token burned
 9. app polls /enroll/status
    → "lock online" → done
```

- **Ownership transfer / resale:** owner triggers *release* in-app → server
  unbinds and revokes broker credentials → lock factory-resets to
  dual-advertise state (§6.1). No transfer without the current owner's
  session — same authorization shape as FTPOS XF-41 owner-bound pairing.
- **Market C:** "owner" = the site's admin account; after enrollment the
  OZKEY app assigns the lock to a room **in the server UI only** — the lock
  is never told (room-agnostic principle).

### 6.1 Factory state & the Matter fork (market B)

Factory-fresh (or factory-reset) lock advertises **both** commissioning
paths: the OZKEY service (§4) *and* the Matter commissioning advertisement
(BLE, with QR/NFC onboarding payload). Whichever commissioner completes
first sets the personality; the other path stops advertising. Factory reset
(hardware button, 10 s hold) revokes local state and returns to dual-offer —
this is also the market-A→B upgrade path when a customer later buys a hub.
Matter certification (CSA membership, per-product cert, DAC provisioning at
factory) is a budgeted market-B workstream, out of scope for this doc.

## 7. Status ladder (lock → app, `status` notify)

Extends ozkey-03 §5; `SERVER_OK` is replaced by broker-first reality:

`BLE_OK → (v2: SESSION_OK) → WIFI_JOINING → WIFI_OK → BROKER_JOINING →
BROKER_OK → ENROLLED`

Terminal errors: `WIFI_FAIL`, `BROKER_FAIL`, `ENROLL_FAIL` (token expired /
site unknown / signature rejected) — app shows retry, lock returns to
advertising within the pairing window.

## 8. Security envelope v2 — blocking gate for real doors

v1 (plaintext GATT) is **bench-only**. No production lock ships without:

1. **PASE-style ECDH session** over GATT before `provision` accepts a write
   (numeric comparison or QR-carried setup code, Matter-pattern), so Wi-Fi
   and enrollment secrets never travel plaintext. This is the `v:2` envelope
   reserved since ozkey-03 §7.
2. **Per-device broker credentials + ACLs**, issued at enrollment (§6 step
   7): lock `X` can publish/subscribe only `ozkey/<site>/locks/X/#`. One
   compromised lock must not reach another's topics. (Mosquitto ACL file or
   EMQX auth hook — deployment detail, requirement is the contract.)
3. **Signed command frames**: DPID envelopes carry a server signature +
   monotonic counter; the lock verifies before acting, so the broker/relay
   is untrusted infrastructure. Replay of a captured `payload_hex` must fail.
4. **TLS everywhere** off-LAN: MQTT 8883, REST 443.

These four are a **single workstream with a named owner**, scheduled before
first hardware install — not a hardening pass after.

## 9. Topic scheme v2 (de-hotel'd) — amendment to ozkey-02

Locks are identified by `device_id` (lab interim: MAC, no colons, lowercase);
rooms exist only server-side. Target scheme:

| Topic | Dir | Replaces (ozkey-02) |
|---|---|---|
| `ozkey/<site>/locks/unclaimed` | lock→srv | `hotel/locks/unpaired/heartbeat` |
| `ozkey/<site>/locks/<id>/command` | srv→lock | `hotel/rooms/<room_no>/lock/command` |
| `ozkey/<site>/locks/<id>/heartbeat` | lock→srv | `hotel/rooms/<room_no>/lock/heartbeat` |
| `ozkey/<site>/locks/<id>/log` | lock→srv | `hotel/locks/<mac>/log` — **already MAC-scoped, added 2026-07-07**; only gains the site prefix |
| `ozkey/<site>/locks/<id>/enroll` | lock→srv | new (§6 step 6) |

Envelope payloads (`msg_id`, `action`, `payload_hex`, DPID frame codec)
are **unchanged** — ozkey-02 §4 and the byte-verified Tuya DP_REPORT builders
carry over as-is. Lab migration order: refactor `ozkeyserv` + LockSim to
MAC-scoped topics first (drop `room_no` from all lock-facing payloads), add
the `<site>` prefix when multi-tenancy lands. Single-tenant lab uses
`site = "lab"`.

## 10. Impacts on components

- **Lock firmware (new):** implements §4 GATT, §5 parser, §6 enrollment, §7
  ladder; dual-advertise + factory reset per §6.1; v2 session before GA.
- **BANOI:** consumes `ozkey_commissioner`; owns §6 begin/status UX; adds
  remote-unlock with biometric confirm (voice alone is not identity — IVR
  may *initiate*, never *authorize*).
- **OZKEY app (Flutter):** same package; adds site-admin flows (enroll →
  assign room in server UI). Replaces the Next.js cockpit for customers;
  cockpit stays as the internal lab/diagnostic bench.
- **OZKEYSERV / OZLOCK:** one codebase. Adds: `/enroll/*` endpoints,
  owner/site tables, REST auth (currently none — LAN-lab artifact), per-device
  broker ACL issuance, §9 topics. Existing pair/issue/revoke/log logic
  survives with `room_no` keys swapped for `device_id` lookups.
- **LockSim:** §2.2 transport clause — gains a "§5 payload" console entry
  point (paste or MQTT), enrollment simulation, §9 topics when the server
  migrates. BLE stays out of scope per ozkey-03 §10.1.

## 11. Conformance checklist

1. One §5 JSON provisions LockSim (pasted) and real hardware (GATT) with no
   field changes — test with non-default ports to catch hardcoding.
2. `mode: ozkey-local` vs `ozkey-cloud` differ **only** in
   broker/server/site values; both reach ENROLLED against their respective
   servers.
3. Enrollment token is single-use: replaying step 6 after `enrollment_ack`
   is rejected and logged.
4. A second lock's broker credentials cannot subscribe to the first lock's
   `command` topic (ACL test).
5. Factory reset → both commissioning advertisements visible; Matter
   commissioner and OZKEY commissioner are each able to claim it.
6. Owner release → old owner's app loses control, lock re-enrollable by a
   new owner; revoked broker creds rejected at CONNECT.
7. Room re-assignment (market C) touches zero lock state — verified by
   re-assigning while the lock is asleep and observing next-heartbeat flush
   only.

## 12. Open questions for the BANOI team response

1. §2.3 package split — agree BANOI owns `ozkey_commissioner`, or prefer a
   neutral repo both apps consume?
2. §5 `owner_hint` format — what BANOI account reference is stable enough to
   audit against (user id vs signed claim)?
3. §6 step 1 — does BANOI want enrollment to work fully offline against a
   market-C local server (installer with no internet), or is cloud-assisted
   enrollment acceptable for v1?
4. §8 item 1 — numeric-comparison pairing vs QR setup code on the lock body:
   BANOI UX preference? (QR requires a label per unit at factory.)
5. `heartbeat_s` floor/default for battery targets — firmware team input
   needed once hardware exists; sim floor is 5 s for bench speed.
