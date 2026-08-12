# ozkey-26 — What the doorlock and bridge ACTUALLY do today: a capabilities reference for app + server

**Written by:** firmware team, 2026-08-12, from a full end-to-end bench run
(bridge rego → lock rego ×2 → door open → datetime → grant → revoke → wipe).
**Audience: server team and app team (ftpos).**

**Status: REFERENCE. Every line below was observed on hardware this session, or
read out of the shipping source and cited.** Where something is inferred rather
than observed it says so. This supersedes assumptions, not other docs' designs.

Firmware at time of writing: **`doorlock-1.58`**, **`bridge32-1.34`**.

---

## 0. Why this exists

Three teams have each been building against a mental model of the firmware, and
this session found four places where those models were wrong in ways that cost
real debugging time — a provisioning field's radix, a topic that structurally
cannot carry door events, a flag that is not evidence, and a lock state that is
indistinguishable from a dead board. None of those were bugs in anyone's code.
They were **contract gaps**. This closes them.

Read §1 and §2 if you integrate with the lock, §3 if you integrate with the
bridge, and **§5 if you read nothing else** — that is the list of things that
have actually bitten.

---

## 1. Doorlock — `doorlock-1.57`

### 1.1 Identity

| Fact | Value |
|---|---|
| `device_id` | `ozk-<mac12hex>`, e.g. `ozk-acebe639f8c4` |
| Identity keypair | X25519, generated on first boot, **regenerated on factory reset** |
| `app_id` | **IS** the app's X25519 public key, hex — not a separate identifier (`ozcrypto.h:414`) |
| Bond #0 | the owner. Created at provisioning from the `app_id` in the payload |
| Bond slots | 0–15; slot 0 admin, 1–15 members |

**Consequence for server:** you already store the app's public key — it is the
`app_id` column. See `ozkey-24 §9.2/§9.3`.

### 1.2 BLE discoverability — the rule changed in 1.57

**A provisioned lock is DARK.** It does not advertise except inside a 60 s
window (`BLE_WINDOW_MS`), opened by:

| Trigger | Available on production hardware? |
|---|---|
| 2 minutes after boot | **only while UNPROVISIONED** |
| Short BOOT press | ❌ bench only — production locks have no BOOT button |
| **DP 8 `ACCESS_RESULT` = denied/expired** | ✅ the real field gesture |
| DP 60 (proposed keypad gesture) | ❌ never allocated by the manufacturer |

🔴 **Changed in 1.57 (XF-96):** a **successful** unlock no longer opens the
window. Only a **failed** entry does. This was a privacy fix — on 1.56,
"advertising" meant "someone entered here in the last 60 s", readable passively
from the street with a stable per-lock ID in the scan response.

**App-facing consequence:** instructional copy must say *"enter any wrong code
at the keypad"*. "Press any key" is now actively wrong — keys that end in a
successful unlock leave the lock dark and look like broken pairing.

### 1.3 What the advertisement carries

- **ADV**: flags + 128-bit service UUID + local name `"OZLOCK"` (identical on
  every unit — 29 of 31 bytes, full)
- **Scan response** (requires an ACTIVE scan): `busy(1B)` + **4 bytes of MAC**
  — the same 8 hex chars the panel prints. This is how you tell three locks
  apart before connecting (XF-94).

### 1.4 GATT

```
SVC   4f5a4b31-0001-4c4f-434b-000000000001
PROV  …0002  (WRITE)   provisioning JSON, chunked; first chunk must start '{'
STAT  …0003  (NOTIFY)  status ladder
INFO  …0004  (READ)    {device_id, mac, fw, name, pub, transport}
CTL   …0006  (WRITE)   sealed control — utf8(app_id_hex,64) ‖ envelope
```

`INFO.transport` reads `"wifi"` on a never-provisioned board (NVS default) and
`"thread"` once provisioned onto a mesh. **`name:""` + `transport:"wifi"` on a
board that was a Thread lock is the signature of a completed factory reset** —
this is currently the only reliable way to confirm a wipe (see §5.4).

### 1.5 🔴 Provisioning payload — field types matter

