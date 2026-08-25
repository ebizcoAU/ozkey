# ozkey-43: ozkieOS Feasibility Assessment (Phase 1)

**In response to:** `docs/ProdDev/ozkieOS.md`, System Architecture Directive,
2026-08-23, "All-in-One ESP32-C6 Firmware OS."
**From:** Firmware team.
**Date:** 2026-08-24.
**Status:** Phase 1 deliverable — feasibility report, driver list, memory
estimate, answers to the 10 questions in §10.1.

This is the firmware team's answer, not an edit to `ozkieOS.md` itself —
that file is the architect's and stays as issued.

## Bottom line up front

The architecture is buildable and the crypto/OZKIE core survives almost
entirely intact. The two things the directive's cost model is missing are
not toolchain risk (see Q1 — smaller than assumed) but: **(a) LockSim, our
entire hardware-bug-finding methodology, has no replacement today**, and
**(b) every peripheral driver is unverified against real hardware because
the RX2067 schematic doesn't exist yet.** Recommend approving the
architecture with a phased plan that stands up a driver-level test rig
*before* RX2067 arrives, rather than after.

## Q1 — Which RTOS? Can the current codebase reach this layered design?

We build with `arduino-cli` against the `esp32:esp32:esp32c6` Arduino core
(confirmed: `blelock/Makefile` invokes `arduino-cli compile`, sketches are
`.ino`, current code depends on `Arduino.h`, `Preferences.h`, `BLEDevice`,
`PubSubClient`, `Arduino_GFX`). We are **not** ESP-IDF-native today, but —
correcting the framing in [[ozkieos-all-in-one-firmware-directive]] — this
is a smaller gap than "toolchain migration" implies. The Arduino-ESP32 core
*is* ESP-IDF underneath: `loop()` already runs as a FreeRTOS task, sketches
can call `xTaskCreate()`/`xQueueCreate()` directly today for the §4.1 task
structure, and `Wire.h`/`SPI.h`/`HardwareSerial`/`analogRead()`/`ledcWrite()`
cover I2C, SPI, UART, ADC, and PWM without dropping to native ESP-IDF C. We
already do direct eFuse and mbedTLS calls alongside Arduino APIs
(`blelock/common/ozcrypto.h` mixes `esp_efuse.h`/`mbedtls/*.h` with
`Arduino.h`) — the codebase is already a hybrid, and can stay one.

**What genuinely is new work:** deep-sleep/wake-source configuration
(`esp_sleep_enable_*_wakeup()`), ISR-safe queue handoff for four new
interrupt sources, and writing drivers for peripherals we've never touched
(today the DL MCU owns 100% of keypad/RFID/fingerprint/motor — confirmed:
`ozdoorlock_core.h:833` — "belongs to the MCU and talks to us over the Tuya
UART", `doorlock/doorlock.ino` and `ozdoorlock_core.h` communicate with the
lock mechanism exclusively via `Serial1` Tuya hex frames, never a GPIO/SPI
line). None of that requires abandoning Arduino-ESP32; it requires writing
code we've never had to write because a second MCU always did it.

**Verdict:** refactorable in-place, not a rewrite-from-scratch. Effort is in
new drivers, not in switching frameworks.

## Q2 — Can the current codebase be refactored into this layered architecture?

Yes, with the caveat that "layered architecture" (L0–L4) is mostly an
organizational relabeling of code that already exists in a flatter form:

| §3.1 layer | Current equivalent | Disposition |
|---|---|---|
| L0 RTOS | Implicit (Arduino-ESP32 default task + loop) | Needs explicit tasks per §4.1, not new capability |
| L1 HAL | `Wire`/`SPI`/`HardwareSerial`/GPIO via Arduino core | Already available, unused for peripherals today |
| L2 Peripheral drivers | **Does not exist** — DL MCU owns this | New code, ~7 drivers (§6) |
| L3 Core services | `ozcrypto.h`, bond store, `ozdoorlock_core.h` txlog/envelope/verb logic | Reuse near-100%, see Q9 |
| L4 Verb dispatcher | `ozResolveVerb()` (confirmed present: `blelock/common/ozdoorlock_core.h`, `ozprofile.h`, `doorlock/doorlock.ino`) | Reuse — already maps OZKIE verbs to actions, just needs new action targets (driver calls instead of DP frames) |

The single biggest *structural* blocker isn't code, it's that the DP
verb-resolution path (`ozResolveVerb()` → DP frame → UART) becomes
`ozResolveVerb()` → direct driver call. That's a real rewrite of the
dispatch tail, but the front half (envelope open, bond check, role gate,
capability check) is transport/target-agnostic already and carries over
untouched.

## Q3 — Memory footprint

