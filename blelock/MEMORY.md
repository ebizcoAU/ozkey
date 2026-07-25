# blelock — handoff memory (2026-07-25)

> **Why this file exists:** the last several sessions' firmware work (GEEK
> bridge bring-up, blecomm partition debugging, mode-taxonomy consolidation)
> was done by an assistant also carrying app-dev duties (BANOI/MAOI). That
> split stopped working — this file hands the firmware thread back to
> whoever on the ozkey team picks it up next, so nothing has to be
> re-derived from scratch. The assistant is returning to app-dev-only scope.

## Start here

Read **`docs/ozkey-09.md`** first — it's the consolidated mode taxonomy +
gap analysis, written 2026-07-24 specifically because the design docs
(ozkey-01→08) had drifted from what bring-up actually proved. Everything
below is a snapshot; ozkey-09's gap table is the living punch list.

## ⚠ Uncommitted work on disk — do not lose this

None of the following is committed. Repo is also 1 commit ahead of
`origin/main` (unpushed: `b924a26`, the bridge32/threadcomm bring-up).

| Path | What it is | Status |
|---|---|---|
| `blelock/blecomm/blecomm.ino` (modified) | Crypto Increment-1 diff | Compiles clean (47% flash on N8 w/ correct partition scheme), **not yet flash-tested** |
| `blelock/blecomm/ozcrypto.h` (untracked) | Same crypto work, new file | Same as above |
| `blelock/GeekDisplayTest/` (untracked) | ST7789 pin-map + QR bring-up for the GEEK's onboard display | Pin map + offsets empirically tuned and confirmed working on real hardware; QR settled at 72 chars/V3 (see below) |
| `docs/ozkey-09.md` (untracked) | Mode taxonomy consolidation + gap list | Complete, not yet reviewed/signed off by the team |

## What's actually verified on real hardware (as of 2026-07-24/25)

- **`bridge32.ino`** flashed to the GEEK (ESP32-C6, confirmed N16) —
  **running**, advertising `OZBRIDGE` over BLE:
  ```
  b-98a316a7e638 mac=98:A3:16:A7:E6:38
  [BLE] advertising as OZBRIDGE
  ```
  Wi-Fi/Thread join itself not exercised beyond this in the current session.
- **GEEK's ST7789 display** (candidate pins from a HA community thread, since
  Waveshare never published an official GEEK pinout) — pin map confirmed
  correct after tuning `PANEL_OFFSET_X`/`Y` (final: `45`/`48`) to eliminate
  stray left/bottom white slivers (unaddressed-GRAM artifact, not a wiring
  fault). See `GeekDisplayTest.ino` inline comments for the full offset
  story.
- **QR-on-this-screen capacity**, empirically walked from 16→32→64→72→77
  (max)→80/96 (deliberately broken to confirm the failure path) chars:
  - **V3 (29×29 modules, 116px, 4px/module) tops out at exactly 77 alphanumeric
    chars** at ECC_LOW (`4 + 9 + 11×⌊n/2⌋ + 6×(n mod 2) ≤ 440 bits`). 78+ chars
    hard-fails encoding (caught by a `qrcode_initText()` return check added
    for this).
  - **V4 (33×33, 132px) technically encodes more but fails to *scan*** on this
    135px-tall screen — only 3px of margin left, confirmed unreadable.
  - **Settled config: 72 chars, V3** — leaves ~5 chars of headroom below the
    77-char ceiling, so future payload growth (a checksum, a version tag)
    doesn't silently tip into the broken V4 regime.
  - This matters beyond the bring-up test: ozkey-08 §3's factory-pubkey QR
    trust anchor needs real character budget (device_id + hex/base64
    pubkey), and this is the first empirical data point on what this exact
    screen can actually carry.
- **`blecomm.ino`** (N8 Touch-LCD board) compiles clean at ~47% flash
  (1,575,490 / 3,342,336 bytes) with **`FlashSize=8M` +
  `PartitionScheme=default_8MB`** ("8M with spiffs, 3MB APP/1.5MB SPIFFS").
  An earlier "sketch too big — 120%" error was purely a leftover board-menu
  setting from GEEK testing (Partition Scheme was on the small 4MB/1.25MB-app
  default), not a real code-size problem.
- **`threadcomm.ino`** — not flashed this session.

