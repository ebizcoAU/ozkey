# ozkey-45 — CH584M doorlock simulator: what to emulate on the ESP32-C6-1.9" dev kit

**Status: 🟢 BUILT AND RUNNING ON HARDWARE (updated 2026-08-30).** Everything
proposed below exists and is verified on the bench — this document is now a
record of *why* it is shaped this way, not a plan. What shipped:

| Sketch | Board | Version |
|---|---|---|
| `dlch584m_19` | LockA, Waveshare 1.9" | `dlch584m-0.3` |
| `dlch584m_147` | LockB, 1.47" | `dlch584m-0.3` |
| `bridgeBLE` | ESP32-C6 GEEK bridge | `bridgeBLE-0.10` |

Beyond §3's original scope, the same work also delivered: Thread AND Wi-Fi
fully removed from the lock builds (`isThread()` forced false under
`OZ_NO_MCU` — CH584M has neither radio); continuous BLE advertising from
boot (a gesture-gated lock can never be reached by a bridge); the bridge's
BLE-central role with a pairing allowlist; and the app-facing
`locks`/`pairctl` GATT contract. See `XFtposDecisions-138` §P5/§P6 for the
cross-team detail.

🔴 **Two findings that post-date this proposal and change its assumptions:**
1. **BLE is capped at 3 concurrent connections** by arduino-esp32's
   precompiled `sdkconfig` (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3`). The range
   is 1-70 — 3 is the ESP-IDF *default*, not silicon — but raising it needs a
   rebuilt lib set or native ESP-IDF. XF-138 §P6, **operator decision open**.
2. §1's "two open PM questions" are RESOLVED: pairing transport is **BLE**
   (three-team convergence), and the bridge targets the EXISTING OZKIE
   service (`4f5a4b31-…`), not XF-138's `0000FFF1-4` appendix — because that
   is what the simulator actually serves. The 1:1 mapping for a later swap is
   recorded in `blelock/bridgeBLE/ozblecentral.h`.

---

*Original proposal follows, unchanged.*

**Status when written: 🟡 PROPOSAL — informs `dlch584m_19` scaffolding, not yet built.**
Written 2026-08-29 by **firmware**, at the operator's request, after reading
`docs/DoorLockHW/DL_CH584M_PINOUT.md` and `DL_CH584M_REFDESIGN.md` (the
CH584M doorlock PCB schematic/pinout specs) against the firmware assessment
done earlier the same day for `XFtposDecisions-138` (PM's directive to build
the CH584M simulator on the Waveshare ESP32-C6-LCD-1.9" board).

Companion docs: `XFtposDecisions-138` (the directive this answers),
`docs/ozkey-44.md` (DL-MCU trust boundary — CH584M selected over BF5885AM64),
`blelock/CONTRACT.md` (the existing OZKIE BLE GATT contract),
`blelock/common/ozdoorlock_core.h` (the shared crypto/BLE/bond/dispatch core
this simulator reuses), `blelock/dlock19/dlock19.ino` and
`blelock/doorlock19/doorlock19.ino` (proven hardware bring-up on this exact
board).

---

## 1. Firmware assessment: most of XF-138's ask already exists

Before proposing what to emulate, the load-bearing finding from reading the
current codebase against `XFtposDecisions-138`'s Phase 1-5 task list: **this
is not a from-scratch build.**

- **Phase 1 (LCD + touch keypad)** is effectively done. `dlock19.ino`
  already proves a real, *visible* 12-key touch grid on this exact hardware
  (Waveshare ESP32-C6-LCD-1.9, CST8xx@0x15 touch, ST7789 LCD) — confirmed
  pins, confirmed touch-coordinate transform. `doorlock19.ino` carries the
  production version of the same pins into the shared core.
- **Phase 2/3 (BLE GATT server, sealed envelopes, bond management,
  counter_floor, command dispatch)** already exist in
  `blelock/common/ozdoorlock_core.h` (8,499 lines, shared byte-identical
  between `doorlock.ino` and `doorlock19.ino`): a 6-characteristic BLE GATT
  service (`provision`/`status`/`challenge`/`control`/`member`/`info`),
  X25519/AES-GCM sealed envelopes, bond table, M3/M4 challenge,
  `counter_floor` replay protection, and the verb dispatcher
  (`ozResolveVerb`). Already live, already proven on two boards.
- **The one real architectural seam**: dispatch today ends at
  `ozResolveVerb("lock.unlock", ...)` resolving to a Tuya DP number written
  over UART to an external DL-MCU (real hardware or LockSim). CH584M has no
  external MCU — it drives its own peripherals. For `dlch584m_19`, verb
  resolution needs to end in **local action** (simulated bolt/motor state,
  keypad PIN checked against a local credential store) instead of a UART
  write. That is the actual new work, not a protocol rebuild.
- **New GATT service** matching XF-138's UUID appendix (`0000FFF1-4`,
  Command/Status/OTA/Handshake) is required in addition to/instead of the
  existing `4f5a4b31-...` OZKIE service — server has already confirmed (same
  XF-138 thread) its queue-flush path speaks the exact sealed-envelope
  format the new Command characteristic expects, so the wire *format*
  already lines up; this is about exposing the right characteristic shape,
  not inventing a new protocol.
- `TUYA_TX_PIN`/`TUYA_RX_PIN`/`SRDY_PIN`/`MRDY_PIN` become unused on this
  board — dead weight to strip, not a problem to solve.

Two open questions flagged for a PM call, not decided here: (1) does the
phone still pair directly to the existing OZKIE BLE service, or does
everything route through the bridge via the new UUIDs — i.e. does the
simulator need to serve two GATT services at once; (2) touch-wake is a
known gap on this specific board's touch IC (no INT pin — confirmed in
`doorlock19.ino`'s own header), so the simulator cannot demonstrate
touch2wake-from-sleep the way real CH584M silicon should — a simulator
limitation, not evidence against CH584M's power story.

## 2. What the real CH584M board actually has

From `DL_CH584M_PINOUT.md`/`DL_CH584M_REFDESIGN.md` (48-pin LQFP-48,
7×7mm): power/crystal/debug (pins 1-9), 14-channel TouchKey wired as a 4×3
matrix — TK0-3 rows, TK4-6 columns (pins 10-16), built-in NFC/RFID antenna
+ detect (pins 18-20), UART fingerprint module + power enable (pins 21-23),
L9110S H-bridge motor driver (pins 24-26), TC5020EJ SPI LED driver (pins
27-30), DAC-driven speaker (pins 31-32), reset + tamper buttons (pins
33-34), built-in BLE antenna (pin 35), an **optional segment LCD** — 4
segments, 2 commons (pins 36-41, calculator-style, not graphical — 🔴 see
§4), NRST/BOOT (42-43), 5 reserved GPIO (44-48).

Two peripherals are genuinely external to the MCU regardless of chip
choice: the **fingerprint sensor** (UART module) and the **motor** (via
L9110S). Everything else (TouchKey, NFC/RFID, BLE, optional LCD) is
native/built into CH584M's silicon — the entire point of the vendor's
power argument in `ozkey-44`.

## 3. Emulation proposal, by fidelity tier

### Tier 1 — emulate faithfully; this is the actual point of the exercise

| Real CH584M peripheral | ESP32-C6-1.9" equivalent | Fit |
|---|---|---|
| TK0-6 capacitive 4×3 keypad | CST8xx touchscreen, drawn 4×3 grid | Already built (`dlock19.ino`). Different sensing tech (7 discrete cap-touch channels vs. touchscreen hit-test), identical output to firmware logic — which key was pressed. |
| BLE 5.4 GATT server | ESP32-C6's own BLE 5.3 radio, existing Arduino BLE stack | Already built (`ozdoorlock_core.h`'s 6-characteristic service); needs the new XF-138 UUID service added alongside (§1). |
| Motor (L9110S → bolt state) | No physical motor — simulate bolt state on screen + serial log | Same pattern LockSim already uses for DP 47 `bolt_state`. Logical fidelity is what's being tested, not the H-bridge signal. |
| Battery ADC (VBAT) | No real battery divider — simulate via a UI control or serial command | Same pattern just built into `locksim`'s battery-level feature this session (DP 45 `battery_percentage`) — reuse that exact approach here. |

### Tier 2 — simulate logically (UI gesture/serial command standing in for hardware the dev kit doesn't have)

- **RFID/NFC** — dev kit has no antenna/reader. A "Tap Card" button/gesture
  on screen injecting a fake UID, same pattern LockSim already uses.
- **LED status (TC5020EJ)** — no LED matrix on the dev kit. Render status
  as color blocks on the TFT instead — more debuggable than the real
  product's LED array for our purposes, not a downgrade.
- **Tamper switch** — no physical switch wired. A long-press gesture or a
  second GPIO button, if the dev kit breaks one out.
- **Speaker/DAC chime** — dev kit almost certainly has no audio output.
  Log-line/screen-flash stand-in only.

### Tier 3 — worth doing as REAL hardware, not simulation

The **fingerprint module is UART-attached external hardware regardless of
which MCU sits behind it** — CH584M's own reference design just wires
`UART1_TX/RX` to an off-chip sensor. If we source the same module and wire
it to the dev kit's spare UART pins, we get real sensor behavior instead
of simulated behavior — strictly better fidelity, and CH584M's own board
wouldn't do this any differently. Worth a small hardware buy if
enrollment/matching matters for this milestone.

### Explicitly not worth emulating

- 🔴 **The "optional LCD" (SEG0-3/COM0-1)** — a tiny segment display
  (calculator-style, a handful of segments), not graphical. The dev kit's
  320×170 color touchscreen is a *development-visibility* superset, not a
  stand-in for this — don't shape the simulator's screen to imitate the
  real product's minimal/likely-absent display.
- **SWD, crystal, VBAT divider, power section** — analog/programming
  details specific to CH584M's own silicon; irrelevant to what's actually
  being tested at the OZKIE/BLE protocol layer.
- **Reset button** — already covered by `ozdoorlock_core.h`'s per-board
  contract, which already requires a real `USER_BUTTON` GPIO for factory
  reset. No new work.

## 4. Net scope

The simulator should stay scoped to *keypad input → BLE/OZKIE protocol →
bolt/battery/status output* — exactly what's testable and exactly what the
app/server/bridge integration in `XFtposDecisions-138` actually depends on.
RFID, LED, and speaker are cosmetic stand-ins; fingerprint is the one place
where real hardware is both possible and probably worthwhile instead of
simulating it.

Related: [[ch584m-network-coprocessor-architecture-locked]],
[[ozkey-44-dl-mcu-trust-boundary]].
