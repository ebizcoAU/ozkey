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

### 3.3 The crystal changes the economics

The operator fitted an **external 32.768 kHz crystal** to the ESP32.
Drift drops from the internal RC's ~±5 % to roughly **±20 ppm** — about
**1.7 s/day** — and it keeps running across deep sleep.

*Figures are the standard specifications for the two oscillator types, not
measured on our board.*

Consequence: the lock needs the time **set rarely**, not continuously. A
daily beacon is ample, and a lock that misses several days is still accurate
to seconds. Without the crystal this design would need constant resync and
would fight G1. **It is what makes the feature affordable.**

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
4. **Time source must be authenticated.** A beacon is a command like any
   other and rides the existing sealed-envelope path. An unauthenticated
   time source is an access-extension primitive.

---

## 4. Work breakdown

### T1 — Clock in the ESP32 *(firmware)*

Monotonic-forward UTC, crystal-backed, NVS-persisted, `time_unknown` state.
Accept time from (a) a sealed beacon, (b) a UTC field on any inbound
command.

### T2 — Serve time to the lock MCU *(firmware)*

Implement the Tuya time service the MCU expects (§2.3). **Owed regardless of
bonds** — it is probably why PIN windows do not work.

### T3 — Bridge distributes time *(firmware, bridge32)*

NTP on the bridge; stamp UTC onto every forwarded command; slow beacon to
the lock group for locks that receive no commands. Bridge is mains-powered,
so cost is airtime only — keep the beacon slow, per ozkey-20 §4.1.

### T4 — Bond expiry field *(firmware)*

Add `expires_at` (uint32 UTC, 0 = permanent) to the bond record and to the
NVS `bondtab` layout. **Additive — existing bonds read back 0 = permanent,
so no migration and no behaviour change for anyone already enrolled.**

### T5 — Enforce it *(firmware)*

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
