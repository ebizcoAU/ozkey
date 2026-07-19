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

## 0. THE FOUR MODES — canonical taxonomy (operator, 2026-07-19)

One lock, one comm-module firmware, four network personalities. This table is
the **canonical mode vocabulary** — it supersedes the earlier per-doc
numberings (ozkey-07's "Mode A", this doc's build "phases", §10's internal
mode 2/3 ladder). Those older labels remain valid in their historical context;
new work references the modes below.

| # | Mode | Transport chain | Server | App | Status |
|---|------|----------------|--------|-----|--------|
| **1** | **MATTER** (Matter over Thread) | lock → Thread border router (Apple TV / Google Nest / Alexa / Home Assistant) → owner's ecosystem | none of ours | owner's platform app | **planned** — whitepaper consumer tier |
| **2** | **OZLOCK-HOME** | lock C6 (Wi-Fi) → MQTT broker → **ozlockserv** (personal cloud, `:4200`) | ozlockserv | **BANOI** (BLE commissioning built-in) | ✅ **BUILT + verified on real C6** (2026-07-17, no 2nd bridge needed) |
| **3** | **OZLOCK-HOTEL** | lock C6 (Wi-Fi) → MQTT broker → **ozkeyserv** (local server, `:3200`) → MAOI | ozkeyserv (on-prem) | **MAOI** | ✅ **BUILT + verified on real C6** (2026-07-18, no 2nd bridge needed) |
| **4** | **OZLOCK-PMS** | lock C6 (Wi-Fi) → cloud MQTT + **cloud server** → PMS app | ozkeyserv-family, cloud-hosted | MAOI-family, **3 management levels** (AUS-wide rental portfolios) | **planned** — contract seeds in ozkey-07 §1 "Fleet" + §2.1 owner-root delegation |

Notes:
- **The 2nd ESP32 (bridge C6 N16) is a DUAL-PERSONALITY box (operator,
  2026-07-19)** — commissioning-time choice:
  - **Personality A — Matter-over-Wi-Fi bridge (Mode 1b):** joins home Wi-Fi
    and exposes the Thread lock as a **bridged Matter Door Lock endpoint over
    Wi-Fi** (Hue-Bridge pattern). This is what reaches Thread-less
    controllers — old Apple TV / Echo / PC-hosted HA are Matter controllers
    with no Thread radio (and Apple won't use 3rd-party border routers), so
    plain Thread-border-routing is NOT enough for them. For HA, the same box
    can also act as a plain OpenThread border router (Mode 1a-adjacent).
  - **Personality B — MQTT uplink (Modes 2–4):** lock →Thread→ bridge
    →Wi-Fi→ broker → ozlockserv / ozkeyserv / cloud.
  - Mode 1a (home already has a modern Apple TV 4K / Nest Hub / Echo v4+ as
    border router) needs no OZ bridge at all. Wi-Fi+Thread coex on one C6 is
    bench-proven (2026-07-13); Matter-bridge + OTBR + MQTT stacks coexisting
    is the third justification for N16 flash. Concurrent A+B (multi-fabric +
    MQTT at once) is technically open but deferred to gen-2 — v1 ships it as
    a commissioning choice.
- **Broker placement in Modes 2–4** is a deployment choice, not architecture:
  the lab runs everything on one LAN host (10.1.1.21 — Mosquitto `:1883`,
  ozlockserv `:4200`, ozkeyserv `:3200`); Mode 4 hosts the same broker+server
  in the cloud. The lock config only ever holds `broker_host:port`.
- **Firmware mode mapping (NVS `mode`):** `ozkey-cloud` = Mode 2 ·
  `ozkey-local` = Mode 3 · Mode 4 reuses `ozkey-local` semantics against a
  cloud host (value TBD if a distinct personality proves necessary) · Mode 1
  is the Matter fabric, entered/exited per §4.2 takeover semantics.
- **Comm-module split (2026-07-19):** `blelock/blecomm/` is the pure comm
  module (Tuya TYWE3S-equivalent) — the lock MCU owns keypad/RFID/fingerprint
  and ALL credential auth; blecomm only transports frames, plus a
  10,000-event offline transaction buffer (LittleFS JSONL ring). `blelock/`
  remains the all-in-one emulator (MCU+module on one board). Wire between
  module and MCU (LockSim Mode B): raw Tuya 55 AA on UART1 GPIO16/17 @9600
  8N1, wire-verified 2026-07-19.

### 0.0.0 App-facing DL configuration menu (operator, 2026-07-19 — checked)

The commissioning app presents SIX options (one lock, one firmware):

| # | Config | Chain | Notes |
|---|--------|-------|-------|
| 1 | **Matter over Thread** | lock ─Thread→ owner's border router (new Apple TV 4K / Nest Hub / Google TV Streamer '24 / Echo v4+) | Mode 1a. Onboards via ecosystem's own Matter QR flow — no OZ app/account needed (sovereignty feature) |
| 2 | **Matter via OZBRIDGE** | lock ─Thread→ OZBRIDGE (Matter-over-Wi-Fi node) → old/Thread-less Apple TV / Google TV / Alexa | Mode 1b |
| 3 | **OZLOCK premium (5s)** | lock ─Thread→ OZBRIDGE ─Wi-Fi→ cloud MQTT → OZLOCK app | Mode 2. 5s downlink = SED poll (whitepaper Table-2 default) |
| 4 | **OZLOCK economy (no bridge)** | lock ─Wi-Fi direct, 10-min + Touch2Wake → cloud MQTT → OZLOCK app | Mode 2 economy. Any router. Saves the bridge cost |
| 5 | **OZKEY (hotel/motel)** | lock ─Wi-Fi direct, 10-min + Touch2Wake → local ozkeyserv → MQTT → OZKEY app | Mode 3. See PIN-race fix below |
| 6 | **OZPMS (managed rentals)** | lock ─Wi-Fi direct, 10-min + Touch2Wake → cloud server + MQTT → OZPMS app | Mode 4 |