Measured just now, current shipped firmware (`doorlock-2.21`,
`PROFILE=tuya-luona-ds013-t3`, board 19, N8 part):

```
Sketch uses 2152590 bytes (64%) of program storage space. Maximum is 3342336 bytes.
Global variables use 82244 bytes (25%) of dynamic memory, leaving 245436 bytes for local variables. Maximum is 327680 bytes.
```

(Matches the 08-23 estimate almost exactly — 2.21 added ~600 B flash over
2.19.) That's the **entire current firmware including the Tuya DP
translation layer we'd be deleting.** Budget headroom against the target
(ESP32-C6 N8: 8 MB flash / 512 KB SRAM per the directive, though our
`default_8MB` partition table caps program storage at ~3.34 MB, not 8 MB —
worth flagging to whoever sized "8 MB Flash" in §10.1 row 3, since the
usable app partition is much smaller than raw chip flash):

- Flash: 36% free today (1.19 MB) before adding any new driver code, and we
  *remove* `ozDpForwardable()` and the DP-parsing/hex-command surface, which
  is not large but isn't zero.
- RAM: 75% free (245 KB) — comfortable margin for driver buffers, fingerprint
  template caching if done on-MCU, and additional FreeRTOS task stacks
  (§4.1 sums to ~23 KB across 7 tasks — trivial against 245 KB headroom).

No hard blocker here. RFID/fingerprint library choice (e.g. Adafruit's
`Adafruit_Fingerprint` pulls in its own buffers) is the main unknown left
to quantify once specific parts are chosen — can't be estimated further
without the RX2067 BOM.

## Q4 — Worst-case unlock latency

Cannot be measured without real motor/limit-switch hardware (RX2067 doesn't
exist). What we can state from the existing crypto/dispatch path: envelope
open + bond/role check + verb resolution is already sub-100ms on-device
today (it runs before the DP frame is even written to the UART) — that part
of the budget is already met. The unknown is entirely in the new territory:
keypad debounce (spec says 50ms) + motor actuation time (spec says <1s) is
the real budget, and today we have zero data because the DL MCU has always
owned that latency and we've only ever measured round-trip-to-UART-ack, not
mechanical actuation. This needs a bench measurement once any motor driver
exists, even on a non-RX2067 stand-in H-bridge + hobby lock mechanism.

## Q5 — Deep sleep + Wi-Fi keep-alive

Thread SED already does this today (5s poll, confirmed working per
[[ozkieos-all-in-one-firmware-directive]] — "unblocked, ready"). Wi-Fi does
not currently sleep between heartbeats; today's Wi-Fi doorlock firmware
stays active because MQTT/TLS session resume from deep sleep is more
expensive than the piggyback model needed to bother optimizing (the DL MCU
handled physical security in deep sleep; the C6 was "just" a comm bridge).
This is new work, not a port: MQTT-over-TLS session resumption after
`esp_deep_sleep_start()` wake, balanced against the §5.3 300s heartbeat
target. Estimate this is the single largest unknown-effort item in the
whole directive — bigger than any individual peripheral driver — because
it's a correctness-sensitive interaction between three subsystems (Wi-Fi
reconnect, TLS resume, MQTT session) none of which we've had to make sleep
before.

## Q6 — How to test drivers without the RX2067 schematic

**This is the directive's real gap, and it's bigger than the directive's
own framing.** §10.1 Q6 treats "no schematic" as a scheduling blocker.
It's also an *architectural* problem: **LockSim (`locksim/`) exists
specifically to emulate the Tuya DL MCU on the other end of the UART**
(confirmed: `locksim/README.md` — "unencrypted 4-wire UART bus... Tuya MCU
Hex Serial Protocol"). Remove the second MCU and LockSim has nothing left
to stand in for. It found every hardware bug logged this month
([[session-state-2026-08-18]] DP encoding mismatch, XF-126 sealed-unlock
bypass, XF-128 PIN-routing bug) — all of them via LockSim's ability to
inject and observe UART frames without real hardware in the loop. ozkieOS
deletes that capability and the directive doesn't propose anything in its
place.

**Recommendation:** before Phase 3 implementation starts, stand up a
minimal driver-level test harness — either (a) a breadboard rig with a
cheap real RC522/AS608/H-bridge wired to a dev C6, decoupled from RX2067's
specific pinout, so drivers can be written and unit-tested against real
silicon early, or (b) a software mock layer at the L2 driver-interface
boundary (§3.1 already specifies "common API" driver interfaces — that
seam is exactly where a LockSim-successor could inject fake
keypad/RFID/fingerprint events without hardware). (b) is cheaper and
directly reuses the "inject a fake event, observe the dispatcher's
reaction" pattern LockSim already validated; (a) is still needed eventually
for real timing/electrical behavior. Raise this now — it changes Phase 1/2
sequencing, not just Phase 3.

## Q7 — Generic drivers across chip variants

Achievable at the API level (§3.1's "common API" driver interfaces already
describe this correctly) but not for free: MIFARE vs. ISO14443A UID
lengths differ (4–7 bytes per §6.2, already flagged in the directive
itself), and fingerprint sensors vary in template format and storage
capacity per vendor. Design the L2 interface around capability negotiation
(what §8.1 peripheral reporting already does) rather than a fixed contract,
and this is normal driver-API work, no unusual risk.

## Q8 — Battery life improvement, business case

Cannot be validated without hardware measurement — this needs current-draw
numbers off a real RX2067 board in each sleep state, which don't exist yet.
Directionally correct that eliminating the DL MCU's own power draw (which
we've never measured directly, since it was always the manufacturer's
concern) should help, but "multi-year" vs. today's actual field battery
life isn't in our data. Flag this as unvalidatable until Phase 4 hardware
testing, not a Phase 1 answer.