```json
{ "app_id":"<64 hex>", "mode":"ozkey-cloud", "site_id":"lab",
  "broker_host":"10.1.1.20", "broker_tcp_port":1883,
  "network_key":"<32 hex>", "network_name":"OZ-A7E638",
  "ext_pan_id":"<16 hex>", "channel":15,
  "pan_id":"1274" }
```

🔴 **`pan_id` MUST be a 4-character HEX STRING, not an integer.** The firmware
validates `panHex.length() != 4` reading it as a string
(`ozdoorlock_core.h:3148,3157`); an integer coerces to `""` through
ArduinoJson's string default and the whole payload is rejected as *"malformed
Thread dataset"* — surfaced to the caller as the generic **`ENROLL_FAIL`**,
which names the wrong stage entirely and sends you hunting an enrollment
problem that does not exist. This cost a debugging cycle this session and
`ozprov.py` had been getting it wrong.

`channel` is an **integer**. `network_key`/`ext_pan_id` are lowercase hex
strings. The bridge's own INFO already publishes `pan_id` in the correct form —
**copy it verbatim, do not parse and re-emit it.**

**Status ladder on success:** `BOND_OK` → `THREAD_JOINING`.
**On failure:** `ENROLL_FAIL` (covers malformed dataset, missing SSID, bad
mode — it is not specific).

### 1.6 Sealed control — the envelope

```
control write = utf8(app_id_hex, 64) ‖ envelope
envelope      = ver(1B=0x02) ‖ counter(8B BE) ‖ nonce(12B) ‖ ciphertext ‖ tag(16B)
AAD           = ver ‖ counter ‖ utf8(device_id)
key           = HKDF-SHA256(ikm = X25519(app_priv, lock_pub),
                            salt = utf8(device_id) ‖ utf8(app_id_hex),
                            info = "ozkey/app->lock" | "ozkey/lock->app")
```

AES-256-GCM. `device_id` in the AAD means an envelope for LockA is
cryptographically invalid at LockB. A **persisted per-bond counter floor** in
NVS kills replay across reboots. Two directional keys, so a captured app→lock
frame cannot be replayed back.

**The server never holds a key that opens these.** It relays `envelope_hex`.

### 1.7 OZKIE verbs — the normative contract

The lock receives **sealed JSON** and builds the DP frame itself
(`ozdoorlock_core.h:3826`). Server and app align to the lock, not the reverse.

| Verb | Bond | → DP |
|---|---|---|
| `{"kind":"unlock"}` | any | DP 1 (BOOL) |
| `{"kind":"grant_pin","slot":N,"cred":hex,"from":ts,"to":ts}` | **#0 only** | DP 21 RAW |
| `{"kind":"grant_rfid",…}` | **#0 only** | DP 23 RAW |
| `{"kind":"delete_pin","slot":N}` | **#0 only** | DP 22 RAW |
| `{"kind":"delete_rfid","slot":N}` | **#0 only** | DP 24 RAW |
| `{"kind":"bond_revoke","pub":hex64}` | **#0 only** | in-lock, never forwarded |
| `{"kind":"invite_cancel","nonce":hex32}` | **#0 only** | in-lock |
| `{"kind":"list_bonds"}` | **#0 only** | in-lock |
| `{"kind":"factory_reset"}` / `{"kind":"unpair"}` | **#0 only** | wipes + reboots |

An unrecognised `kind` is **rejected, never guessed at, never forwarded**.

DP 21/23 RAW value layout: `slot(2 BE) ‖ credential ‖ from(4 BE) ‖ to(4 BE)`.
**So the access window is already inside the seal** — the server's
`date_from`/`date_to` columns are a redundant plaintext second copy
(`ozkey-23 §8.1`).

### 1.8 🔴 A Thread lock has NO direct MQTT connection

This surprises people, so it gets its own heading.

`publishLog()` is gated on `mqtt.connected()`. **On a Thread lock that is never
true.** Therefore:

- **Door events never appear on `locks/<id>/log` for a Thread lock.** Not
  "sometimes" — structurally never. Do not build UI or tests that wait for one.
