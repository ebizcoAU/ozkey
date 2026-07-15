# blelock BLE GATT contract — OZ commissioning (v0, ozkey-08 §10)

The profile MAOI/BANOI's `OzkeyBleTransport` and this firmware both build
against (XFtposDecisions-43 §7.5 / ozkey-08 §10; payload/status logic = the
shared `ozkey_commissioner` Dart package, firmware implements the mirror
image).

> **v0 amendments (operator directive 2026-07-16, ozkey-08 §10.3):**
> advertised name is plain **`OZLOCK`** (supersedes `OZKEY-<last4>` below);
> v0 targets **mode 3** (`mode=ozkey-cloud`, ozlockserv :4200, site `lab`,
> terminal status **ENROLLED**); payload gains optional **`name`** (doorlock
> display name) and `info` reports it back; hotel `mode=ozkey-local` (below)
> becomes v1 — same firmware, different payload.

## Advertising

- Name: **`OZLOCK`** (v0; ~~`OZKEY-<last4 of device_id>`~~ superseded)
- Advertises the service UUID below; connectable while UNCOMMISSIONED or in an
  operator-opened pairing window. Stops advertising once provisioned (re-opened
  by factory-reset / re-provision gesture).

## Service

`4f5a4b31-0001-4c4f-434b-000000000001`  (ASCII motif "OZK1…LOCK")

| Characteristic | UUID (…0002/3/4) | Props | Payload |
|---|---|---|---|
| `provision` | `…0002` | write | `ProvisionPayload` JSON — v1 plaintext (bench); v2 = `OzkeyEnvelope`-sealed bytes, same characteristic |
| `status`    | `…0003` | notify | `OzkeyStatus` wire strings: `BLE_OK`, `WIFI_JOINING`, `WIFI_OK`, `BROKER_JOINING`, `BROKER_OK`, `WIFI_FAIL`, `BROKER_FAIL` |
| `info`      | `…0004` | read | JSON `{"device_id":"ozk-…","fw":"blelock-0.1","mac":"AA:BB:…"}` |

## Provision payload (mode=ozkey-local — the hotel case)

```json
{
  "v": 1,
  "mode": "ozkey-local",
  "ssid": "…", "password": "…",
  "broker_host": "10.1.1.21", "broker_tcp_port": 1883,
  "server_ip": "10.1.1.21", "server_port": 3200,
  "device_id": "ozk-<machex>",
  "site_id": "hotel",
  "heartbeat_s": 60
}
```

Validation rules = `ozkey_commissioner/lib/src/provision_payload.dart`
(authoritative). Unknown fields ignored (forward compat).

## Sequence (5-phase, XF-43 §7.5)

```
0. app connects, reads info, subscribes status        → lock notifies BLE_OK
1. app writes provision JSON
2. lock joins Wi-Fi (WIFI_JOINING → WIFI_OK)
   lock dials MQTT broker (BROKER_JOINING → BROKER_OK)   ← terminal success for
   BLE stays up through both (C6 coex) — closed loop      mode=ozkey-local
3. lock announces on hotel/locks/unpaired/heartbeat → MAOI pairs to a room
   (POST /locks/pair → provision_assign; identical to LockSim)
4. operational: heartbeat / DPID credential frames / log
```

Failure: notify `WIFI_FAIL`/`BROKER_FAIL`, stay connectable, accept a
re-written provision (re-provisionable, never one-shot — §7.5).

## Deferred (v2)

- Factory-pubkey trust anchor (QR on screen) + X25519 session → sealed payload
- Matter-takeover semantics (this emulator boots straight into OZKEY mode)
- Admin-PIN keypad menu / battery-compartment factory reset (§7.5 device-side)