## Q9 — How much OZKIE/crypto code is reused

**Most of it**, confirmed by reading the actual files, not estimating:

- `blelock/common/ozcrypto.h` — X25519 (via `mbedtls/ecp.h`), AES-GCM
  (`mbedtls/gcm.h`), HKDF (`mbedtls/hkdf.h`), eFuse key storage
  (`esp_efuse.h`/`esp_efuse_chip.h`) — **100% reusable**. These are mbedTLS
  and ESP-IDF eFuse calls already, not Tuya-specific, not even
  Arduino-specific beyond `Preferences.h` for a couple of NVS reads.
- Bond store, envelope seal/open, txlog, verb resolver
  (`ozResolveVerb()`), capability reporting — all operate on OZKIE verbs
  and bonds, never touch a DP frame directly. **Reusable essentially
  as-is**; only the dispatch *targets* change (driver calls instead of
  `ozDpForwardable()` → UART write).
- What's genuinely Tuya-specific and gets deleted per §9.2: DP parsing/hex
  command construction, `ozDpForwardable()`, the DP catalogue/profile JSON
  files. This is real but is a minority of the codebase — the crypto and
  OZKIE layers are the bulk of the logic, DP translation is comparatively
  thin glue.

## Q10 — OTA for OS + drivers

No new problem here beyond what exists today — `ozpower.c`/OTA manager per
§3.1 L3 is new code, but OTA-over-MQTT/REST with a secure-update path is
already a solved problem in the current Wi-Fi firmware (`bridge32`/doorlock
both already do MQTT-triggered OTA). Splitting "OS" vs. "driver" update
payloads (implied by the directive's phrasing) needs a partition-table
decision (A/B slots vs. single app slot with drivers statically linked in)
— recommend static linking for v1 (single combined image) unless field
data shows driver-only updates are needed often enough to justify the
added partition-table and rollback-safety complexity.

## What the directive's own cost model is missing

1. **LockSim replacement is not optional, and belongs in Phase 1/2, not
   deferred.** See Q6.
2. **§12 asks whether to archive the legacy Tuya code — this needs an
   operator decision, not a firmware one**, per [[ozlock2-is-not-firmware-doc]]/
   [[pm-role-and-ozapp-channel]] convention: the DP catalogue in
   `profiles/tuya-lock-catalogue.json` is also the only record of a real
   supplier map we ever obtained (per
   [[ozkieos-all-in-one-firmware-directive]]) — archiving is reversible,
   deleting is not, and firmware should not make that call unilaterally.
3. **XF-128 (PIN/RFID/fingerprint blocked on missing Tuya raw payload
   layouts) fully dissolves under this architecture** — this is the
   strongest argument *for* ozkieOS and isn't in the directive's own §13
   "Bottom Line" list. Worth the architect/PM knowing it as a concrete,
   already-filed bug this closes, not just an abstract "peripheral
   flexibility" bullet.
4. Q3's "8 MB Flash" framing overstates usable app-partition budget by our
   current partition scheme (~3.34 MB usable, not 8 MB) — not a blocker,
   just worth correcting before anyone budgets driver code against the
   wrong number.

## Recommendation

Approve the architecture (§12 decision 1: **(a) yes**). Do not decide
archive-vs-delete on the legacy Tuya code without the operator (§12
decision 2: **defer to operator/PM**, recommend (a) archive given the DP
catalogue's standalone value). Sequence Phase 1→2 to include standing up a
software mock at the L2 driver boundary (Q6) so driver work in Phase 3 can
start without the RX2067 schematic in hand, rather than blocking on it.

Related: [[ozkieos-all-in-one-firmware-directive]],
[[our-dp-map-is-invented]], [[pin-writes-land-on-navigation-volume]],
[[two-heartbeat-builders-diverge]].
