# ozkey-22 — Reset, in both directions, and the credentials a factory reset currently leaves behind

**Status: DESIGN, requested by the operator 2026-08-10.**
Blocked on external input: **§6 Q0 (does the DL MCU wipe its own credentials?)**
and §6 Q1 (its slot count). Both are manufacturer questions.

**Audience: firmware.** Contains a **live security gap (§2)** that exists today
in shipped behaviour and is independent of the new feature.

---

## 0. What the operator asked for

> *"We should add remote door lock reset… ESP32 will send reset cmd to DL MCU
> for reset as well — this is called soft reset. Likewise MCU sends reset to
> ESP32 on hardware reset (factory reset)."*

Two directions, and neither exists today. Investigating them turned up a third
thing that is worse than either.

---

## 1. What exists today

| Path | Status |
|---|---|
| Remote factory reset of the **ESP32** (MQTT `op:"factory_reset"` / `"unpair"`) | ✅ works |
| Physical 5 s BOOT hold → ESP32 factory reset | ✅ works |
| ESP32 → MCU reset (**soft reset**) | ❌ does not exist |
| MCU → ESP32 reset (**physical factory reset on the lock body**) | ❌ does not exist |
| Factory reset clearing **MCU-held credentials** | ❌ **does not happen — §2** |

---

## 1a. Naming — DL MCU vs OZKIE MCU (operator, 2026-08-10)

Two microcontrollers, and this document originally blurred them. Adopted
going forward, everywhere:

- **DL MCU** — the lock controller. Owns the motor, the keypad, the RFID
  reader, the fingerprint sensor, **the credential store**, and **the physical
  factory-reset button on the lock body**.
- **OZKIE MCU** — our ESP32-C6. Owns Wi-Fi/Thread/BLE, the bond table, the
  ceremony keypair, the sealed-envelope protocol. This is what Tuya's docs call
  "the module".

The distinction is not cosmetic: **most of §2 below turned on which chip owns
the button, and I got it wrong until the operator corrected it.**

---

## 2. 🔴 THE LIVE GAP — a factory reset does not remove anyone's PIN

> **CORRECTED 2026-08-10 after the operator's challenge (XF-88 §7).** The
> original §2 asserted a single gap. There are two reset paths with two
> different owners, and conflating them made this both wider and narrower than
> stated. The corrected decomposition is §2.1.

### 2.1 Three reset paths, three different answers

| Path | Initiated by | Who should wipe the DL MCU's credentials |
|---|---|---|
| **Physical button on the lock body** | **DL MCU** (it owns the button) | **The DL MCU itself, locally.** Self-contained. Then it tells the OZKIE MCU via `0x34 0x0A` so our side resets too. |
| Physical BOOT hold on the OZKIE MCU board | OZKIE MCU | OZKIE MCU must instruct the DL MCU |
| Remote factory reset (MQTT `op:"factory_reset"`) | OZKIE MCU | OZKIE MCU must instruct the DL MCU |

**The operator's point, and it is correct:** for the first path the wipe should
not depend on the wireless side at all. The chip that owns the button also owns
the credentials; clearing them is its own local responsibility, and routing that
through `0x34 0x0A` → OZKIE MCU → DP 22/24 enumeration would be an absurd
round trip through a component that does not own either thing.

**Consequences of the correction:**

- **R2's enumeration is NOT needed for the physical-button path.** It is needed
  only for the OZKIE-MCU-initiated paths (rows 2 and 3), where our chip is the
  initiator and the DL MCU has no idea a reset happened.
- §6 Q1 (slot count) therefore blocks *less* than stated — it blocks remote and
  BOOT-hold resets, not the physical lock-body reset.
- **R1 is not the mechanism for the physical path's credential wipe. It is only
  the notification** that lets the OZKIE MCU reset itself in sympathy.

### 2.2 What is definitely still broken on our side

`factoryReset()` (`ozdoorlock_core.h:1077`) wipes the OZKIE MCU: the `blelock`
NVS namespace (ceremony keypair, bond table, nonce replay cache), the `txlog`
door-event buffer, and OpenThread's persisted dataset.

**It sends nothing to the DL MCU.** `Serial1.write()` is never called on this
path. So for **rows 2 and 3** — BOOT-hold and remote factory reset — the lock
ends up unowned, unpaired, on a new mesh, **and every PIN and card the previous
owner enrolled still opens it.**

