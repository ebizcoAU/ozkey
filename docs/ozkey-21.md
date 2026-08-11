# ozkey-21 — Time on the lock, and membership that expires by itself

**Status: DESIGN, approved to write 2026-08-10 by the operator.**
Raised by the operator after noticing the app already carries invitation
duration that the firmware does nothing with.

**Audience: firmware and server.** The app-side half — moving the
membership expiry inside the invite MAC — is XF-87, and **this design does
not work without it**.

---

## 0. Standing instruction

ozkey-18 §0. Two claims in this document are **calculated or inferred and
marked as such**, and §2.3 is a suspected live bug that must be **tested on
hardware before anything is built on it**. Do not treat §2.3 as established.

---

## 1. What the operator asked for

> *"the invited member can access the door for 3 days, then his paired
> doorlock should auto remove and revoke, without him to touch the doorlock
> via BLE… why can't it tell the doorlock to remove that invitation when
> expired?"*

Correct requirement, and it is the OZLODGE tier's core promise — hotel
access is *"checkout is Sunday 10 am"*. Today it cannot be delivered.

---

## 2. Why it does not work today

### 2.1 The app has the field. The lock never sees it.

`packages/ozkey_commissioner/lib/src/member_invite.dart:83` —
`membershipExpiresAtUnix` (QR field `'me'`):

> *"Deliberately **NOT** part of the MAC… the lock **never verifies or even
> sees this field** — it is advisory metadata carried app-to-app so the
> member's own device can… self-expire locally."*

So the expiry is enforced by **the member's own phone**, voluntarily. If
they never open the app, never update it, or shift their clock, the bond
stays live on the lock forever. **The limit is enforced by the party it is
enforced against**, which is not an access control.

