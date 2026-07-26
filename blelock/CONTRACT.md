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
  "broker_host": "10.1.1.20", "broker_tcp_port": 1883,
  "server_ip": "10.1.1.20", "server_port": 3200,
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

## Operational / member profile — XF-46 (v1 DRAFT 2026-07-20, FtposPM proposal)

> The N-bond multi-user contract (ozkey-08 §0.4 / ftpos XFtposDecisions-46).
> App-side (BANOI) is built against this section; firmware (blelock/blecomm)
> pending — ozkey-team to confirm UUIDs, wire shapes, and the invite-MAC
> realization below, then this becomes canonical.

**Advertising (operational):** touch-window only — an enrolled lock
advertises `OZLOCK` + the service UUID for **~60 s after any keypad/screen
touch**, never while idle (power + no trackable beacon; ozkey-08 §0.4).
Production adds BLE RPA rotation; bench keeps the plain name.

**`info` gains `"pub"`:** the lock's X25519 ceremony public key (lowercase
hex, 64 chars). Needed by the member ceremony (and the future v2 sealed
commissioning) to derive the pairing secret. Lock keypair minted at first
boot, NVS-persisted, survives re-provision, wiped on factory reset.

**New characteristics** (same service `4f5a4b31-0001-…`):

| Characteristic | UUID (…0005/6/7) | Props | Payload |
|---|---|---|---|
| `challenge` | `…0005` | read | 16 random bytes, fresh per read; valid for this connection, ~30 s |
| `control` | `…0006` | write | `utf8(app_id_hex, 64 chars)` ‖ `OzkeyEnvelope` (app→lock, per-bond counter). Envelope **plaintext = challenge(16 B) ‖ DPID frame**. Lock: look up bond by app_id → open envelope (counter > bond floor) → verify challenge == last-issued → execute frame (DP 1 remote-unlock in v1; role-gate admin verbs to bond #0) |
| `member_enroll` | `…0007` | write | plaintext JSON `{"app_id":"<member X25519 pubkey hex>","invite":"OZINV1:…"}` — chunked like `provision` (buffer resets on `{`, parse on JSON-complete). No bond exists yet, so this is unsealed; the INVITE is the authenticator |

**Member-enroll lock-side algorithm:** decode invite (`OZINV1:` +
base64url JSON, fields v/d/i/r/l/n/e/m) → recompute MAC:
`mac_key = HKDF-SHA256(ikm = bond#0 pairing secret, salt = utf8(device_id ‖
issuer_app_id_hex), info = "ozkey/invite-v1")`;
`mac = HMAC-SHA256(mac_key, utf8("1|device_id|issuer|role|label|nonce|expires"))`
(byte-exact vectors: ftpos `packages/ozkey_commissioner/test/
member_invite_test.dart` + `tool/gen_invite_vector.dart`) → nonce unused
(replay cache, suggest 32-entry LRU in NVS; nonce = the HARD guarantee) →
expiry best-effort (clock drift tolerated) → capacity ≤16 bonds → add bond
`{pubkey, role, label}` → pairing secret = X25519(lock_priv, member_pub) →
notify. Lock reports `bond_added` / `bond_revoked` on its log topic at next
sync (OZLOCK builds the door→apps map passively).

**New `status` wire strings:** `MEMBER_OK`, `MEMBER_FAIL`, `MEMBER_FULL`,
`MEMBER_REPLAY`, `MEMBER_EXPIRED` (enroll) · `UNLOCK_OK`, `UNLOCK_DENIED`
(control). These are OUTSIDE the commissioning ladder — apps consume them on
a raw-status stream, `OzkeyStatus.parse` ignores them.

## Deferred (v2)

- Factory-pubkey trust anchor (QR on screen) + X25519 session → sealed payload
- Matter-takeover semantics (this emulator boots straight into OZKEY mode)
- Admin-PIN keypad menu / battery-compartment factory reset (§7.5 device-side)
- Member profile: RPA advertising rotation · second admin / bond #0 transfer
  · member self-remove verb ("rời khỏi cửa này" currently local-only)