- The heartbeats you *do* see on `locks/<id>/heartbeat` are **presence beacons
  relayed by the bridge** (ozkey-20 R3), not the lock talking to the broker.
- A **Wi-Fi-direct** lock does connect to MQTT and does publish `log` in
  plaintext — that is the C7 exposure, and it is tier-specific (see
  `ozlock.md` C7).

### 1.9 Heartbeat (relayed) fields

```json
{"from":"ozk-…","ext":"<thread ext addr>","kind":"presence","fw":"doorlock-1.57",
 "roster_epoch":0,"bonds":1,"mcu_link_up":true,"uptime_s":720}
```

`bonds` is the live bond count — useful for confirming a revoke landed.
`roster_epoch` increments on roster change (the pull half of ozkey-20 R4).
`mcu_link_up` says whether a real DL MCU is answering on the UART.
**`uptime_s` not resetting is proof a factory reset did NOT happen.**

### 1.10 Broker credentials — new in 1.57

The lock now **presents** `broker_username`/`broker_secret` from NVS on MQTT
connect, falling back to anonymous when they are empty. Before 1.57 it stored
them at enrollment and connected anonymously anyway (`ozkey-23 §10.2a`).

### 1.11 Time

The lock serves **UTC** to the DL MCU on request (`0x1C GET_LOCAL_TIME`).
Timezone offset is **panel-only** and deliberately not applied to what the MCU
receives (`ozkey-21 §8.4`) — **do not "fix" this.** Whether the MCU self-expires
credentials on the `from`/`to` window is an open question with the manufacturer
(`ozkey-21 §8.3`), not with app or server.

---

## 2. Doorlock — what it does NOT do

- ❌ No delivery ACK for any command. There is no transport-level confirmation
  anywhere in this system (`ozkey-19`).
- ❌ No door events on MQTT when on Thread (§1.8).
- ❌ No enrollment handshake over Thread — `enrolled` stays false by design.
- ❌ Does not enforce the **PIN/RFID** credential window itself; it hands
  `from`/`to` to the MCU in the DP frame. (**Membership/bond expiry IS enforced
  as of 1.58 — see §1.12.**)
- ❌ Does not advertise on a successful unlock (1.57, deliberate).

---

## 3. Bridge — `bridge32-1.34`

### 3.1 Role

Thread **leader / border router** + MQTT uplink. Relays sealed envelopes from
`bridges/<id>/command` to the target lock over Thread. **It cannot open them.**

### 3.2 GATT

```
SVC   4f5a4b32-0001-4272-6467-000000000001
INFO  …0004  (READ)
```

INFO exposes the **operational Thread dataset**, which is how a lock gets
provisioned onto the same mesh:

```json
{"device_id":"ozb-98a316a7e638","fw":"bridge32-1.34","transport":"bridge",
 "mode":"mqtt-uplink","thread_role":"Leader","wifi_connected":true,
 "thread_formed":true,"broker_connected":true,"last_status":"BLE_OK",
 "network_name":"OZ-A7E638","ext_pan_id":"c63662989bda3675",
 "network_key":"b4a54888a27c69a8f13a89a9dbabd3a9","channel":15,"pan_id":"1274"}
```

🔴 **`pan_id` here is a HEX STRING.** `"1274"` means **0x1274**, not 1274
decimal. Misreading it puts locks on a different PAN with the same network key
and channel — they then form their **own partition and each declare themselves
Leader**, which looks exactly like "two leaders" and nothing joins. Cost a cycle
this session. **Pass it through verbatim.**

Dataset fields are only present once `thread_formed` is true.

### 3.3 Presence — retained, and it is the liveness state of record

| Payload | Retained? | Meaning |
|---|---|---|
| `{"state":"online","id":…,"role":"bridge"}` | ✅ | up |
| `{"state":"offline","reason":"lwt"}` | ✅ | LWT — connection lost |
| `{"state":"offline","reason":"factory_reset"}` | ✅ | **published immediately before wiping** |
| `{"state":"online",…,"reason":"factory_reset_denied"}` | ❌ **not retained** | refused a reset (**new in 1.34**) |