- **Check-in PIN race fix (options 5/6):** touch triggers wake AND an
  immediate heartbeat pull in parallel with PIN entry — ozkeyserv already
  flushes queued credentials on heartbeat (verified bench behavior), so the
  PIN lands during the 3–5s the guest spends typing it. Requires fast-connect
  (<1s join) — the same work item as the battery claim. This makes 10-min+T2W
  hotel-viable on ANY motel AP (no Wi-Fi-6 requirement); **TWT 30–60s is a
  site-dependent UPGRADE** (instant remote unlock) where ax APs support it,
  not the baseline.
- **The 10-min interval is HOUSEKEEPING CADENCE, not user-facing latency**
  (operator + team, 2026-07-19). Touch-pull collapses every human-at-the-door
  flow to touch+join seconds: check-in PIN (pulled during the guest's own
  entry; the walk-to-room time is bonus margin, not the guarantee) ·
  **checkout revocation** (a departed guest's touch pulls the pending revoke
  before their entry validates — the attempt defeats itself) · **remote
  unlock** (the person at the door touches → lock pulls the queued command —
  remote open is touch-assisted, and someone wanting in always touches). Only
  logs/config/OTA checks actually wait the 10 min — and they can. Install
  checklist: site-survey Wi-Fi AT THE DOOR positions (2.4GHz, steel frames —
  motel Wi-Fi is planned for rooms, not corridor doors) + non-captive-portal
  SSID/VLAN for locks.
- **Guest-facing unlock is instant in EVERY option** (auth is local on the
  lock MCU, network-free) — the tiers differ only in REMOTE latency. App copy
  should state this so option 4 doesn't read as a "slow lock".