The other field, `expiresAtUnix` (`'e'`), is only how long the QR stays
redeemable, and it *is* in the MAC — but the firmware explicitly ignores it
(`ozdoorlock_core.h:2643`, *"parse-and-ignore in v1 (XF-47): the lock has no
clock, so enforcing it would be theatre"*).

### 2.2 A bond has nowhere to store an expiry

The bond record holds `present`, `role`, `label`, `pub`. No time field.
Unlike PIN/RFID, there is no "add bond with a window" frame at all — bonds
are created by a ceremony the lock runs, not by a command an admin sends.

### 2.3 🔴 CONFIRMED ON HARDWARE — temp PIN/RFID expiry has never been enforceable

**Tested 2026-08-10 against DoorA (`doorlock-1.31`) over the real CP2102 MCU
UART, 9600 8N1. No longer a suspicion.**

Method: `blelock/bench/mcu_time_probe.py` stands in for the lock MCU and sends
the frame a real MCU sends — `55 AA 00 1C 00 00 1B`, `0x1C GET_LOCAL_TIME`.

Evidence, from DoorA's own serial log:

```
[TUYA<-] cmd 0x1c (7 bytes)                    <- received AND checksum-valid
[MON] ... mcu=up tx=0 rx=4  →  mcu=up tx=0 rx=10
```

**Re-confirmed from BOTH ENDS simultaneously**, with LockSim driving the MCU
side over the same CP2102 (12:16, 2026-08-10). Timestamps correlate to ~20 ms:

| LockSim (MCU side) | DoorA (module side) |
|---|---|
| `12:16:09.977  TX 55 AA 00 1C 00 00 1B` | `12:16:09.997  [TUYA<-] cmd 0x1c (7 bytes)` |
| 7 requests sent, **7 unanswered** | `mcu=up tx=0 rx=22` |
| `MCU CLOCK: UNSYNCED` | 22 frames parsed, **zero transmitted** |

Three independent measurements now agree — the CLI probe, LockSim, and DoorA's
own counters. The MCU link is demonstrably alive (`mcu=up`, and the new LCD
link dot is green throughout); the time service simply does not exist.

- **The link works.** `rx` climbed 4 → 10 as frames were sent, and `mcu=up`.
  DoorA parsed each frame (a bad checksum logs `bad checksum — dropped`; none
  did).
- **`tx=0`, always.** DoorA has never transmitted a single byte on the MCU
  UART.

Root cause is one line of code. `ozdoorlock_core.h:1298`, in `handleMcuFrame()`:

```c
if (n >= 4 && f[3] == 0x00) return; // MCU heartbeat = link-alive only
```

and every other command falls through to DP handling. `Serial1.write()` appears
exactly **once** in the whole firmware (`:1249`), for pushing DP commands *down*.
**There is no code path that answers the MCU. Not for time, not for anything.**

So the `from`/`to` window we pack into DP 21/23 (`:3150`) is handed to an MCU
that was never told what time it is. **Every time-limited PIN and RFID
credential we have ever issued is, in effect, permanent.**

This is a shipped security defect, not a missing feature. T2 is its fix.

*Original reasoning, now vindicated, retained below.*

PIN/RFID grants carry a window: `ozdoorlock_core.h:3150` —

> *"DP 21/23 RAW value: slot ‖ credential ‖ from(4 BE) ‖ to(4 BE) — the
> layout ozctl.py's dp_grant() mirrors and **the MCU already parses**."*

We assumed the MCU holds an RTC and enforces this. **Tuya's architecture is
the opposite.** From Tuya's MCU protocol documentation:

> *"the **module** comes with a software real-time clock (RTC) that
> **provides time for the MCU**, and therefore, even when the module is
> offline, the MCU can also get the time. The MCU automatically
> synchronizes time… after the module is powered on and connected, it will
> notify the MCU when the time sync is completed."*

**We replaced the Tuya module with our ESP32.** The MCU has been expecting
*us* to serve it the time, and we never implemented that service. So the
`from`/`to` window we send may be evaluated against a clock that was never
set.

~~**Action: bench-test a temp PIN with a window that has already expired.**~~
**Done — see the hardware evidence above. Confirmed.**

This also means the fix in §3 is not only for bonds: **serving time to the
MCU is owed regardless**, because the MCU is a client of it by design.

---

## 3. The design

### 3.1 Where the time comes from — ranked

The operator proposed four sources. Assessed:

| Source | Verdict |
|---|---|
| **Bridge NTP → distribute to locks** | ✅ **Primary.** Bridge has Wi-Fi and mains power. |
| **Piggyback UTC on commands already being sent** | ✅ **Do as well.** Free — the datagram is already in flight. |
| Thread network time sync | ❌ **Rejected — see §3.2** |
| Ask the lock MCU | ❌ **Backwards — the MCU asks *us* (§2.3)** |

**This was already designed.** `CONTRACT.md:480-499`:

> *"Neither doorlock nor bridge32 has an RTC. **Expiry needs UTC only** —
> timezone is not required… every forwarded command **carries a UTC
> timestamp** (free — the datagram is already being sent), plus a slow
> **time beacon** to the lock group… **SECURITY RULE — the clock is
> monotonic-forward only.** Firmware refuses any time [going backward, so
> nothing can] resurrect an expired token."*

Nothing in that needs redesigning. It needs implementing.

### 3.2 Why NOT Thread's network time service

`OPENTHREAD_CONFIG_TIME_SYNC_ENABLE` exists, but:

- It is an **OpenThread extension**. Per the documentation, *"the
  network-wide time service is not specified in any Thread specification"* —
  so it is non-standard and will not interoperate.
- The failure mode is severe: a router-capable device that cannot find a
  neighbouring router supporting time sync **forms its own partition**; a
  non-router device **remains an orphan**.

We would be trading a clock problem for a mesh-partitioning problem, on a
mesh whose reconvergence behaviour has already cost us a week. Rejected.

### 3.3 The crystal changes the economics — ⚠ CORRECTED 2026-08-11

**The original claim below was wrong, and it was load-bearing.** Our PCB does
fit an external 32.768 kHz crystal (the Waveshare dev kits do not document
one). But fitting the part is not using it:

```
CONFIG_RTC_CLK_SRC_INT_RC=y
# CONFIG_RTC_CLK_SRC_EXT_CRYS is not set
```

That is the Arduino ESP32 3.3.11 C6 build config. Arduino ships **precompiled**
IDF libraries, so the RTC slow clock runs on the **internal RC** and no amount
of soldering reaches it — selecting the crystal needs a custom IDF build. So
the ±20 ppm / 1.7 s-per-day figure below does not describe anything we have
ever run.

What is true: the RC is recalibrated against the main crystal while awake
(`CONFIG_RTC_CLK_CAL_CYCLES=576`), so a lock that stays awake or light-sleeps
keeps good time, and system time survives deep sleep
(`CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y`). A production lock that deep-sleeps
for hours drifts on the RC, and **the sync cadence must then be re-derived from
measured drift, not assumed**. Unmeasured as of 2026-08-11.

The daily cadence still stands for the current bench and for any mains-adjacent
lock; it is the deep-sleep battery case that needs the measurement.

*Original text, retained because the conclusion survives and the reasoning does
not:* the operator fitted an external 32.768 kHz crystal; drift drops from the
internal RC's ~±5 % to roughly ±20 ppm — about 1.7 s/day — and it keeps running
across deep sleep. Consequence: the lock needs the time set rarely, not
continuously. A daily beacon is ample.

### 3.4 The rules

1. **Monotonic-forward only.** Never accept a time earlier than the current
   one. This is what stops a replayed or spoofed beacon resurrecting an
   expired membership. From `CONTRACT.md`, non-negotiable.
2. **Unknown ≠ zero.** A lock that has never been told the time must not
   treat every expiry as passed (locks everyone out) *or* as future (locks
   nobody out). It must report `time_unknown` and **fail closed for expiry
   checks on time-limited bonds only** — a permanent bond is unaffected.
3. **Persist across reboot.** Store last-known UTC in NVS on a slow cadence
   (not every tick — NVS endurance). On boot, restore, then let the crystal
   carry it until the next beacon.
4. **Time source must be authenticated — AMENDED 2026-08-11, operator-accepted.**
   The original rule (below) required the beacon to ride the sealed-envelope
   path. **It is not implementable as written, and it aimed at the wrong
   threat.**

   Not implementable: the sealed envelope's keys are **per-bond app↔lock**
   keys. The bridge does not hold them — it is deliberately a relay, not an
   authority. Sealing the beacon would mean giving the bridge crypto authority
   over locks, contradicting the bridge32 role decision. And the app is not the
   time source; NTP/our server is, so sealing app→lock time would protect the
   wrong hop.

   Wrong threat: rule 1 (monotonic-forward) already blocks the
   access-*extension* attack, because extending access needs the clock to move
   **backward**. What is left is the opposite — a forward jump that expires
   credentials early — and monotonic-forward makes that **irreversible**: after
   a spoofed "year 2099" every legitimate time is backwards and is refused
   forever. One packet, permanent brick, factory reset to recover.

   **The rule is therefore replaced by three concrete protections:**
   - the Thread network key authenticates the transport (only a commissioned
     device can inject at all);
   - **monotonic-forward** (rule 1) blocks extension;
   - a **400-day forward-jump cap** blocks the one-packet brick while still
     letting a lock dormant for a year resync in a single step.

   Note the credential itself is unaffected: DP 21/23's `from`/`to` window
   already travels **inside** the sealed envelope. Only the clock *reference*
   is unsealed, and it is not part of the passport.

   *Original rule:* "Time source must be authenticated. A beacon is a command
   like any other and rides the existing sealed-envelope path. An
   unauthenticated time source is an access-extension primitive."

5. **NTP is an optimisation, never the only source.** Measured 2026-08-11 on
   the lab network: DNS resolves `pool.ntp.org`, **UDP 123 times out**. A site
   that blocks NTP would leave every lock `clock=UNKNOWN` and reintroduce the
   §2.3 defect through the network. Our own MQTT connection to the server is
   the time source of record — if the bridge cannot reach our server, there is
   no product anyway.

---

## 4. Work breakdown

### T1 — Clock in the ESP32 *(firmware)* — ✅ BUILT + HARDWARE-VERIFIED 2026-08-11

Monotonic-forward UTC, crystal-backed, NVS-persisted, `time_unknown` state.
Accept time from (a) a sealed beacon, (b) a UTC field on any inbound
command.

### T2 — Serve time to the lock MCU *(firmware)* — ✅ BUILT + HARDWARE-VERIFIED 2026-08-11

Implement the Tuya time service the MCU expects (§2.3). **Owed regardless of
bonds** — it is probably why PIN windows do not work.

### T3 — Bridge distributes time *(firmware, bridge32)* — ✅ BUILT + HARDWARE-VERIFIED 2026-08-11

NTP on the bridge; stamp UTC onto every forwarded command; slow beacon to
the lock group for locks that receive no commands. Bridge is mains-powered,
so cost is airtime only — keep the beacon slow, per ozkey-20 §4.1.

### T4 — Bond expiry field *(firmware)*

Add `expires_at` (uint32 UTC, 0 = permanent) to the bond record and to the
NVS `bondtab` layout. **Additive — existing bonds read back 0 = permanent,
so no migration and no behaviour change for anyone already enrolled.**

### T5 — Enforce it *(firmware)* — 🟢 UNBLOCKED 2026-08-11, not yet built

**§8 DECIDED (operator, via XF-87 §12): DELETE the bond outright on expiry —
do NOT park it as inactive.** His reasoning: the window was explicitly and
knowingly granted upfront and signed into the invite, so there is nothing left
to approve at expiry — it is a pre-authorized outcome, not a decision point.
Parking would only make sense if someone might reinstate it, and reinstating is
what T7 (amend-by-command) is for, *before* expiry rather than after.

Both former blockers are gone: **XF-87 v2 is hardware-verified** (ftpos's
signed-`me` MAC matches ours byte-for-byte, so the invite can carry a
trustworthy expiry), and **the lock now has a clock** (T1/T2/T3 live in
`doorlock-1.54` / `bridge32-1.28`). T5 is the next real work in this document.


On expiry: drop the bond, emit `roster_changed` (reason
`membership_expired`), bump `roster_epoch` (ozkey-19 v2 R5), update the LCD
count. Check on a slow timer and on every use of the bond — **the check at
point of use is the one that matters**; the timer is housekeeping.

### T6 — 🔴 Move `'me'` inside the invite MAC *(app — XF-87, BLOCKING)*

The lock cannot act on an unauthenticated field. Until `'me'` is in the MAC,
T4/T5 have nothing trustworthy to store. **This gates the feature and it is
not our change to make.**

### T7 — Amend by command *(firmware + server)*

Let an admin shorten or extend an existing membership without minting a new
QR. Complements T6 rather than replacing it: T6 works with no network at
all, T7 needs the lock reachable.

---

## 5. Ordering

**T1 + T2 first**, because §2.3 may be a live security defect and T2 is its
fix. Test §2.3 before either.

Then T3 → T4 → T5. **T6 must land in the app before T5 is useful**, so raise
XF-87 now (done) and let ftpos schedule it in parallel.

T7 last — it is convenience, not correctness.

---

## 6. Acceptance

1. ~~§2.3 test first~~ **DONE 2026-08-10 — CONFIRMED, see §2.3.** DoorA
   receives `0x1C` and never answers (`tx=0`). Re-run
   `blelock/bench/mcu_time_probe.py` after T2 and require a served time.

   LockSim now models this honestly and is proven against it — 15/15 on the
   MCU-clock suite: UNSYNCED start, expired PIN → `TIME_UNKNOWN` (fails
   closed), a valid PIN ALSO refused with no clock, module serves time →
   expired PIN → `EXPIRED` / live PIN → `VALID`, and a backwards time refused
   so an expired credential cannot be resurrected.
2. Lock with no time ever set reports `time_unknown` and refuses to honour a
   time-limited bond, while a permanent bond still works.
3. Time set, then a beacon sent with an **earlier** timestamp — refused,
   clock does not move backwards.
4. Member enrolled with a 5-minute membership: door opens at 4 minutes,
   refused at 6, bond gone from `list_bonds`, `roster_changed` emitted with
   `membership_expired`, epoch bumped.
5. **Same test with the bridge powered off for the whole window** — this is
   the operator's actual requirement: no app, no BLE, no server, and it
   still expires.
6. Lock power-cycled mid-membership: expiry survives, and the crystal has
   held the clock close enough that expiry lands within seconds.
7. Existing permanent bonds are untouched by the NVS layout change.

---

## 7. What this does not cover

- **Timezones.** Expiry is UTC only, per `CONTRACT.md:480`. Time-of-day
  schedules ("weekdays 9-5") need a timezone and are out of scope.
- **The trust question.** A lock that accepts time from the bridge trusts
  the bridge not to wind it forward to expire someone early, or hold it back
  to extend access. Monotonic-forward blocks the second, not the first.
  Worth a decision before OZLODGE ships; not a blocker for OZLOCK.
- **G1.** T3's beacon adds airtime and T1 adds a wake source. Both are small,
  both are unmeasured, and both belong in the G1 measurement.

---

## 8. Open question for the operator

**Should an expired membership delete the bond, or park it as inactive?**

Deleting is cleaner and is what "revoke" means everywhere else in the
system. Parking would let an admin re-activate without a new QR ceremony,
which for a hotel guest who extends by a night is a real workflow — but it
keeps key material on the lock past its stated life, which cuts against the
threat model.

My recommendation is **delete**, with T7 (amend-by-command) as the way to
extend *before* expiry. Operator's call.

---

## 7. Status 2026-08-11 — T1/T2/T3 done, verified end to end

`doorlock-1.43`, `bridge32-1.20`. Chain proven on the bench with both locks
attached to the bridge as Thread children:

```
bench/server --MQTT--> bridge --Thread ff03::1--> lock --0x1C--> DL MCU
   utc=1786396938        utc=1786396958        clock=known      tserved=2
```

- **T2 answers.** LockA: `[TUYA->] 55 AA 00 1C 00 08 ...`, `treq` climbing with
  `tserved` behind it. Before 1.42 this was `tx=0` — total silence (§2.3).
- **The unknown-time reply is a real answer**, full length with flag 0, so
  "module present but unfed" is distinguishable from "no service". That
  distinction is what made the rest of this debuggable.
- **T3 multicast reached BOTH children** — LockA and DoorB both went
  `clock=UNKNOWN` → `clock=known` off one `ff03::1` beacon.
- **Codec is cross-checked against LockSim**, which is the other end of this
  UART: `blelock/bench/t2_host.cpp` compiles the real `oztime.h` natively and
  asserts the `0x1C` frame byte-for-byte against `locksim/test`'s fixture, plus
  the monotonic rule and the 400-day cap. 16/16, no board required.

### Known gaps — do not read the above as "ozkey-21 is done"

1. 🔴 **No real DL MCU has ever been observed asking us for the time.** Every
   `0x1C` in this document came from *our own* emulators (`mcu_time_probe.py`,
   LockSim). §2.3 proves the module never answered; it does **not** prove the
   production MCU asks. **Manufacturer question.**
2. 🔴 **Timezone does not exist anywhere in firmware.** `grep` for
   `tz|timezone|utc_offset` across lock and bridge returns nothing. We serve
   UTC in `0x1C`, which is nominally *local* time. If the app writes DP 21/23
   windows in local wall-clock, every temporary credential is wrong by the
   offset — 7 hours in Vietnam. **Action 1, raised with ftpos.**
3. **NTP unusable on this network** — see §3.4 rule 5. The MQTT path carries it
   today; NTP stays in for sites that allow it.
4. **Deep-sleep drift unmeasured** — see §3.3.
5. T4/T5 (bond expiry field + enforcement) not started; T5 still gated on
   ftpos's XF-87.

---

## 8. Tuya's actual clock architecture — and the question it forces (2026-08-11)

Operator research, and it both **validates T2 and exposes a gap in what we
know about the manufacturer's board.**

### 8.1 There are two clocks, and only one of them is a calendar

- **High-speed system oscillator** (16/32 MHz) — present on *both* the lock MCU
  and the wireless module. Runs code, reads the fingerprint sensor, drives the
  keypad, times the 5-second re-lock. **It is not a calendar** and cannot tell
  you it is Tuesday.
- **Real-time clock (calendar time)** — needed for temporary passwords,
  time-limited guest access, and unlock history. Per Tuya's smart-lock MCU
  protocol this is provided **one of two ways**:
  1. **The module's RTC.** The module keeps calendar time, syncs from the cloud
     or the phone whenever it connects, and keeps ticking offline — quoted drift
     **under 1 minute per 24 h**.
  2. **A dedicated external RTC chip** on the lock's main board — DS1302 or
     PCF8563 with its own 32.768 kHz crystal and a **backup coin cell**, so
     calendar time survives even a full battery pull.

### 8.2 What this confirms

**We are the module, so option 1 is our job — this is exactly T2.** The design
is not a workaround for a missing Tuya feature; it is the standard
architecture, and we had simply never implemented our half of it. §2.3's
`tx=0` was us failing to be the module Tuya's protocol assumes.

It also gives us a **drift budget with a number in it**: under 1 min/24 h is
what a Tuya module delivers, so it is the bar our ESP32 clock should meet. Note
§3.3 — we run on the internal RC, not the fitted crystal, so **we have not
shown we meet it.** Still unmeasured.

### 8.3 🔴 The question this forces — manufacturer

Per the operator: **only high-end Tuya locks put an RTC in the MCU.** So which
is our board?

> **Does the lock MCU have a dedicated RTC chip (DS1302 / PCF8563 or similar)
> with a backup cell — or does it depend entirely on the module for calendar
> time?**

The answer changes the severity of everything in this document:

| If | Then |
|---|---|
| **No MCU RTC** (expected for our tier) | We are the ONLY calendar clock. Every temporary credential depends on T2/T3 working, and a lock whose clock is unknown must fail closed. This is the assumption the current firmware is built on. |
| **MCU has its own RTC + coin cell** | The MCU keeps time across battery pulls independently. T2 still matters (it is how that RTC gets *set* and corrected), but the failure mode is far softer, and our unknown-clock handling is belt-and-braces rather than the only thing standing up. |

Add to the manufacturer list alongside ozkey-22 Q0/Q1/Q2b/§7. It pairs with
the other open unknown in §7: **no real DL MCU has ever been observed asking us
for the time** — every `0x1C` came from our own emulators. If the board has its
own RTC chip, it may never ask at all, and that would explain a silence we
have so far only been able to test against ourselves.

### 8.4 Timezone — RESOLVED, and the answer is counter-intuitive

XF-90 §11: DP 21/23 `from`/`to` are **true UTC epoch seconds** (ftpos verified
in their source — the admin picks local wall-clock, but it reaches the wire via
`DateTime.millisecondsSinceEpoch`, which is always UTC-based).

**So we serve UTC to the MCU and apply the timezone offset ONLY to the panel** —
even though `0x1C` is named `GET_LOCAL_TIME`. Serving genuine local time there
would shift every temporary credential by the offset, 7 hours in Vietnam, with
nothing on our side able to detect it. The command's name is a trap, not a
guide. Marked in `serveMcuTimeRequest()` so nobody "fixes" it back.

Shipped in `doorlock-1.50`.