## Gotchas learned the hard way this round (save yourself the time)

1. **Partition Scheme and Flash Size are independent Tools-menu settings.**
   Flash Size just tells esptool the chip's true physical size; Partition
   Scheme is what actually carves it into app/filesystem slots. Switching
   boards (GEEK N16 ↔ Touch-LCD N8) without re-checking *both* is the classic
   "sketch too big" trap. **`blelock/flash.sh [4M|8M|16M] [sketch-dir]`
   already encodes the right FQBN per size — use it instead of the IDE menu
   by hand when possible**, or check `blelock/blelock/TESTING.md` for the
   exact known-good FQBN string.
2. **ESP32-C6 native-USB boards + `USB CDC On Boot: Disabled`** (the board
   profile's default) → `Serial` output routes to the physical UART0 pins,
   which aren't wired to anything on native-USB-only boards. Nothing shows
   in the Serial Monitor even though upload succeeds and the sketch runs
   fine. Fix: **Tools → USB CDC On Boot → Enabled**, then re-upload (compile-
   time setting, a reset alone won't fix it).
3. **Arduino IDE and `arduino-cli` share the same on-disk build cache**
   (`~/Library/Caches/arduino/sketches/<hash-of-sketch-path>/`). Running an
   `arduino-cli compile` syntax-check while the IDE has the same sketch open
   (its language-server rebuilds continuously in the background, not just on
   Verify) corrupts that shared cache — "cannot find X.cpp.o" linker errors.
   Fix is a clean cache wipe, but only safe with the IDE **fully quit**
   (Cmd+Q, not just "cancel the build") — otherwise the daemon/language-server
   recreate files mid-delete. **Rule going forward: don't run compile checks
   against a sketch the IDE currently has open.**
4. GEEK's LCD pin map (`LCD_SCK=1 LCD_DIN=2 LCD_DC=3 LCD_RST=4 LCD_CS=5
   LCD_BL=6`) is a best-guess from a community thread, not a Waveshare-
   published spec — now bench-confirmed correct for this unit, but flag it
   as unverified-until-tested if it's ever used on a different GEEK
   revision/batch (HARDWARE.md's touch-controller-address story is exactly
   this failure mode happening before).

## Open gaps (full detail + rationale in `docs/ozkey-09.md` §4)

1. **OPEN, needs a decision:** a proposal to route OZKEY-hotel through a
   bridge conflicts with ozkey-08 §0.0.1's explicit no-bridge-for-hotel
   rule (Thread SEDs can't mesh through concrete). Not resolved either way.
2. Bridge32 has no way to be told its personality (Matter-bridge vs
   MQTT-uplink) at provisioning time — not designed, not built.
3–4. QR trust anchor: designed for the lock (ozkey-08 §3) but deferred even
   there (`CONTRACT.md` "Deferred v2"); not designed at all for OZBRIDGE.
5. **Thread mesh RF range (bridge32 ↔ lock) is completely untested** — no
   frame transport exists yet to even exercise it. Same physics that got
   hotels excluded from bridging applies at house scale; no mitigation story.
6. MQTT uplink not built on bridge32 — Thread network forms, zero path to
   ozlockserv yet.
7. Tuya UART relay not built on threadcomm — no actual unlock/credential
   function once joined.
8. Thread-side frame transport (the payload riding on top of the joined
   mesh) — not built; this is what would let #5 actually be tested.
9. **OPEN:** LCD status on bridge32 — bench debug aid (GEEK happens to have
   a screen) or reconsidering headless-for-production? Production spec
   (§0.0.1) says headless (LED + BLE status chars only).

## Suggested sequencing (proposal, not a decision — ozkey-09 §6)

1. Resolve gap #1 (hotel-bridge) first — downstream choices depend on it.
2. Design bridge32 personality selection (#2) — likely a `mode` field on
   `bridge32`'s `provision` payload, same pattern as the lock's.
3. Decide gap #9 (LCD scope) — determines if display work is throwaway or
   a real feature.
4. **Before building MQTT uplink / Tuya relay (#6–8), bench-test #5 (RF
   range) at realistic residential distance.** Cheaper to find out now than
   after two more features are built on top of an unproven radio link.
5. QR trust-anchor wiring (#3–4) can follow — it's anti-MITM hardening, not
   a functional blocker.
