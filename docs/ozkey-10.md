# OZKEY-10 — Unified Doorlock Comm-Module Firmware (One Binary, Five Modes)

> **SCOPED 2026-07-26 (operator directive) — design only, not built.** Corrects
> ozkey-08 §0's "comm-module split" decision (2026-07-19): that section treats
> `blecomm` (Wi-Fi+BLE) and `threadcomm` (Thread-only) as two separate
> firmware builds, distinguished by `info.transport`. The operator's actual
> intent is stronger: **one firmware image**, and the app selects the
> operating mode entirely through the BLE provisioning payload at
> commissioning time — never by which sketch was flashed. `threadcomm.ino`
> and `bridge32.ino` (both bench-verified 2026-07-25/26, see
> `blelock/MEMORY.md`) proved the underlying Thread-join + UDP-relay
> mechanics work; this doc is the plan to fold that proof into the one real
> product firmware. **Build starts next session** — tonight's session was
> deep in live-hardware debugging (see the crash/race-condition fixes across
> `bridge32.ino`, `threadcomm.ino`, `blecomm.ino`, `blelock.ino` and the
> BANOI app earlier the same day) and this was deliberately deferred rather
> than started mid-fatigue.
>
> Consumers: whoever picks up the firmware thread next (per `blelock/
> MEMORY.md`'s handoff note), BANOI/MAOI app teams (provisioning payload
> shape), ozlockserv/ozkeyserv (no server change implied — see §5).

---

## 1. The five configurations

Restates ozkey-08 §0 / §0.0.0's canonical taxonomy, reframed as exactly what
one firmware image must be told to do at commissioning:

| # | Configuration | Transport | Server | RBAC | Firmware today |
|---|---|---|---|---|---|
| 1 | **Matter** | Thread → ecosystem's own border router (Apple TV/Alexa/Nest) | none of ours | none | **not started** — separate protocol stack (esp-matter), out of scope here (§6) |
| 2a | **OZLOCK-HOME, premium** | Thread → bridge32 → MQTT → ozlockserv | ozlockserv (free relay) | none (app+lock only) | proven in `threadcomm.ino`, not merged |
| 2b | **OZLOCK-HOME, economy** | Wi-Fi direct → MQTT → ozlockserv | ozlockserv (free relay) | none | proven in `blecomm.ino` (`mode=ozkey-cloud`) |
| 3 | **OZKEY** (hotel/motel/campus/mining camp) | Wi-Fi direct → MQTT → ozkeyserv | on-prem or cloud, **3-level RBAC**, AI-generated site/room map | org → operator → tenant | proven in `blecomm.ino` (`mode=ozkey-local`) |
| 4 | **OZPMS** (property management) | Wi-Fi direct → MQTT → ozkeyserv-family, cloud | cloud, **3-level RBAC**, Google Maps-assisted | platform → property manager → tenant | **no firmware difference from #3** — ozkey-07 §14 already settled this: "same wire contract as Mode 3 — the lock only ever knows `broker_host:port`, so hotel-on-LAN vs PMS-in-cloud is a deployment choice, not a firmware or protocol fork" |

So the real firmware gap is narrower than "5 modes" suggests:
- **#3 and #4 need zero new firmware** — already unified under `mode=ozkey-local`, differing only in which `broker_host`/`site_id` the app's provision payload points at. The "3-level RBAC" and "AI room map" / "Google Maps" pieces are entirely server+app concerns (fleet RBAC is ozkey-07 §3/§8, already built server-side; the mapping features are BANOI/MAOI/OZPMS-app UI, not lock firmware).
- **#2a and #2b are one mode (`ozkey-cloud`) with two transports.** The firmware gap is real: `blecomm.ino` only speaks Wi-Fi; the Thread-join + UDP-relay logic proven in `threadcomm.ino`/`bridge32.ino` needs to become an alternate transport path inside the same binary.
- **#1 (Matter) is a separate, larger, later workstream** (§6) — not part of this unification pass.

**The actual unification work is: teach `blecomm.ino` to also speak Thread**, selected by the same payload-shape discriminator `threadcomm.ino` already uses (`network_key` present → Thread dataset; `ssid` present → Wi-Fi credentials) — not a new top-level "mode" value.

