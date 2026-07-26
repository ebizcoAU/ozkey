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
  "pan_id": "<4 hex chars>"
}
```

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