The function's own comment states the intent:

> *"an owner who resets a lock must not inherit the previous owner's identity,
> and a new owner must not be able to reuse a captured pairing secret."*

Achieved for **our** identity; not at all for **the credentials that physically
open the door**. That is the sold-house case XF-46 §1 exists for, and for the
remote path it is real today.

### 2.3 🔴 What we do NOT know — and it is the operator's actual question

**Does the DL MCU wipe its own credential store on a physical button reset
today?**

We cannot answer this from our side, and we have never tested it, because:

- **Our bench has no DL-MCU-wired reset button.** What we call "the physical
  factory reset" on the bench is the **BOOT button on the ESP32 board** — that
  is row 2, not row 1. The real lock-body button path has never been exercised
  here at all.
- The DL MCU runs the lock manufacturer's firmware, not ours.

Standard Tuya lock behaviour probably does clear credentials on a local factory
reset, but *probably* is not good enough for a claim that a reset lock is safe
to hand to a new owner. **This is a manufacturer question and it is now the
highest-value unknown in this document** — see §6 Q0.

---

## 3. What the Tuya protocol actually gives us

*Verified against Tuya's door-lock MCU protocol documentation, 2026-08-10.*

### 3.1 MCU → module factory reset: `0x34`, sub-command `0x0A`

> *"The MCU can locally reset the module to factory settings through this
> command. The module will be reset with data erased and enter low power mode."*

Module replies `0x34 0x0A` with `data[1]`: `0` = success, `1` = failure.

This is exactly the operator's second direction, it is standard, and it uses the
**same `0x34` envelope as the time sub-commands** already added in ozkey-21
(`0x01` subscribe, `0x02` push). Sub-command dispatch on `0x34` therefore needs
to exist properly rather than as a time-only special case.

### 3.2 module → MCU credential wipe: **no such command exists**

Tuya's lock protocol defines **no** DP or command for "clear all credentials".
It has per-slot delete only — DP 22 (PIN) and DP 24 (RFID), each taking a
2-byte slot number, which we already build (`ozSemGrantValue`, `dp_delete`).

So a credential wipe must be **an enumeration**: delete slot 0..N-1 for each
credential type. That requires knowing N (§6 Q1), and it is not atomic — a wipe
interrupted halfway leaves an arbitrary subset alive.

---

## 4. Design

### 4.1 R1 — Handle `0x34 0x0A` from the MCU  *(firmware)*

Physical factory-reset gesture on the lock body → MCU sends `0x34 0x0A` → we run
`factoryReset()`.

- Reply `0x34 0x0A` with `0` **before** starting, because `factoryReset()`
  platform-resets and never returns. A success byte sent after it is a byte
  never sent.
- This is the first time we will ever answer the MCU (`tx` has been 0 for
  everything except forwarded DP commands — see ozkey-21 §2.3), so it also
  proves the reply path works.
- **Gate it.** An unauthenticated "wipe yourself" command on a UART is a
  denial-of-service primitive if that UART is ever reachable. On this hardware
  the wire is inside the door, so physical access is already game over — but
  record the assumption rather than leave it implied (§7).

### 4.2 R2 — 🔴 Wipe MCU credentials as part of factory reset  *(firmware)*

**This is the §2 fix and it is not optional.** On every factory reset, before
anything destructive to our own state:

```
for slot in 0..N-1:  send DP 22 (delete PIN, slot)
for slot in 0..N-1:  send DP 24 (delete RFID, slot)
```

