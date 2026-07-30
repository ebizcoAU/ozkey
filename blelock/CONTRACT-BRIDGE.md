# OZBRIDGE BLE GATT contract — Thread border router bootstrap (v0)

Companion to `blelock/CONTRACT.md` (the lock's contract). This file covers the
**bridge** (`blelock/bridge32/`) and the Thread-transport variant of the lock
(`blelock/threadcomm/`) — the two halves of ozkey-08 §0's Mode 2 residential
chain: `threadcomm (lock, Thread) → bridge32 (Wi-Fi/Thread border router) →
MQTT broker → ozlockserv`.

Design locked 2026-07-23 (operator + FtposPM discussion): **Option B — direct
dataset handoff.** BANOI is the courier for both devices' BLE provisioning; no
over-the-air Thread joiner/commissioner protocol in v1 (the installed Arduino
core's OpenThread wrapper exposes commit-a-dataset as a typed call, but only
exposes joiner/commissioner via a text CLI console — rougher to build against
for a first bring-up. Revisit post-v1 once the basic loop is proven).

**Updated 2026-07-25 (PM decision):** Bridge provision payload now includes a
`mode` field to select Personality A (Matter bridge) or B (MQTT uplink).
The app shall send this field at provisioning time. Firmware (bridge32.ino)
shall parse and store it.

## Device roles

| Device | Radio | Job |
|---|---|---|
| **bridge32** | Wi-Fi + Thread (802.15.4) | Forms the Thread network, holds the operational dataset, bridges it to Wi-Fi/MQTT. One per home. |
| **threadcomm** | Thread only (BLE for commissioning) | The lock's comm module. Joins the dataset bridge32 hands it. Many per home. |

Neither device "finds" the other over the air in v1. BANOI already holds a
BLE-authenticated session with the bridge (from provisioning it) and reads the
operational dataset from it; when a new lock is added, BANOI writes that same
dataset into the lock over BLE. The Thread radio only carries traffic **after**
both ends already agree on the network — no discovery step happens on 802.15.4.

## bridge32 — GATT service

`4f5a4b32-0001-4272-6467-000000000001` (ASCII motif "…Brdg…")

- Advertised name: **`OZBRIDGE`** — distinct from the lock's `OZLOCK` so a BLE
  scan for "add a bridge" never lists a lock, and vice versa (app filters by
  advertised name per intent, not by scanning everything and guessing).

| Characteristic | UUID | Props | Payload |
|---|---|---|---|
| `provision` | `…0002` | write | JSON – see below. Chunked like the lock's provision char (buffer resets on `{`) |
| `status` | `…0003` | notify | `BLE_OK`, `WIFI_JOINING`, `WIFI_OK`, `WIFI_FAIL`, `THREAD_FORMING`, `THREAD_OK`, `THREAD_FAIL`, `BROKER_JOINING`, `BROKER_OK`, `BROKER_FAIL`, `PAYLOAD_REJECTED` |
| `info` | `…0004` | read | JSON, see below |

### Provision payload (updated 2026-07-25)

```json
{
  "v": 1,
  "mode": "mqtt-uplink",              // REQUIRED – "mqtt-uplink" or "matter-bridge"
  "device_id": "b-98a316a7e638",      // echo of info.device_id
  "ssid": "HomeWifi",
  "password": "wifi-secret",
  "broker_host": "10.1.1.20",         // Required for "mqtt-uplink"
  "broker_tcp_port": 1883,            // Required for "mqtt-uplink"
  "site_id": "lab"                    // Required for "mqtt-uplink" – used in MQTT topics
}
```

Mode values:

- `"mqtt-uplink"` – bridge connects to Wi-Fi, MQTT broker; subscribes to `ozkey/<site>/locks/<bridge>/command`.
- `"matter-bridge"` – bridge starts Matter-over-Wi-Fi stack (stubbed in v0; only stores the mode).

If `mode` is missing or invalid, bridge rejects provision with `ERR_PAYLOAD` and notifies `PAYLOAD_REJECTED`.

`info` payload:

```json
{
  "device_id": "ozb-<machex>",
  "mac": "AA:BB:...",
  "fw": "bridge32-0.1",
  "transport": "bridge",
  "mode": "mqtt-uplink",              // stored personality
  "thread_role": "leader",
  "network_name": "OZ-a1b2",
  "ext_pan_id": "<16 hex chars>",
  "network_key": "<32 hex chars>",
  "channel": 15,
  "pan_id": "<4 hex chars>"
}
```

The `network_*`/`ext_pan_id`/`channel`/`pan_id` fields are absent until the
bridge has formed a Thread network (i.e. until `THREAD_OK`). This is the
payload BANOI reads and relays verbatim into a threadcomm lock's `provision`
characteristic.

## threadcomm — GATT service

Reuses the lock's existing service and characteristic UUIDs from
`CONTRACT.md` (`4f5a4b31-0001-…`) and the `OZLOCK` advertised name — it is
still "a doorlock" from the app's perspective, just a different transport
underneath. `info.transport` is the discriminator:

```json
{"device_id":"ozk-<machex>","mac":"…","fw":"threadcomm-0.1","transport":"thread","thread_role":"detached"}
```

`provision` accepts a **Thread dataset** instead of Wi-Fi credentials —
distinguished from the Wi-Fi payload by the presence of `network_key` rather
than `ssid`:

```json
{
  "device_id": "ozk-<machex>",
  "network_name": "OZ-a1b2",
  "ext_pan_id": "<16 hex chars>",
  "network_key": "<32 hex chars>",
  "channel": 15,
  "pan_id": "<4 hex chars>",
  "app_id": "<64 hex chars — optional>"
}
```

**[XF-47 §11.5] `app_id` is REQUIRED for a Thread lock to ever hold a bond.**
Bond #0 is created from `provision.app_id` (`CONTRACT.md` §"[XF-47] Bond #0
establishment"). That field originally existed only on the **Wi-Fi**
`ProvisionPayload`, so as first specified a Thread lock could never obtain bond
#0 — and therefore no `member_enroll`, no sealed unlock, and no DPID 101/102.
The entire XF-46 member model would have been Wi-Fi-only, on the platform's
primary topology. Caught by ftpos in XF-47 §11.5; shipped app-side in F2c.

Same key, same meaning, same 64-hex-char member/admin X25519 pubkey as the Wi-Fi
payload. Optional and additive — firmware that ignores it records the lock as
unbonded, and the app falls back to `thread-joined`.

### [XF-47] Bridge ownership guard

> **Touching an unowned bridge requires physical presence. Touching an owned bridge
> requires a matching `app_id`.**

```
bridge provision:
  no owner (app_id present OR absent) → REQUIRES claim window open
                                        open   → apply; if app_id present, claim it
                                        closed → BRIDGE_CLAIM_REQUIRED
  owner == incoming app_id            → idempotent, no window needed
  owner != incoming app_id            → BRIDGE_DENIED, change nothing
  owner exists, app_id ABSENT         → BRIDGE_DENIED, change nothing
```

**Refusal is atomic in every branch** — a rejected provision must not apply the
broker or Wi-Fi fields either, or an attacker who cannot steal the bridge can still
repoint it.

**Claim window:** a short **BOOT** press opens a ~60 s window. A 5 s hold is already
factory reset (`bridge32.ino:108-139`), so the short press is free. Re-provisioning
an already-owned bridge by its owner needs no button, keeping the normal
change-my-Wi-Fi path intact. `owner_app_id` is cleared only by factory reset.

**Why physical presence is required at all:** `bridge32.ino:916-922` keeps BLE
advertising up permanently after provisioning (deliberate, so the app can always
reconnect). Unlike the lock's ~60 s touch window, **a bridge's provisioning window
never closes** — so without the button gate an unowned bridge is reconfigurable by
anyone in BLE range, at any time, with no wait.

Two bypasses were found and closed during XF-47 review, both worth recording
because each made the guard optional at the attacker's discretion:

1. **Omit `app_id` on an owned bridge** — dodged the guard entirely. Closed by the
   `owner exists, app_id ABSENT → BRIDGE_DENIED` clause.
2. **Omit `app_id` on an unowned-but-deployed bridge** — could not claim it, but
   could repoint `broker_host` and move a live mesh's uplink to attacker
   infrastructure, with no physical access. Closed by requiring the claim window for
   *all* unowned-bridge provisions regardless of whether `app_id` is present.

**`BRIDGE_CLAIM_REQUIRED` must be distinct from `BRIDGE_DENIED`.** An unowned bridge
answering `BRIDGE_DENIED` tells the user it has an owner, which is false and
undiagnosable — the same reason `BOND_DENIED` exists rather than reusing
`PAYLOAD_REJECTED`. Both strings are terminal, both settle at progress 2.5/6.

**SCHEDULED FOR REMOVAL after M4:** the `app_id`-absent-and-unowned branch exists
only for ozkey bench tooling that provisions bridges by raw JSON. BANOI sends
`app_id` unconditionally and is the only production producer, so this branch has no
legitimate caller once the bench tooling is updated. It is a temporary
accommodation, not an accepted variant.

**Firmware MUST read `app_id` once, before the `hasNetworkKey` branch**
(`doorlock.ino:~1102`), not separately inside each transport arm. Two reads in
two branches is the same divergence hazard as `ozcrypto.h` existing in two copies
or `hexToBytes` existing twice app-side — both of which produced real bugs in this
project. One read, one meaning, both transports.

New `status` wire strings (outside the existing WIFI_*/BROKER_* ladder, same
pattern as XF-46's MEMBER_*/UNLOCK_* additions in `CONTRACT.md`): `THREAD_OK`,
`THREAD_FAIL`. `THREAD_JOINING` reuses the existing generic "in progress" UX.

## Sequence (v0, Option B)

```
1. Bridge already provisioned + THREAD_OK (one-time, per home).
2. App connects to bridge32, reads info -> has the operational dataset.
3. App connects to threadcomm (new lock), reads info, writes the dataset
   (from step 2) into threadcomm's provision characteristic.
4. threadcomm commits the dataset, attaches to the Thread mesh.
   -> notify THREAD_OK (or THREAD_FAIL on timeout).
5. threadcomm relays Tuya frames to bridge32 over Thread; bridge32 forwards
   to MQTT (this is the next increment — building the Thread-side frame
   transport and MQTT uplink).
```

## Not in this increment

Both sketches compile and prove the BLE-provision → radio-join loop only.
Deliberately deferred, so this lands small and provable:

- **Tuya UART relay on threadcomm.** No MCU wire, no credential frames. The
  lock-MCU relay logic that exists in `blecomm/` moves here once the Thread
  transport itself is proven.
- **Thread-side frame transport** between threadcomm and bridge32 (the actual
  payload that eventually carries Tuya frames over the mesh). This increment
  only proves both ends can join the *same* Thread network — nothing rides
  on top of it yet.
- **Matter-over-Wi-Fi bridging (Personality A / Mode 1b, Apple Home etc.)** —
  a fully separate protocol/SDK (`esp-matter`), not reachable through BANOI at
  all. Tracked in `docs/ozkey-08.md` §0, not started.
- **Over-the-air Thread joiner/commissioner (Option A).** Noted above —
  revisit once Option B's basic loop is bench-proven.

## Toolchain note

Both sketches add `#include <OThread.h>` (Arduino ESP32 core 3.3.10's
OpenThread wrapper, bundled — no separate library install). This is the
**first use of this library in the repo**; whether the precompiled core
libraries actually have `CONFIG_OPENTHREAD_ENABLED` baked in for the ESP32-C6
variant is confirmed by the first successful compile, not by static
inspection (no board-menu toggle exists for it, unlike Zigbee on C5/H2 — see
compile note in the build log).