The denial is deliberately **not** retained: this topic's retained value is
liveness, and retaining an event would make every later subscriber read a
one-off refusal as the bridge's current condition (`ozkey-25 §5.2`).

### 3.4 Ownership and remote reset

`bridgeOwnershipCheck()` governs both BLE and MQTT resets:

| State | Result |
|---|---|
| Owned, `app_id` matches | ✅ accept → publish `factory_reset` → wipe + reboot |
| Owned, `app_id` differs | ❌ `BRIDGE_DENIED` → publish `factory_reset_denied`, **bridge survives** |
| Unowned, claim window **open** | ⚠️ **claims ownership and proceeds** |
| Unowned, claim window closed | ❌ `BRIDGE_CLAIM_REQUIRED`, nothing published |

🔴 **The claim window opens ONLY on a physical short BOOT press and lasts 60 s.**
It does not open on boot. An unowned bridge sitting on a bench is therefore
safe from a remote claim — but note the third row: a reset sent to an unowned
bridge during a claim window will **take ownership and then wipe it**.

### 3.5 Liveness

```json
{"kind":"thread_liveness","bridge_id":"ozb-…","role":"leader","authoritative":true,
 "children":2,"locks":[{"id":"ozk-…","ext":"…","age_s":28,"rssi":-41,"lqi":3,
                        "rx_on":true,"state":"child"}]}
```

This is the authoritative view of who is on the mesh. `children:0` with locks
you believe are provisioned means they are **on a different PAN** (§3.2).

### 3.6 🔴 The bridge has NO broker credentials

`bridge32` contains no `buser`/`bsecret` handling and the server historically
minted them for locks only. Server work has since landed
(`ozkey-23 §10.2a`); **firmware will present them once the delivery path is
live — not yet built on our side.** Until then a bridge cannot authenticate to
the broker at all, so **enabling broker ACLs would drop every bridge**
(`ozkey-24 §7` sequencing).

---

## 4. Verified end-to-end paths (2026-08-12 bench)

| Path | Status |
|---|---|
| Bridge rego via BANOI → owned, leader, broker up | ✅ |
| Lock provisioning over BLE → `BOND_OK` → joins mesh as child | ✅ |
| **Remote sealed unlock** app→server→bridge→Thread→lock→MCU→bolt | ✅ observed |
| Sealed grant (temp PIN, slot+window) | ✅ delivered; `raw_value` never stored server-side |
| Sealed grant delete (DP 22) | ✅ |
| **Sealed remote factory reset**, bond-0 gated | ✅ both locks wiped, left the mesh |
| Remote bridge decommission, no BLE | ✅ `verdict: reset_confirmed` |
| **Bridge reset refused on wrong `app_id`** | ✅ `verdict: reset_denied`, bridge survived |
| UTC push to bridge on connect | ✅ |

---

## 5. 🔴 The five things that have actually bitten

**Read this section even if you skip everything else.**

### 5.1 `pan_id` is a hex string, in both directions

§1.5 and §3.2. Wrong radix → silent partition, or `ENROLL_FAIL` naming the
wrong stage.

### 5.2 `likely_delivered` is not delivery evidence

It reports **presence freshness at the moment of the call**, never delivery —
this system has no ACK anywhere. In one session it returned:

- `false` on two resets that **had provably executed** (verified by BLE INFO)
- `true` on the same operation minutes later
- `false` correctly, on a reset that genuinely did not land
- `null` on an unknown-presence Thread lock

Same flag, opposite truths. It is now three-valued (`true`/`false`/`null`) —
**treat `false` as "we don't know", never as "it didn't happen"**
(`XF-92 §11-13`).

### 5.3 An ECO-profile Thread lock is "offline" most of the time

It sleeps between heartbeats, so presence — and anything derived from it —
reads offline for most of any given minute. This is normal, not a fault.

### 5.4 A wiped lock is indistinguishable from a dead one, passively

Gesture-gating (§1.2) means an unprovisioned lock is dark: no MQTT, no
advertising outside its grace. So "silent everywhere" fits **both** a clean wipe
and a brick. **Quiet is not evidence, in either direction.**

