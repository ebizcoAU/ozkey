# OZKEY-09 — Operational Support Modes: Consolidated Reference & Gap Analysis

> **Why this doc exists (operator ask, 2026-07-24):** the mode taxonomy has
> been decided piecemeal across ozkey-01→08 and the two `blelock/CONTRACT*.md`
> files. Real bring-up work on the GEEK bridge (bridge32 now running,
> advertising `OZBRIDGE` over BLE) surfaced several gaps the design docs never
> caught — this doc pulls the decided parts into one place, and just as
> importantly, names the gaps as gaps rather than letting them stay implicit.
> **This is a consolidation + gap list, not a new set of decisions.** Where a
> gap needs a call, it's marked OPEN and left for sign-off, not resolved here.

## 1. Canonical mode taxonomy (source: ozkey-08 §0, §0.0.0 — unchanged)

One lock, one comm-module firmware, four network personalities:

| # | Mode | Transport chain | Server | App |
|---|------|----------------|--------|-----|
| 1 | **MATTER** | lock → Thread border router (owner's ecosystem) | none of ours | owner's platform app |
| 2 | **OZLOCK-HOME** | lock (Wi-Fi or Thread+bridge) → MQTT → **ozlockserv** | ozlockserv | **BANOI** |
| 3 | **OZLOCK-HOTEL** | lock (Wi-Fi direct) → MQTT → **ozkeyserv** (on-prem) | ozkeyserv | **MAOI** |
| 4 | **OZLOCK-PMS** | lock (Wi-Fi direct) → cloud MQTT + cloud server → PMS app | ozkeyserv-family, cloud | MAOI-family (fleet) |

App-facing, this is presented as **six configuration options** (§0.0.0):

| # | Config | Mode | Bridge? |
|---|--------|------|---------|
| 1 | Matter over Thread (new Apple TV 4K / Nest Hub / Echo v4+) | 1a | No — owner's own border router |
| 2 | Matter via OZBRIDGE (old/Thread-less controllers) | 1b | **Yes** |
| 3 | OZLOCK premium (5s remote) | 2 | **Yes** |
| 4 | OZLOCK economy (10-min + touch-wake) | 2 (economy) | No |
| 5 | OZKEY (hotel/motel) | 3 | **No** — deliberately Wi-Fi direct |
| 6 | OZPMS (managed rentals) | 4 | No |

**Hard rule (§0.0.1, do not relitigate without a deliberate revision):** the
bridge is **residential + Matter-bridge only** (options 2 & 3). Hotel/PMS
(options 5 & 6) are **Wi-Fi 6 TWT direct to site APs, no bridge** — Thread
SEDs (battery locks) can't mesh/route, so bridging hotel corridors through
concrete would need wall-powered repeater chains, which is why that path was
explicitly rejected. **OPEN (raised 2026-07-24, unresolved):** a proposal
surfaced to route OZKEY-hotel through a bridge after all — flagged as a
conflict with the above, not yet decided either way.

## 2. Role glossary

| Name | What it is | Radio | Notes |
|---|---|---|---|
| **OZBRIDGE** (`bridge32`) | Dual-personality N16 box, one per home | Wi-Fi + Thread | Personality A = Matter-over-Wi-Fi bridge (Mode 1b). Personality B = MQTT uplink (Mode 2 only). **Not** in the hotel/PMS chain. |
| **threadcomm** | The lock's comm module, Thread transport | Thread + BLE (commissioning only) | Not a separate product — reuses `blecomm`'s exact `OZLOCK` BLE identity/UUIDs; `info.transport` discriminates. Many per home. |
| **blecomm** | The lock's comm module, production silicon (all SKUs) | Wi-Fi + BLE | One firmware, two NVS-selected personalities: `mode=ozkey-cloud` (Mode 2) or `mode=ozkey-local` (Mode 3; Mode 4 TBD, likely reuses `ozkey-local` semantics against a cloud host). |
| **BANOI** | Personal app | — | Commissions Mode 2 locks + bridges (BLE), is the courier for the two-hop bridge/lock handoff. |
| **MAOI** | Commercial app | — | Commissions Mode 3/4 locks. |

## 3. Pairing / commissioning — as currently speced

### 3.1 Single-hop (direct lock: `blecomm` or `threadcomm`'s own BLE identity)

Per ozkey-08 §3–5 and `CONTRACT.md`:

```
Phase 0 (boxed lock)   BLE only — advertises OZLOCK, factory-pubkey QR on display
  │  App scans QR (trust anchor, pins the handshake to THIS physical lock)
  │  → BLE session → writes provision JSON (wifi/thread + server + site + mode)
  ▼
Wi-Fi/Thread join → broker connect → ENROLLED/READY (closed-loop STATUS notify)
  ▼
Pair to room/account (server-side /locks/pair)
```

**OPEN / already-known gap (`CONTRACT.md` "Deferred v2"):** the QR
trust-anchor step is *designed* (ozkey-08 §3) but explicitly **not yet wired
into the actual bench firmware** — v0/v1 provision is plaintext, no
QR-pinned session yet. So even for the simplest single-hop case, "scan QR
→ pinned handshake" is a plan, not a built behavior, today.

### 3.2 Two-hop courier (bridge32 + threadcomm — Mode 2, Option B, locked 2026-07-23)

Per `CONTRACT-BRIDGE.md`:

```
1. Bridge already provisioned + THREAD_OK (one-time, per home).
2. App connects to bridge32, reads `info` → has the Thread operational dataset.
3. App connects to threadcomm (new lock), writes that dataset into its
   `provision` characteristic.
4. threadcomm commits the dataset, joins the mesh → THREAD_OK / THREAD_FAIL.
```

Neither device finds the other over the air — **the phone is the courier**,
sequentially, for both BLE sessions. This means bridge32 and the lock **never
need to be within BLE range of each other**, only each within range of the
phone in turn (same as any normal walk-up commissioning) — physical
separation between bridge and lock is a non-issue for *this* step.

**OPEN — bridge32 personality selection:** `bridge32`'s `provision`
characteristic currently only accepts `{"device_id","ssid","password"}` — no
field exists for the app to tell it "be Personality A (Matter bridge) or
Personality B (MQTT uplink)". Not designed, not built.

**OPEN — no QR trust anchor for OZBRIDGE at all.** Unlike the lock (which at
least has one *designed*, per 3.1), `CONTRACT-BRIDGE.md` has no anti-MITM
pinning step for bridge32's own BLE provisioning.

## 4. Known gaps / open risks (as of 2026-07-24 bring-up)

| # | Gap | Status | Why it matters |
|---|---|---|---|
| 1 | Hotel routed through a bridge (conflicts §0.0.1) | **OPEN**, needs a decision | Contradicts the explicit no-bridge-for-hotel rationale (Thread SEDs can't mesh through concrete) |
| 2 | Bridge32 personality (A/B) selection | Not designed | App has no way to configure which job bridge32 does at provisioning time |
| 3 | QR trust anchor missing for OZBRIDGE | Not designed | Lock at least has a deferred plan (3.1); bridge has none |
| 4 | QR trust anchor deferred even for the lock | Designed, not built | v0/v1 bench flow is plaintext-only; "scan-QR-first" isn't real yet |
| 5 | **Thread mesh RF range, bridge32 ↔ lock** | **Untested, unmitigated** | Once joined, the lock's actual 802.15.4 traffic must physically reach bridge32. Same physics ozkey-08 §0.0.1 used to reject bridging hotels (leaf-node locks, no mesh routing, ~9–20m through-concrete range) applies at house scale — no repeater story, no "weak link" warning at commissioning, for a residential mode that *does* use the bridge |
| 6 | MQTT uplink on bridge32 | Not built (`CONTRACT-BRIDGE.md` "Not in this increment") | Thread network forms, but there is currently zero path from bridge32 to ozlockserv |
| 7 | Tuya UART relay on threadcomm | Not built | Lock joins the mesh but has no actual credential/unlock function riding on it yet |
| 8 | Thread-side frame transport (threadcomm↔bridge32 payload) | Not built | The mesh link itself (and gap #5's range risk) has never been exercised with real traffic |
| 9 | LCD status on bridge32/GEEK | **OPEN** — bench debug aid, or reconsidering headless-for-production? | Production is speced headless (LED + BLE status chars only, §0.0.1); GEEK just happens to have a screen |

## 5. Current build/bring-up status (this session, 2026-07-24)

- `bridge32.ino` flashed to the GEEK (N16) board — confirmed **running**,
  advertising `OZBRIDGE` over BLE (serial: `b-98a316a7e638 mac=98:A3:16:A7:E6:38`,
  `[BLE] advertising as OZBRIDGE`). Wi-Fi/Thread bring-up path unexercised
  beyond that in this session.
- `GeekDisplayTest.ino` — ST7789 pin map + QR rendering bring-up, done as a
  bench aid (not yet folded into `bridge32.ino` itself). Settled on 72-char
  QR payloads (V3, 116px, confirmed scannable) as the practical ceiling
  before hitting V4's much tighter physical fit (132px in a 135px screen,
  confirmed to fail scanning).
- `blecomm.ino` (N8 Touch-LCD board) — compiles clean at ~47% flash with the
  correct board settings (`FlashSize=8M`, `PartitionScheme=default_8MB`);
  earlier "sketch too big" errors were a board-menu leftover from GEEK
  testing, not a real code-size problem.
- `threadcomm.ino` — not flashed this session.

## 6. Proposed next-step sequencing (PROPOSAL — not decided, for sign-off)

1. Resolve gap #1 (hotel-bridge conflict) — either reaffirm §0.0.1's no-bridge
   rule or deliberately revise it; downstream decisions depend on this.
2. Design bridge32 personality selection (gap #2) — likely a `mode` field on
   `bridge32`'s `provision` payload, mirroring the lock's existing pattern.
3. Decide gap #9 (LCD scope) — determines whether display work continues as
   throwaway bench code or becomes a real bridge32 feature.
4. Before investing further in Thread-transport features (gaps #6–8),
   bench-test gap #5 (RF range) at a realistic residential distance —
   cheapest to find out now than after MQTT/Tuya relay are built on top of an
   untested link.
5. QR trust-anchor wiring (gaps #3–4) can follow once the above are settled —
   it's an anti-MITM hardening step, not a blocker for functional bring-up.