**Ordering is the whole risk.** `factoryReset()` already carries a comment about
exactly this class of bug (2026-08-02: *"everything destructive must happen
BEFORE the OpenThread reset below, because otInstanceFactoryReset() performs a
PLATFORM RESET and never returns"*). The MCU wipe must come **first**, before
the NVS clear and long before the OpenThread reset, or it is dead code for the
same reason.

Pace the frames — the MCU has a serial buffer and no flow control beyond
MRDY/SRDY. Spacing and an overall timeout, not a tight loop.

### 4.3 R3 — Soft reset: ESP32 → MCU  *(firmware + server)*

The operator's first direction. "Soft" = clear the MCU's credentials and
operational state **without** destroying the ESP32's ownership/bonds.

Use case: a hotel room turning over. Wipe every guest PIN and card; keep the
lock owned, paired, on the mesh, with its bonds intact.

Mechanically it is R2's enumeration without the ESP32-side wipe. Exposed as a
new sealed verb (`{"kind":"mcu_reset"}`) over the existing OZKIE path — admin
bond only, same role gate as any credential verb.

### 4.4 R4 — Report what actually happened  *(firmware)*

A reset that silently half-succeeds is worse than one that fails.

- Count deletes acknowledged vs sent.
- Emit `roster_changed`-style uplink with the outcome, and bump `roster_epoch`
  (ozkey-19 R5) so the app resyncs.
- **Never report success for the MCU half unless the MCU acknowledged it.** Per
  XF-84 §14's courier rule, and per ozkey-21 §2.3 — where we discovered the MCU
  answers nothing at all, which means R4 may be unimplementable until we know
  whether DP 22/24 are acknowledged (§6 Q2).

---

## 5. Ordering

**R2 first** — it is the live gap, and it is the same enumeration R3 needs, so
R3 becomes nearly free afterwards.

Then R1 (`0x34 0x0A` handler), then R3 (remote verb), then R4 (reporting, gated
on Q2).

R1 has a useful side effect worth taking early: it forces proper sub-command
dispatch on `0x34`, which the ozkey-21 time work currently handles as a
one-off.

---

## 6. 🔴 Open questions — BLOCKING, and not ours to answer

0. 🔴 **Does the DL MCU wipe its own credential store on a physical
   lock-body factory reset?** THE key question (§2.3). If yes, the
   sold-house case is already safe on that path and R2 shrinks to the
   remote/BOOT-hold paths only. If no, no reset by any route makes a lock
   safe to resell, and that is a product-blocking defect we cannot fix in
   our own firmware. **Manufacturer question — route via PM/Nexus.**

1. **How many credential slots does the MCU have, per type?** R2/R3 cannot be
   written without N. Guessing high wastes UART time and may error on
   out-of-range slots; guessing low leaves credentials alive, which is the
   entire bug. **Needs the lock manufacturer** — route via the PM/Nexus channel.
2. **Does the MCU acknowledge DP 22/24 deletes?** Determines whether R4 can
   report truthfully or only "sent". ozkey-21 §2.3 showed the MCU never answers
   `0x1C`, so this cannot be assumed either way — it must be measured with
   LockSim standing in, then confirmed on the real MCU.
2b. 🔴 **What does the DL MCU actually report to the module, and does any of
   it carry credential material?** We handle exactly two of its DPs — 8
   (access result) and 5 (battery). We have no enumeration of the rest.

   Until 2026-08-11 every unrecognised DP was hex-encoded **in full** and
   published to the server as a door-log line. So if the MCU reports an
   entered PIN, a card UID or a fingerprint identifier in any DP we have not
   classified, we were shipping it off-device by default, with nobody having
   decided to — straight through XF-47's no-plaintext-credential rule.

   **Fixed on our side** (`handleMcuFrame()` now logs unclassified frames to
   the serial console only and publishes shape — dp id / type / length — never
   the value). But the fix is a guard, not an answer: **we still do not know
   what production hardware sends.** Please provide the DP list the lock
   emits, so the safe ones can be handled properly and the rest stay contained.

   Found because LockSim modelled keypad entry as DP 1 carrying the digits.
   That was our own invention and is now corrected — a lock reports the
   RESULT, not the keystrokes — but it exposed a real firmware path.

3. **Does a factory reset need to clear fingerprints too?** DP 3 reports
   fingerprint results but we have no fingerprint enrol/delete DP in our set.
   If the MCU stores fingerprints, §2's gap is wider than PIN/RFID and we
   currently have no mechanism at all.

**Q1 and Q3 both mean §2 may be only partially fixable by us.** That is worth
knowing before anyone states that a factory reset makes a lock safe to resell.

### Server team — confirming blast radius on 2b, 2026-08-11

Checked whether any of the raw hex-encoded DP data firmware describes ever
reached `ozlockserv`, independent of firmware's own fix. It did not.
`SUB_LOG` was removed from this server entirely on 2026-07-31 — the
current subscribe list is eight explicit topics (`enroll`, `heartbeat`,
`member_request_remove`, `member_ack_remove`, `uplink`, lock/bridge
`presence`, bridge `liveness`), each with a legacy-root pair, **no
wildcard subscription anywhere**, and no `locks/+/log` topic at all. The
code comment from that removal states it plainly: *"door events are never
delivered to this process."* Confirmed still true today by reading the
actual subscribe call, not just the comment.

So regardless of what firmware was publishing to that topic before
today's fix, this server never stored it, never served it via any
endpoint, and never had it in reach of the app. Whatever risk 2b
describes was real on the wire and is worth firmware's fix regardless —
but it did not propagate past the broker on the server side.

---

## 7. Assumptions recorded

- The MCU UART is inside the door. Anything with physical access to it can
  already remove the lock, so an unauthenticated `0x34 0x0A` is not a new
  exposure. **If a future design routes this UART anywhere externally
  accessible, R1 must gain authentication first.**
- `factoryReset()` never returns. Every design here is ordered around that.

---

## 8. Acceptance

1. Physical reset gesture → MCU sends `0x34 0x0A` → ESP32 answers `0` → wipes.
   Verify with LockSim first (it can send the frame today), then real hardware.
2. **The §2 regression test:** enrol a temp PIN, factory reset, then try that
   PIN. It must be refused. **This test fails on every firmware we have ever
   shipped** — it is the proof R2 works.
3. Soft reset (R3): guest PINs gone, bonds and mesh intact, lock still owned.
4. Reset with the MCU UART disconnected: must not report credential removal it
   could not perform.
5. Power-cut mid-wipe: on reboot, the lock must not present itself as fully
   reset if the MCU half did not complete.

---

## 7. 🔴 DP ALLOCATION REQUEST — the keypad pairing gesture (manufacturer)

**Raised 2026-08-11.** Belongs with §6's questions; route via the PM/Nexus
channel with Q0 and Q1.

### The gap

On production hardware **the keypad belongs to the DL MCU.** Our OZKIE MCU
opens its BLE maintenance window on a *local touch* — but in the real product
our board has no touch panel. So **a member standing at the door has no way to
make the lock advertise**, and member enrolment is unreachable.

This has been invisible because our dev boards happen to carry their own touch
screen wired to the ESP32 (CST8xx @ 0x15). That is bench-only hardware. It is
the same class of mistake as ozkey-21 §2.3 and ozkey-22 §2: **reasoning about
the OZKIE MCU as though it were the whole lock.**

It is also already documented as load-bearing —
`ozdoorlock_core.h` calls the touch path *"the M3 PREREQUISITE, not a
convenience"*, because BOOT is inside the door and the keypad is outside:

> *"without a touch path a member standing at a commissioned lock has no way
> to make it advertise, so member_enroll — and Wi-Fi/ECO owner unlock, and
> member unlock — are unreachable no matter how correct the ceremony is."*

That reasoning is correct and the mechanism it relies on does not exist in
production.

### What we are asking for

**A DP the DL MCU emits when the user performs a designated pairing gesture on
the keypad** (long-press, or a key combination — the manufacturer's choice, we
have no preference as long as it is deliberate and not reachable by accident).

- Direction: DL MCU → module, standard `DP_REPORT`.
- Type: BOOL, value 1. No payload needed; the event *is* the message.
- Frequency: rare, user-initiated.

**We have NOT chosen the DP number.** `PAIRING_REQUEST_PROPOSED = 60` exists
in LockSim purely so firmware can be written and tested before allocation. It
is a placeholder: the low DP space is the manufacturer's and their MCU already
uses IDs we cannot enumerate, so picking one ourselves risks a silent
collision with a real credential or status DP. **Please allocate.**

### Why this is not a security regression

The BLE window is deliberately physical-presence-gated (XF-52 §4: there must
never be a remote verb that opens it). This preserves that exactly — the
gesture is on the keypad, outside the door, in the user's hand. It moves which
chip observes the press; it does not make the window remotely reachable.

### Status

- **LockSim**: implemented, clearly marked PROPOSED, button
  *"⌨ Keypad Pairing Gesture (proposed DP 60)"*.
- **Firmware**: not implemented — deliberately, pending a real DP number.
- **Blocking**: production member enrolment. Not bench testing, which works
  via the dev board's own touch panel.
