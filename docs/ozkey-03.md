# OZKEY-03 — BLE Commissioning Contract ("OZKEY" Service)

> Standalone contract for **Phase 0** of doorlock onboarding: delivering Wi-Fi
> credentials and the gateway address to a factory-fresh lock over BLE, before
> the lock has any network connection. Consumers: the **OZKEY mobile app**
> team (peripheral side) and **LockSim** (central side, simulated lock).
> Once Phase 0 completes, the network contract in `ozkey-02.md` takes over
> unchanged. Written 2026-07-06.

---

## 1. Roles

| Party | BLE role | Why |
|---|---|---|
| **OZKEY mobile app** (already exists; gains Advertise + GATT server) | **Peripheral** — advertises the OZKEY service, hosts writable/notify characteristics | Browsers cannot advertise |
| **Doorlock** = LockSim in Chrome on the laptop, using the laptop's BLE radio | **Central** — scans for the OZKEY service, connects, receives provisioning data | Web Bluetooth supports central role only |

> ⚠️ **Inverted vs. real hardware.** A production lock advertises and the app
> scans (Matter/Tuya pattern). The simulation flips the roles because Chrome's
> Web Bluetooth API cannot act as a peripheral. The *data contract* (§4) is
> direction-agnostic — do not "fix" the sim to match hardware; only the
> transport roles differ.

## 2. Advertising & discovery

- **Service UUID (128-bit, canonical):**

  ```
  4f5a4b45-5900-0001-0000-6f7a6b657900
  ```

  Derived from ASCII "OZKEY" (`4F 5A 4B 45 59`); lowercase string form for Web
  Bluetooth. This UUID is the single source of truth for filtering.
- **Advertised local name:** `OZKEY` (cosmetic — helps humans in scan pickers;
  never filter on it programmatically).
- The app advertises connectable, general-discoverable, with the service UUID
  in the advertisement (not only the scan response), so Chrome's filter can
  see it.
- LockSim discovery call:

  ```js
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ services: ['4f5a4b45-5900-0001-0000-6f7a6b657900'] }],
  });
  ```

  Note: requires a user gesture and shows Chrome's device chooser — the
  operator clicks the app entry. Silent background scanning is not possible
  in a browser; this is the accepted UX ceiling for the sim.

## 3. GATT layout

One service, two characteristics. Single-JSON-blob design: atomic, no
partial-state risk, fits within a ~185–512-byte ATT MTU.

**Direction note:** the app is the GATT *server* (it hosts the
characteristics), but the provisioning *data flows app → lock*. Hence the
lock **reads** `provision` and **writes** `status`:

| Characteristic | UUID | Props | Flow | Content |
|---|---|---|---|---|
| `provision` | `4f5a4b45-5900-0002-0000-6f7a6b657900` | read + notify | app → lock | the §4 JSON blob; the app sets the value, the lock reads it after connecting (and subscribes for updates if the operator edits) |
| `status` | `4f5a4b45-5900-0003-0000-6f7a6b657900` | write | lock → app | the lock writes §5 status strings so the app can display commissioning progress |

(This is the mirror of the real-hardware layout, where the lock hosts a
write-`provision` / notify-`status` service — same two channels, same
payloads, swapped GATT ownership. Keep payloads identical.)

## 4. Provision payload (app → lock)

UTF-8 JSON, single value on the `provision` characteristic:

```json
{
  "v": 1,
  "ssid": "OZKEY-LAB",
  "password": "labwifi-secret",
  "server_ip": "10.1.1.21",
  "server_port": 3200
}
```

| Field | Req | Notes |
|---|---|---|
| `v` | ✅ | Contract version, integer, currently `1` |
| `ssid` | ✅ | Target Wi-Fi network |
| `password` | ✅ | WPA2 passphrase (plaintext — see §7) |
| `server_ip` | ✅ | OZKEYSERV gateway host (lab: the Mac running ozkeyserv) |
| `server_port` | ✅ | Gateway HTTP port (lab: `3200`). ozkey-02 previously never carried a port — BLE is its origin |

The lock MUST use exactly this address for all subsequent gateway traffic
(`http://<server_ip>:<server_port>/ozkeyserv/api/...`) — no hardcoded
fallbacks once provisioned.

## 5. Status codes (lock → app)

Written by the lock to `status` as plain UTF-8 strings, in order:

| Status | Meaning |
|---|---|
| `BLE_OK` | Connected, provision payload received and parsed |
| `WIFI_JOINING` | Attempting to join the SSID (LockSim: simulated ~1.5 s delay) |
| `WIFI_OK` | On the network |
| `SERVER_OK` | First announce to the gateway succeeded (§6 step 4) — commissioning complete, app may disconnect |
| `WIFI_FAIL` / `SERVER_FAIL` | Terminal errors; app shows retry UI, lock returns to scanning state |

## 6. End-to-end sequence

```
 OZKEY app (peripheral)          LockSim / lock (central)         OZKEYSERV :3200
 ──────────────────────          ────────────────────────         ───────────────
 1. Operator opens "Add Lock"
    → advertise OZKEY service
                                 2. Operator clicks SCAN FOR
                                    OZKEY SETUP → Chrome chooser
                                    → connect GATT
                                 3. read `provision` JSON
                                    status ← BLE_OK
                                    status ← WIFI_JOINING (sim delay)
                                    status ← WIFI_OK
                                 4. announce MAC to gateway
                                    POST http://<ip>:<port>/ozkeyserv
                                         /api/sim/unpaired-heartbeat ──► unpairedCache
                                    status ← SERVER_OK
 5. app shows "Lock online —
    assign a room in the cockpit"
    → disconnect
                                 ══ Phase 0 complete. ozkey-02 applies from here:
                                    §3.2 room handshake → §3.3 heartbeats → §3.4 credentials
```