## 2. Base sketch decision (operator, 2026-07-26)

**Start from `blecomm.ino`, not `threadcomm.ino`.** It already has the mature
half of the product: keypad UI, DPID codec (issue/revoke/verify), PIN/NVS
storage, MQTT client, sleep/wake state machine (ozkey-08 §0.2/§0.3), and the
existing `mode=ozkey-cloud|ozkey-local` field. `threadcomm.ino` is
comparatively minimal — it exists to prove Thread-join and the F4 UDP relay
in isolation, nothing else. The unification grafts threadcomm's Thread
pieces into blecomm's shell, not the other way around.

## 3. What's shared vs. transport-specific

Both sketches already advertise the identical GATT surface (`OZLOCK` name,
service `4f5a4b31-0001-...`) per `blelock/CONTRACT.md` /
`CONTRACT-BRIDGE.md` — that's the head start that makes this tractable; the
app-side commissioner already treats them as the same product and
discriminates by `info.transport`.

| Layer | Shared (blecomm's, unchanged) | Transport-specific (new branch) |
|---|---|---|
| Identity | `device_id` (MAC-derived), GATT UUIDs, advertised name | — |
| Provisioning | `applyProvision()` parses `mode`, `site_id`, `broker_host`, `broker_tcp_port`, `name`, `heartbeat_s` | **New**: same function also checks for `network_key` (Thread dataset) vs `ssid` (Wi-Fi) — exact discriminator `threadcomm.ino` already uses |
| Credential path | Keypad entry, DPID 21/22/23/24/1 codec, PIN/NVS storage, scramble-PIN matcher (ozkey-08 §0.3) | unchanged — a DPID frame is a DPID frame regardless of how it arrived |
| Frame transport (down) | — | **Wi-Fi**: MQTT subscribe, existing `PubSubClient` path. **Thread**: `OThreadUDP` listen on the bridge32 F4 channel (realm-local multicast, port 5052 today — reconsider addressing once this is the real product, not a bench pair) |
| Frame transport (up, logs/heartbeat) | — | **Wi-Fi**: MQTT publish, existing path. **Thread**: relay to bridge32 over the same UDP channel; bridge32 forwards to MQTT (F4's other direction — proven, not yet built into the bridge, see `blelock/CONTRACT-BRIDGE.md` "Not in this increment") |
| Sleep/wake | ozkey-08 §0.2 SRDY/MRDY model, light-sleep cadence | **Open question (§7)**: does Thread's own SED polling replace this, or run alongside it? Needs a decision, not assumed |
| Local escape hatch | BOOT-hold-5s factory reset OR the existing keypad gesture (`*` then `5`) — blecomm already has this; no change needed | — |

## 4. Mode/transport selection model

No new field needed on top of what already exists — reuse and extend:

```json
{
  "v": 1,
  "mode": "ozkey-cloud",              // unchanged: ozkey-cloud | ozkey-local
  "site_id": "lab",
  "broker_host": "10.1.1.20", "broker_tcp_port": 1883,
  "name": "Cửa trước",
  "heartbeat_s": 60,

  // Wi-Fi transport (existing):
  "ssid": "...", "password": "...",

  // OR Thread transport (new — mutually exclusive with ssid/password,
  // exact shape threadcomm.ino already validates):
  "network_name": "OZ-a1b2", "ext_pan_id": "...", "network_key": "...",
  "channel": 15, "pan_id": "..."
}
```

`mode` still selects **who the lock talks to** (ozlockserv vs ozkeyserv-family).
The Thread-vs-Wi-Fi choice is **transport**, orthogonal to `mode` — any of
2a/2b/3/4 could in principle ride over either transport, though in practice
Wi-Fi-direct is what 3/4 (hotel/PMS) use per the hard no-bridge-for-hotel
rule (ozkey-08 §0.0.1) and Thread is what 2a (residential premium) uses.
Firmware shouldn't hard-code that pairing — validate the payload shape it's
actually given, same principle `validModePayload()` already applies in
`bridge32.ino`.

## 5. Server-side impact

**None expected.** ozlockserv/ozkeyserv already route by `device_id` on
device-scoped topics (ozkey-04 §9) regardless of what radio got the bytes
there. The one real gap is on `bridge32`, not the lock or the servers: F4's
uplink direction (lock → bridge → MQTT) is proven only in the downlink
direction so far (bridge → lock, per this session's F4 build) — bridge32
needs the reverse relay before an OZLOCK-HOME-premium lock's logs/heartbeats
can actually reach ozlockserv. Track this as a `bridge32.ino` gap, not a
unified-firmware gap.

## 6. Explicitly out of scope for this pass

- **Matter (#1)**: a full separate commissioning + fabric stack
  (`esp-matter`), DAC/attestation, takeover semantics (ozkey-08 §4.2). Bigger,
  later, and doesn't touch the OZKEY BLE provisioning path at all — a
  factory-fresh lock offers both commissioning paths simultaneously
  (ozkey-08 §6.1), it doesn't select Matter *through* this payload.
- **3-level RBAC UI, AI room-map generation, Google Maps assist**: all
  app/server, not firmware. Don't let "OZKEY/OZPMS need firmware work" creep
  in — per §1, they don't.
- **Bridge32's own F1-F6 hardening** (the OpenThread lock-acquire timing
  issues, the advertising-restart bug, the app-sync/orphan fixes) — all
  already done this session, unrelated to this unification.

## 7. Open questions for next session

1. **Flash-size risk (the reason N8 vs N16 matters, per the operator's own
   framing).** `blecomm.ino` alone: 1,575,618 bytes (47% of N8's ~3.34 MB app
   partition). `bridge32.ino` (Wi-Fi+BLE+LCD+MQTT+OpenThread+OThreadUDP, no
   keypad/DPID/PIN storage): 1,962,910 bytes (62% of N16's ~3.15 MB
   partition). These aren't directly subtractable (different feature sets),
   so **the actual delta OpenThread+OThreadUDP add on top of blecomm's
   existing footprint is unknown until the merge is attempted and compiled**.
   First concrete task next session: do the merge, compile for N8, measure.
   If it doesn't fit, N16 is the documented fallback — no redesign needed,
   just a bigger chip, per the operator's own note.
2. **Sleep/wake model when Thread is the transport.** ozkey-08 §0.2's
   SRDY/MRDY keep-alive model was designed around Wi-Fi's join/rejoin cost.
   Thread SEDs have their own native poll-period concept. Do these compose
   (Thread SED polling *is* the wake mechanism, §0.2 becomes Wi-Fi-only
   legacy) or does the MCU-side wake contract need to stay transport-agnostic
   because the comm module's identity (Wi-Fi vs Thread) isn't known until
   commissioning? Needs a decision before the sleep-state-machine code is
   touched.
3. **Thread UDP addressing beyond one bridge.** F4's current addressing
   (realm-local multicast, every lock on the mesh receives every datagram
   and filters by `target` device_id) was a deliberate v0 bench simplification
   because there was no discovery mechanism. Fine for a bench pair; worth
   revisiting once this is real product firmware with potentially several
   locks per bridge (CONTRACT-BRIDGE.md already says threadcomm is "many per
   home").
4. **Does `mode` need a third value**, or does deployment-only
   differentiation (§4) genuinely hold for OZKEY vs OZPMS indefinitely? Flag
   for the ozkeyserv/OZPMS product owner, not a firmware decision.

## 8. Suggested build order (next session)

1. Fork `blecomm.ino` (or work directly in it — operator's call at the time).
2. Port `threadcomm.ino`'s `applyProvision()` Thread-dataset branch,
   `OThread`/`DataSet` join logic, and the F4 `OThreadUDP` receive-and-filter
   code in as an alternate path selected by payload shape (§4).
3. Wire DPID frame delivery to route through whichever transport is active —
   the DPID codec itself doesn't change, only how frames arrive/leave.
4. Compile for N8 first (§7 Q1) — this is the concrete go/no-go signal for
   whether N16 is actually needed, not an assumption.
5. Bench-verify against a real bridge32 + real ozlockserv, mirroring the
   conformance discipline every prior milestone in this repo has used.