The only reliable confirmation is an **active probe**: open a window (reset the
board, or a denied keypad entry), connect, read INFO, and check for `name:""` +
`transport:"wifi"` (§1.4). Do not build UI that infers wipe success from a lock
going quiet.

### 5.5 A sealed `envelope_hex` is REQUIRED to reset a Thread lock

`DELETE /locks/:id` without one falls back to publishing on the **lock's own
MQTT topic** — which a Thread lock never reads (§1.8). The call returns
`ok:true` and the lock keeps running. Verified this session: the unsealed
attempt left both locks bonded with uptime unbroken; the sealed
`{"kind":"factory_reset"}` wiped both.

---

## 6. Open, and who owns it

| Item | Owner |
|---|---|
| DP 60 allocation for a deliberate keypad gesture | manufacturer — **no longer blocking**, 1.57 solved the privacy need |
| Does the DL MCU self-expire on `from`/`to`? (`ozkey-21 §8.3`) | manufacturer |
| Bridge broker credentials — firmware side | **firmware**, once server's delivery path is live |
| `date_to` UTC vs `NOW()` local (`ozkey-23 §11`) | **server** |
| REST auth, then app broker JWT (`ozkey-24`) | **server**, then app |
| Does the app read `user_name` back? (`XF-95 §5.1`) | **app** |
| Seal or drop the `log` topic per tier (`ozlock.md` C7) | firmware, queued |

---

## 1.12 🔴 UPDATE `doorlock-1.58` — membership expiry is now ENFORCED

Added 2026-08-12, hours after this document was first written. **It changes a
behaviour app and server may have been relying on.**

### What changed

A member bond now carries `expiresAt`, set from the invite's **signed** `me`
(v2, XF-87). When it comes due the lock **deletes the bond outright** — it does
not park it, flag it, or downgrade it. Full detail and the test matrix in
`ozkey-21 §10`.

Before `1.58`, `me` was verified and then discarded. `ozkey-21 §9` recorded the
consequence on real hardware: a membership that expired at 12:38 opened the
door at 12:39. On `1.58` the same ceremony refuses at 13:06.

### What app and server should expect

- **`bonds` in the heartbeat will drop on its own**, with `roster_epoch`
  incrementing, **with nobody at the door**. Any code assuming the roster only
  changes in response to a command is now wrong. The epoch is the convergence
  signal — use it.
- **`roster_changed` fires with `reason: "bond_expired"`.** Treat it exactly
  like `bond_revoked`; an expired bond is deliberately indistinguishable from a
  revoked one at every layer above the lock.
- **A member whose window has passed is denied at the door**, not merely absent
  from a list. Expiry is checked on the command path as well as on a 15 s
  timer, so there is no "we would have removed you shortly" gap.
- **An already-expired invite is refused at enrolment** with
  `MEMBER_EXPIRED` — a status that was reserved and never emitted before. Apps
  should render it distinctly from `MEMBER_FAIL`: it means *"this QR was valid,
  you are too late"*, which is actionable (ask for a new invite), whereas
  `MEMBER_FAIL` means the invite was never authentic.
- **`me = 0` (or a v1 invite) means PERMANENT and is never swept.** Verified
  against a permanent and an expiring bond in the same table simultaneously.

### One thing that did NOT change

**Temp PIN / RFID expiry is unaffected.** That credential lives on the DL MCU;
we pass `from`/`to` inside DP 21/23 and whether the MCU self-expires is still
open with the manufacturer (`ozkey-21 §8.3`). **Bond membership is entirely
ESP32 and required nothing from the MCU or the supplier** — the MCU never
learns which bond authorised an unlock, so it could not enforce membership
expiry even in principle.

### Field-upgrade note for anyone reading this before an OTA

`1.58` grows the on-NVS bond record 80 → 88 bytes. `ozBondsLoad()` reads both
strides and migrates once on boot. This is called out because the naive version
of that change would have made every deployed lock come up **with no owner and
no members** — see `ozkey-21 §10.2`. Verified on hardware: the owner bond
survived the flash.