## 7. Security posture (lab simplification — flagged)

Wi-Fi credentials travel as **plaintext over an unencrypted GATT link**. This
is acceptable for the lab bench only. Production hardware must add a session
layer (Matter PASE / ECDH like Tuya) or at minimum BLE LE Secure Connections
pairing before the `provision` read is allowed. The `v` field exists so a
`v:2` encrypted envelope can be introduced without breaking the sim.

## 8. Impacts on the other components

- **LockSim state machine:**
  `UNPROVISIONED → SCANNING(OZKEY) → BLE_CONNECTED → WIFI_JOINING(sim) →
  WIFI_OK → announce → awaiting room handshake → PAIRED - ROOM X`.
  The existing "Broadcast Hardware MAC ID" action moves to *after* `WIFI_OK`
  and targets the BLE-received `server_ip:server_port`. Persist the §4 payload
  alongside `locksim.provisioning.v1`.
- **ozkey-02 amendments:** `server_ip` in the room-handshake JSON (§3.2 there)
  is demoted from lock-required to **consistency check** — BLE is now the
  source of truth; warn on mismatch. `server_port` should ride along in the
  handshake for the same check.
- **OZKEY mobile app (new work):** advertise the §2 service, host the §3
  characteristics, provision-payload editor (ssid/password prefilled, server
  defaults `10.1.1.21:3200`), live status display from §5.
- **OZKEYSERV:** no changes required for Phase 0 itself — the lock arrives at
  the gateway exactly as ozkey-02 already describes.

## 9. Conformance checklist

1. App advertising → LockSim's Chrome chooser lists a device named `OZKEY`
   filtered by the §2 UUID.
2. Connect → app receives `BLE_OK` within 2 s of connection.
3. Status progresses `WIFI_JOINING → WIFI_OK → SERVER_OK`; total ≤ 5 s in sim.
4. Gateway cockpit shows the lock's MAC in UNPAIRED HW (proves the lock used
   the BLE-delivered address, not a hardcoded one — test with a non-default
   port to catch cheating).
5. Kill the gateway and rerun → lock reports `SERVER_FAIL`, app shows retry,
   lock returns to scanning state cleanly.

---

## 10. LockSim team response (2026-07-06) — BLE deferred + address correction

### 10.1 DISAGREE with the premise for the sim: BLE Phase 0 is deferred in LockSim

Per operator direction ("assume the network is connected"), LockSim **removed**
the BLE Provisioning Mode toggle and the "Broadcast Hardware MAC ID over BLE"
button. LockSim does **not** implement the Web Bluetooth central role, and does
not run the §8 state machine (`SCANNING(OZKEY) → BLE_CONNECTED → WIFI_JOINING →
…`). Do not expect a browser BLE central in the near term.

Instead LockSim is **network-native**: a "Register Doorlock" button publishes the
MAC announce directly over MQTT-over-WS. Current LockSim states:

```
UNLINKED → REGISTERING (announced MAC, awaiting room) → PAIRED - ROOM X
```

Phase 0 as a whole is still valid **for real hardware** (a physical lock
advertises/scans BLE natively) — the mobile-app team can keep building §2–§5.
It just won't be exercised against LockSim.

### 10.2 DISAGREE: `server_port` = gateway :3200 is not what the lock connects to

§4 mandates the lock use `http://<server_ip>:<server_port>/ozkeyserv/api/...`
with `server_port: 3200` for "all subsequent gateway traffic." For a lock that
speaks **MQTT** (LockSim, and real Tuya/ESP hardware), this is wrong: the lock's
traffic goes to the **broker** (WS `:9001` for a browser, TCP `:1883` for
hardware), **not** the gateway HTTP `:3200`. The gateway port is control-plane
only (see ozkey-02 §8.2).

**Recommendation for the BLE provision payload (§4):** when Phase 0 is
reintroduced, carry the **broker** address the lock must connect to, not just the
gateway. Suggested `v:1` additions:

```json
{
  "v": 1,
  "ssid": "OZKEY-LAB",
  "password": "labwifi-secret",
  "broker_host": "10.1.1.21",
  "broker_ws_port": 9001,
  "broker_ws_path": "/mqtt",
  "broker_tcp_port": 1883,
  "server_ip": "10.1.1.21",
  "server_port": 3200
}
```

- `broker_*` = what the lock actually connects to (browser central uses
  `ws://broker_host:broker_ws_port/broker_ws_path`; native hardware uses
  `broker_host:broker_tcp_port`).
- `server_ip`/`server_port` = gateway control-plane, kept for health/REST and as
  the consistency value in the ozkey-02 §3.2 handshake.

### 10.3 AGREE

- The single-JSON-blob GATT design (§3) and the status-code ladder (§5) are fine
  as-is; no changes needed for when BLE returns for real hardware.
- Keeping `v` for a future encrypted envelope (§7) is the right call.