- **"App only sees the lock on wake" — resolved as DEVICE-SHADOW UX
  (operator concern, 2026-07-20):** the app never talks to the lock directly
  in ANY option — it reads the SERVER's shadow, and the shadow is accurate
  because every state change originates at an awake lock and is pushed as it
  happens. A sleeping lock costs exactly two things: the live "online" dot,
  and unsolicited downlink (≤10 min when nobody is at the door). UI: show
  "🔒 Locked · synced X min ago" + pending-command badge ("applies ≤10 min,
  or instantly on touch") — the standard commercial battery-lock pattern.
  Outs: at-the-door app unlock can go **BLE-direct** (instant, no network —
  radio already there from commissioning); truly-remote-instant buyers = the
  bridge tier (option 3) / hotel TWT — that IS the upsell. **App work item
  P7:** BANOI lock UI needs shadow-state treatment (drop live-presence badge,
  add synced-ago + pending badges); servers need nothing (ozlockserv DB is
  the shadow).
- Options 3/4 are the same OZLOCK app — present as a speed/price toggle
  ("have an OZBRIDGE?"), not separate products.

### 0.0.1 Production silicon decision (operator, 2026-07-19; REVISED same day)

- **Doorlock comm module = ESP32-C6 N8, ALL SKUs** (revised from H2 — the
  video SKU decides it): the video doorlock variant uses a **Tuya peephole
  media MCU that encodes H.264 itself**; the comm module only TRANSPORTS the
  stream — to the bridge or **direct to the app via STUN/P2P** (broker =
  signaling only; video never transits our servers — sovereignty win, note
  for whitepaper v3). Thread/802.15.4 tops out ~250 kbps and can NEVER carry
  H.264, so a shared module must be Wi-Fi-capable → **H2 is disqualified by
  physics** for any video-sharing platform. C6 N8 = the exact bench-verified
  chip+firmware (production module ≡ blecomm, headless).
  ⚠ Video-SKU integration check: media-MCU→C6 link must be **SDIO/SPI**
  (C6 has an SDIO slave; ESP-Hosted pattern) — UART cannot carry 1–4 Mbps.
  Some Tuya media SoCs expect to own the Wi-Fi themselves; in that variant
  the C6 stays lock-comm-only and video has its own radio. Decide per module
  sourced.
- Headless in production: **no LCD/touch** — status via one LED + the BLE
  status/info characteristics (the app is the dashboard); factory reset via
  MCU-initiated Tuya module-reset command (lock's own UI gesture) + strap-GPIO
  recovery fallback.
- **Transport (operator-revised 2026-07-19 PM): control plane is PER
  DEPLOYMENT — the C6's dual radio is what makes this free.**
  - **Residential (Modes 1/2) = THREAD**: lock ─Thread→ owner's border router
    (Mode 1) or our N16 bridge (Mode 2) ─Wi-Fi→ MQTT → ozlockserv. Short
    distances; Matter ecosystem; home APs rarely do TWT properly.
  - **Hotel/PMS (Modes 3/4) = Wi-Fi 6 TWT DIRECT to the site's existing APs —
    NO bridge.** Rationale (operator finding): Thread SEDs cannot route
    (battery locks = leaf nodes, no lock-to-lock mesh — ⚠ whitepaper §6.2
    currently overclaims this and must be fixed in v3), so through-concrete
    range (~9–20m worst case) would demand chains of wall-powered repeaters
    in guest corridors — unplugged/misplaced/stolen in practice. The motel
    already has a Wi-Fi blanket; use it.
  - Lock Wi-Fi additionally does video burst on the video SKU (peephole media
    MCU → SDIO → C6 → STUN/P2P) in ANY deployment.
- **TWT engineering truths (do not overclaim):** TWT keeps association via
  LIGHT sleep (C6 ~130–200µA floor) — realistic average ≈ **100–250µA**, NOT
  the ~11µA sometimes cited (deep sleep 7µA drops association → 2–5s rejoin).
  2400mAh ÷ ~200µA ≈ **1.4 years** — fine for scheduled hotel battery swaps.
  "Instant" is UPLINK-only (touch-wake transmit anytime); DOWNLINK (remote
  unlock, **check-in PIN sync**) waits for the next TWT service period → the
  TWT interval IS the whitepaper Table-2 poll knob; set **30–60s for hotels**
  (PIN must beat the guest to the door), not 10 min. Deployment prereqs:
  802.11ax APs with TWT actually ENABLED (many enterprise sites ship it off),
  dedicated lock SSID/VLAN, per-AP-model TWT verification.
- **Bridge = ESP32-C6 N16, RESIDENTIAL (Mode 2) + Matter-bridge (Mode 1b)
  only** — no longer in the hotel chain. Jobs: Thread border router + MQTT
  uplink + failover token cache + store-and-forward spool + lock-OTA host.
- **Bridge value proposition & the bridge-less residential SKU (operator,
  2026-07-19):** the bridge buys exactly three things — (1) INSTANT downlink
  (remote unlock / PIN sync ~5s via Thread SED) at years-class battery,
  (2) Matter for Thread-less homes, (3) home-router independence. If ~10-min
  downlink + touch-to-wake is ACCEPTED, a **lock-only SKU needs no bridge on
  any router**: deep sleep 7µA → touch interrupt → LOCAL MCU auth (guest
  unlock is always instant, network-free) + 10-min heartbeat join to push
  logs / pull PINs. ⚠ Battery there is dominated by the periodic Wi-Fi JOIN,
  not sleep: naive 3–5s join ≈ 0.5mA avg (~6 months); fast-connect (PMK
  cache, static IP, pinned channel, <1s join) ≈ 0.1–0.2mA → 1.5–3yr — the
  fast-connect work is where the lock-only battery claim is earned. Product
  ladder: lock-only economy (any router, no Matter) · lock+bridge combo
  (instant remote + Matter — the whitepaper sub-$100 bundle) · Mode 1a
  (owner's own border router, no bridge) · hotel TWT (site APs, no bridge).
- **Firmware gap to production:** bench blelock/blecomm (Wi-Fi/MQTT direct)
  = **near-production for the HOTEL path** — gen-1 delta is TWT negotiation +
  sleep-state machine + touch-wake, not a protocol rewrite. OpenThread SED on
  the lock + bridge firmware move to the RESIDENTIAL SKU's timeline. Servers/
  apps unchanged either way.

### 0.1 DP vocabulary decision — STRICT TUYA ONLY (2026-07-19)

The doorlock speaks the **standard Tuya DP vocabulary and nothing else**:
DP 1 (unlock channel), 2 (RFID), 3 (fingerprint), 5 (battery), 8
(ACCESS_RESULT), 21–24 (credential add/delete). Rationale: lock makers will
not modify MCU firmware for us, and the standard MCU↔module serial protocol
is unauthenticated plaintext — a swapped-in OZ comm module works with any
stock Tuya lock MCU with **no secret required** (the only Tuya secrets are
module↔cloud auth, which we replace, and the optional offline-dynamic-PIN
seed, which we don't need — our servers issue temp PINs via standard DPID 21).

**DP 9 "ATTEMPT_REPORT" (tier-2 credential escalation) was built and then
REJECTED 2026-07-19**: the standard protocol never transmits attempted
credential values, so no stock MCU would ever emit it. Large-directory
(>50-user) authentication belongs to a **separate access-control product**
with its own keypad/RFID/fingerprint sensors that commands the lock via the
standard remote-unlock DP — the commercial reader/controller model — not to
the doorlock's comm module. Code reverted from blecomm + LockSim same day.

### 0.2 Module power & wake topology — persistent power + SRDY/MRDY (operator decisions 2026-07-20)

> Supersedes any earlier framing of "T2W = power-on-by-MCU". Where §0.0.1's
> option rows say "10-min + Touch2Wake", read them through this section: the
> cadence is MODULE-owned, the touch is an accelerator (§0.3).

**Tuya ships two Wi-Fi lock architectures**, and the difference decides the
whole product:

1. **Power-shutdown** ([Wi-Fi Lock Hardware Design], WBRU): *"In standby
   mode, the Wi-Fi power supply must be completely shut down."* Module dead
   between events; MCU wakes it to exchange data. **No downlink while
   asleep → the field horror story**: guest waits 5–10 min for a PIN that
   never syncs, calls support, support regenerates into the same undelivered
   queue, cycle repeats. Common to hotel/motel/PMS deployments — exactly the
   segment that issues time-critical remote credentials forty times a day.
   Rail-gating also **forecloses TWT physically** (TWT = a held association;
   a rail-gated module can't hold one). Gated vs TWT are mutually exclusive
   topologies, not points on a dial.
2. **Keep-alive** ([Wake-Up Logic of Keep-Alive Smart Lock]): module on
   **dedicated, persistent 3.3V/GND rails**, holds its Wi-Fi association in
   low-power keep-alive, and wakes/is-woken over two IO lines. The BLE lock
   guide (TYBN1, I/O 11 / I/O 14) is the same pattern.

**DECISION — our comm module is keep-alive topology, always:** a separate
battery line feeds the module through its **own regulator** (low-Iq buck —
Tuya's own doc names SGM2040-class LDO or DC-DC; regulator Iq must sit well
below the ~130–200µA TWT sleep floor; bulk capacitance for ~300–400mA Wi-Fi
TX bursts; common ground with the MCU UART; no back-feed into the MCU's
module-rail net). Second, independent reason: a stock lock MCU's rail was
never sized to source ESP32 TX peaks. The four signal lines are then exactly
Tuya's: **TXD, RXD, SRDY, MRDY** — power is never signalled.

**Wake-line contract** (Tuya keep-alive semantics, adopted verbatim):

| Line | Dir | blecomm GPIO | Active | Meaning |
|---|---|---|---|---|
| SRDY | MCU → module | **GPIO7** (LP pin — deep-sleep-wake capable) | low | "module, wake" / held low = MCU awake |
| MRDY | module → MCU | **GPIO8** (⚠ C6 strapping pin — must be high/floating at reset; MRDY idles high so semantics align, but a stock MCU pull-down on this line would block boot → remap when real lock hardware arrives) | low | "MCU, wake" / held low = module awake or has downlink |

Handshake = answer-before-transmit: initiator pulls its line low, **waits for
the other line to answer low, then transmits UART**; after 10 s of serial
idle the module releases MRDY, MCU (if idle) releases SRDY, both may sleep.
No bytes ever hit a sleeping UART — the handshake is the flow control (this
obsoletes any wake-preamble hack). GPIO1–4 are reserved (SPI/SD).

**Drop-in mapping (one firmware, pin-role set at commissioning):** a stock
keep-alive MCU speaks MRDY/SRDY natively → true drop-in. A stock
power-shutdown lock only has an EN/power line → our battery-fed module
senses that line as an SRDY-equivalent wake input and ignores its
power-gating intent.

**Radio policy is the only per-tier variance** (module self-manages, per
deployment): TWT 30–60 s where the site has ax APs (hotel upgrade) · DTIM
keep-alive · timed light-sleep + fast rejoin (any router). ⚠ AP inactivity
timers (often 5 min) can deauth a station sleeping longer — install
checklist: AP idle timeout ≥ wake interval, or accept the (fast-connect-
cheap) rejoin.

**Bench stand-in (CP2102 exposes TX/RX only):** NVS flag `wake_sim`
(default ON): SRDY treated as permanently asserted (module never blocks a
transmit, does not light-sleep); MRDY is still driven genuinely (probe-able,
logged in `[MON]`). Flip the flag off when real wake wiring exists — honest
handshake + sleep engage with zero code change. Optional bench sleep demo
fallback: RX-start-bit GPIO wake + LockSim 0x00 preamble.

[Wi-Fi Lock Hardware Design]: https://developer.tuya.com/en/docs/iot/hardware-design-guidance?id=K9pestuito11n
[Wake-Up Logic of Keep-Alive Smart Lock]: https://developer.tuya.com/en/docs/iot/wifi-keepalive-door-lock-wakeup-logic?id=Kada52p7tfr5b

### 0.3 Credential delivery model — the SLA (operator decisions 2026-07-20)

A stock MCU is **reactive**: nothing obliges it to assert SRDY during keypad
entry — it wakes the module to *report*, i.e. after the failure. Honest
stock-hardware sequence for a not-yet-synced PIN:

```
new PIN typed → local table miss → DENIED
→ MCU wakes module (report failed transaction)
→ module connects → pushes log → heartbeat pulls pending → DP21 stored
→ retry works (if the guest is still there; if not → support call → cycle)
```

**The three delivery paths, ranked by guarantee:**

| Path | Nature | Guarantee level |
|---|---|---|
| Module's own proactive wake (interval knob) | independent of MCU and humans | **THE guarantee** — a PIN issued at booking lands within one interval, before the guest reaches the door |
| Failed-attempt sync (sequence above) | reactive, stock-MCU-native | **the safety net** — self-healing IF the retry happens; requires (a) heartbeat-on-connect flush [verified bench behavior] and (b) guest copy: *"if the code fails, wait a few seconds and try once more"* — never "call support" |
| Touch-parallel wake / scramble-digit mask | only on MCU firmware that asserts SRDY on keypad activity | **accelerator, never assumed** |

The proactive cadence is the product, not housekeeping — it is why the
module needed independent power (§0.2). Revocation (checkout, bond revoke
§0.4) rides the same SLA; a departed guest's own entry attempt still
triggers the safety-net sync that defeats it.

- **Wake-interval knob (USER-SETTABLE, operator 2026-07-20):** reuse
  `heartbeat_s` (no new field). App-visible per-lock setting in minutes:
  **range 1–10 min, step 1** (revised same day from an earlier 1–30).
  **Defaults per mode: hotel/PMS 1–2 min · residential 10 min (= range
  max).** It is explicitly the battery↔latency trade, so it belongs in the
  user's hands; app copy states both sides ("mã mới đến khoá trong ≤N phút ·
  N nhỏ hơn = tốn pin hơn").
- **Supersede, don't accumulate** (ozkeyserv/ozlockserv work item): a new
  PIN issued for a booking/room that already has one pending REPLACES the
  pending grant and queues a revoke for any previously-synced one. Slots are
  finite (≤16 in blecomm; similar on real locks); the support cycle must not
  deliver a stack of DP21s.
- **Scramble PIN (anti-peeping, Tuya-style `<junk><PIN><junk>#`):** adopt.
  Matching = any contiguous substring of the entry against stored PINs —
  implemented in the **MCU** (LockSim `submitPin`), never the comm module
  (strict-DP split, §0.1). Rules: wrong-attempt lockout counts **per entry,
  not per substring** (an L-digit entry contains ~L−k+1 candidate substrings
  → ~L× brute-force speedup if counted naively); max entry length ~20.
  Second job: the junk prefix masks wake latency — by the time the real PIN
  passes the matcher, the failed-attempt/touch sync has often already
  delivered it.

### 0.4 Multi-user — N-bond members over BLE (operator decisions 2026-07-20)

The second-person problem: the door key lives in the first person's app.
**Tuya's answer is accounts**: cloud account + join the "Home" + explicit
Administrator role (their plain "shared device" path for locks is
view-only — a support-page genre of its own), and BLE-direct is
one-phone-at-a-time (their fix: buy a gateway). **Ours is the XF-42 §14.2
N-bond keyring, account-less**: the lock stores a **bond set**
`{pubkey, role, label}`; **bond #0 = the primary appID written at first
commissioning = the lock's root of trust**; members are unlock-only. Role is
**per-(app, lock)**: one app identity (one keypair) can be Chủ khoá on its
own doors and Thành viên on someone else's — same key, different rows in
different locks' bond tables.

**Grant ceremony (4 decisions CONFIRMED by operator 2026-07-20):**

```
PRIMARY (anywhere): Thành viên khoá → "Thêm thành viên" → name + role
  → biometric prompt → app builds INVITE, shows as QR:
    { lock device_id, member label+role, nonce, expiry ~10 min,
      SIGNATURE by primary key }          ← public info only, no secrets
2ND PHONE: same app, own self-generated keypair →
  "Thêm cửa bằng mã QR" → scan → at the door within expiry →
  tap any key → BLE window → encrypted session → presents
    { its own pubkey + the invite }
LOCK (fully offline): verify invite sig against bond #0 → nonce unused +
  unexpired → add bond {pubkey, role:member, label} → confirm
NEXT SYNC: log "bond_added" → OZLOCK's door→apps map builds passively
```

1. **Holder-at-door is enough** — primary need not be present; sending the
   QR remotely (within expiry) is allowed: physical-key semantics. ✔
2. **Single-use nonce = the hard guarantee** (NVS replay cache); the ~10-min
   expiry is best-effort (lock clock may drift). ✔
3. **Biometric gate (OS `local_auth` prompt — Face/fingerprint/passcode; we
   never see biometric data) required to CREATE an invite and to UNLOCK.**
   Private keys stored auth-gated in Keychain/Keystore. ✔
4. **Exactly one admin in v1 (bond #0).** Second-admin (spouse), admin
   transfer, label sync = v2. ✔

**Member unlock ladder** (no server, no primary, no internet in the loop):

```
tap any key → MCU wakes → SRDY → module wakes → BLE advertising (~60 s)
app connects → lock sends fresh nonce → biometric prompt →
app answers with sealed unlock command (ozkey-06 envelope, counter fresh) →
lock: pubkey in bond set? role ok? envelope valid? →
DP1 remote-unlock frame → MCU fires motor → event logged → pushed at next sync
```

Decisions attached:

- **Advertising = touch-window only in v1** (~60 s after any touch): zero
  idle power, zero idle RF beacon (no wardriving enumeration), BLE attack
  surface exists only with physical presence. Always-on low-duty
  (+20–40µA, requires RPA rotation) = a later app toggle enabling proximity
  auto-unlock — an upsell, not a v1 promise. Proximity may *initiate*,
  never *authorize* (XF-42).
- **Labels live ON THE LOCK** (bond record carries the name): the lock is
  the authoritative member registry; phones cache; **OZLOCK sees pubkeys and
  events only** (names+doors+times in one server DB is a surveillance
  dataset we don't build). The door→apps mapping OZLOCK lacked builds
  passively from bond_added/bond_revoked log events — no new server write
  path, blind-registry preserved. Optional: **encrypted keyring backup
  blob** on OZLOCK (sealed by the primary key, server-opaque) — the XF-42
  export/import seam, materialized.
- **Owner-reset wipes ALL bonds** (a reset lock trusts nobody; re-invite is
  minutes; sold-house semantics). **Bond cap declared: 16**, surfaced in UI
  ("14/16 thành viên").
- **BANOI UI** (FTPOS-side — to be filed as its own XFtposDecisions entry):
  Khoá cửa list mixes roles with badges (Chủ khoá / Thành viên); "Thành viên
  khoá" tab exists only on admin doors (UI mirrors lock-side enforcement —
  the lock refuses admin verbs from member bonds regardless); member-door
  row is thin (unlock + own history + "rời khỏi cửa này"); member states use
  the §0.0.0 shadow-badge language (active / pending-invite /
  revoked-pending-sync). New dep: `local_auth` (+ the existing
  flutter_blue_plus).
- **Stolen-phone analysis:** locked phone → nothing; snatched-unlocked →
  unlock and invite-creation each demand a fresh biometric; a photographed
  QR is dead after first redemption (nonce) or ~10 min (expiry); sniffed
  BLE replays fail (fresh nonce + envelope counter).

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
> **Awaiting on-device verify (operator flashes).**
>
> **BUILD LOG 2026-07-16 (later): B4 BANOI leg BUILT + PUSHED** (ftpos
> `edfcc52`, analyzer-clean, 59/59 package tests): flutter_blue_plus ^1.35.0 +
> iOS/Android BLE permissions; `FlutterBlueOzkeyTransport` (scan by service
> UUID + "OZLOCK" name fallback, MTU 247, chunked provision writes, status
> notifies); `OzkeyBleSession.readInfo()`; `ProvisionPayload.name`; Khoá cửa
> 20 s background scan → banner → `_BleCommissionSheet` 6-step ladder →
> `addEnrolledLock`. **B4 awaits the flashed board for the end-to-end run.**

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



# *************** OPERATION MODE ***************
+---+-----------------------------------+--------------------------------------+

| # | Architecture Configuration        | Technical Review Notes               |
+---+-----------------------------------+--------------------------------------+

| 1 | Matter/Thread -> New TV Hubs      | Mode 1a. Valid. Google hubs = Nest   |
|   | (AppleTV, GoogleTV, Alexa)        | Hub 2nd-gen, Nest Wifi Pro, or Google|
|   |                                   | TV Streamer. Older plain Google TVs  |
|   |                                   | lack Thread (hence Option 2).        |
+---+-----------------------------------+--------------------------------------+

| 2 | Matter/Wi-Fi via OZBRIDGE ->      | Mode 1b. Valid. OZBRIDGE acts as a   |
|   | Legacy Hubs                       | Matter-over-Wi-Fi node. Exposes lock |
|   | (AppleTV, GoogleTV, Alexa)        | as bridged endpoint. Lock-to-bridge  |
|   |                                   | leg runs natively on Thread.         |
+---+-----------------------------------+--------------------------------------+

| 3 | Thread -> OZBRIDGE ->             | Mode 2 Premium. Wording fix: Thread  |
|   | Wi-Fi/MQTT Cloud -> OZLOCK        | to bridge, then Wi-Fi egress. The 5s |
|   | (5s Opening Response)             | lag is the SED poll interval based   |
|   |                                   | on Table-2 whitepaper defaults.      |
+---+-----------------------------------+--------------------------------------+

| 4 | Direct Wi-Fi (10m + Touch2Wake)   | Mode 2 Economy. Lock-only SKU. Works |
|   | -> Cloud MQTT -> OZLOCK           | natively on any standard router. No  |
|   | (No Bridge Required)              | secondary bridge needed.             |
+---+-----------------------------------+--------------------------------------+

| 5 | Direct Wi-Fi (10m + Touch2Wake)   | Mode 3. Valid local architecture.    |
|   | -> Local Server -> OZKEY          | Requires single verification fix to  |
|   |                                   | optimize local server timeout logs.  |
+---+-----------------------------------+--------------------------------------+

| 6 | Direct Wi-Fi (10m + Touch2Wake)   | Mode 4. Mechanically identical to    |
|   | -> Cloud Server + MQTT -> OZPMS   | Mode 5, but entirely cloud-hosted.   |
|   |                                   | App integration naming is clean.     |
+---+-----------------------------------+--------------------------------------+

