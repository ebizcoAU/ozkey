/*
 * doorlock19 — Waveshare ESP32-C6 Touch-LCD 1.9" (touch SKU 30941, CST8xx
 * @0x15, N8 8MB) — TWO physical units, both flashed identically as this
 * same file.
 *
 * 2026-08-07: refactored to a thin per-board file over
 * blelock/common/ozdoorlock_core.h, the shared crypto/BLE/bond/MCU-
 * forwarding/Wi-Fi/Thread/dispatch logic — previously a full byte-for-byte
 * fork of doorlock.ino (the 1.47" board's own file), now one shared
 * implementation both boards include. See ozdoorlock_core.h's own header
 * for exactly what a per-board file must define before including it.
 *
 * Board : Waveshare ESP32-C6 Touch-LCD 1.9"
 * WHAT'S DIFFERENT FROM doorlock.ino (the 1.47" board), all confined to
 * this file:
 *   - LCD pins + driver init: DC/CS/SCK/MOSI/MISO/RST/BL and the
 *     Arduino_ST7789 constructor args, from the authoritative Arduino_GFX
 *     WAVESHARE_ESP32_C6_LCD_1_9 device profile (confirmed against real
 *     hardware 2026-08-06, not guessed).
 *   - Backlight is ACTIVE-LOW on this board (LOW = on), opposite of the
 *     1.47" board.
 *   - gfx->setRotation(3), not (5).
 *   - Touch: CST8xx @ I2C 0x15 on SDA=18/SCL=8, NOT the 1.47" board's
 *     CST816 @0x63/GPIO20-21. No RST or INT pin at all — pure I2C polling
 *     (confirmed from Waveshare's own demo repo). KNOWN GAP: touch can no
 *     longer wake the device from light sleep (no HAS_TOUCH_INT here) —
 *     SRDY + the heartbeat timer are the only wake sources on this variant.
 *   - Touch coordinate transform: an affine fit (least-squares, from 11
 *     real labeled taps) — the two boards' touch panels don't share a
 *     coordinate convention.
 *   - Panel height is 170, not 172 (2px shorter).
 *   - SRDY_PIN 7->20, MRDY_PIN 8->21 — the OLD assignments collide
 *     directly with this board's hardwired LCD_CS(7)/TOUCH_SCL(8). 20 is
 *     confirmed free (Waveshare's own SD-card pin, unused); 21 is
 *     best-current-evidence free (mirrors the old board's retired
 *     TOUCH_INT), not schematic-confirmed — flag if wrong on real hardware.
 *   - A brief startup splash screen shows before the operational dashboard
 *     (operator request).
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Hardware pins — confirmed against real hardware 2026-08-06 (Arduino_GFX
// WAVESHARE_ESP32_C6_LCD_1_9 profile + Waveshare's own demo repo), NOT
// HARDWARE.md (that's the 1.47" board) ──────────────────────────────────────
#define LCD_DC   6
#define LCD_CS   7
#define LCD_SCK  5
#define LCD_DIN  4
#define LCD_MISO 19
#define LCD_RST  14
#define LCD_BL   15   // ACTIVE-LOW on this board
#define I2C_SDA  18
#define I2C_SCL  8
#define TOUCH_ADDR 0x15 // CST8xx — different sub-variant than the 1.47" board's CST816@0x63
// No TOUCH_RST/TOUCH_INT — this board's touch has neither pin wired in
// either of Waveshare's own reference implementations (pure I2C polling).

// Tuya MCU bus → LockSim Mode B (unchanged wire convention from doorlock.ino)
#define TUYA_TX_PIN 16  // -> CP2102 RXD
#define TUYA_RX_PIN 17  // <- CP2102 TXD

// §0.2 wake lines. REASSIGNED from doorlock.ino's 7/8 — those collide
// directly with this board's hardwired LCD_CS(7)/I2C_SCL(8). 20 is
// confirmed free (Waveshare's own SD-card CS pin, unused). 21 is
// best-current-evidence free (mirrors the old board's retired TOUCH_INT)
// but NOT schematic-confirmed — flag if wrong.
#define SRDY_PIN 20 // MCU → module: "module, wake" / held low = MCU awake
#define MRDY_PIN 21 // module → MCU: "MCU, wake" / held low = module awake

// Mechanical factory-reset button (hold 5s) — CONFIRMED on real hardware
// 2026-08-07: GPIO9 is correct. The initial "doesn't work" report was the
// wrong physical button — this board was upside down on the bench, so BOOT
// and RESET were swapped from what the operator expected; holding the
// actual BOOT button (not RESET) triggers the 5s countdown and factory
// reset correctly. Was previously an inherited toolchain default with no
// board-specific verification at all (esp32-hal.h's generic BOOT_PIN) —
// now a real, tested value, not a guess.
#define USER_BUTTON 9

// ── Palette — STANDARD RGB565, not BGR. This board's confirmed profile uses
// IPS=true, which is standard RGB — confirmed empirically (dlock19.ino's
// badge/highlight colors all read correctly with these direct values).
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xF800
#define C_BLUE  0x001F
#define C_GREEN 0x07E0
#define C_AMBER 0xFD20
#define C_GREY  0x8410
#define C_DIM   0x39E7

// ── Display geometry + rotation + backlight polarity ───────────────────────
#define PANEL_W 320
#define PANEL_H 170        // 170, not 172 — this panel is 2px shorter
#define LCD_ROTATION 3     // confirmed correct this session — 5, 7, 1 were each tried and wrong
#define LCD_BL_ON  LOW     // ACTIVE-LOW backlight — LOW = on
#define LCD_BL_OFF HIGH

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation at construction */, true /* IPS */,
    170 /* width */, 320 /* height */,
    35 /* col offset 1 */, 0 /* row offset 1 */, 35 /* col offset 2 */, 0 /* row offset 2 */);

// ── Firmware version ────────────────────────────────────────────────────────
// Own namespace (19-1.0 through 19-1.3) RETIRED 2026-08-07 — see 1.15 below.
// History while it lasted:
//   19-1.0 (2026-08-06): first port to this board. All crypto/BLE/bond/MCU
//           logic unchanged from doorlock-1.12 — only display/touch driver,
//           SRDY/MRDY pins, and backlight polarity changed.
//   19-1.1 (2026-08-07): refactored onto the shared ozdoorlock_core.h — no
//           functional change, pure restructuring (see file header).
//   19-1.2 (2026-08-07): drawn keypad (dlock19-style), hex-command readout,
//           USER_BUTTON made explicit — see doorlock.ino's own changelog for
//           the shared-logic details (identical change, both boards).
//   19-1.3 (2026-08-07): layout overlap fix (LOCKED badge/hex row/keypad
//           were painting over each other) + thin color-bar door-status
//           indicator — see doorlock.ino's own changelog / ozdoorlock_core.h.
//   1.15 (2026-08-07): UNIFIED VERSION SCHEME — this board now shares
//        doorlock.ino's FW_VERSION/FW_DISPLAY_VERSION string instead of its
//        own diverging counter, since both boards have shared byte-identical
//        core logic since the refactor and there's no real reason left to
//        version them separately (operator directive).
//   1.16 (2026-08-07): fixed a real bug shipped in 1.15 — drawHexReadout()
//        was still drawing at y=STATUS_H (same y as the color bar), so its
//        black background painted over the bar every redraw (operator-
//        caught: "big red line... overlay and disappear"; this is the black
//        screen the operator saw on this board). Also, after seeing 1.15 on
//        real hardware: status text merged back to one line, color bar
//        shrunk to 30px with the BLE countdown moved onto that row instead
//        of its own line, keypad reduced 4x3->4x2 (no real PIN backend,
//        taps only exercise touch zones), hex readout now 2 lines with the
//        freed space, filled blue keypad buttons + filled amber BLE-open
//        badge (operator: "employ blue amber etc...to improve your UI").
//   1.17 (2026-08-07): third layout pass after seeing 1.16 on real hardware
//        — status text + color bar + BLE countdown merged onto ONE row
//        (was two), bar shrunk to 60px with compact "BLE Ns" text so it
//        still fits; hex readout is now one bigger line (textSize 2, was
//        two lines) trading max visible bytes for readability. Also fixed
//        the 3s periodic refresh causing visible flicker — drawStatusLine()
//        split out of drawOperational() so the periodic tick repaints only
//        that one row directly, never a full fillScreen(). See doorlock.ino's
//        own changelog for the exact operator quotes.
//   1.18 (2026-08-07): fourth pass on the hex readout after real-hardware
//        feedback — textSize back to 1 (size 2 didn't fit horizontally,
//        "1 step smaller"), and the line now leads with the lock's own name
//        (first 6 chars of cfgName, falling back to deviceId, same
//        convention as drawJoining()) before the hex bytes. See doorlock.ino's
//        own changelog for the exact operator quotes.
//   1.19 (2026-08-07): lock name on the hex line widened 6->10 chars, and
//        C_GREY swapped for C_WHITE (operator: "as I cant hardly see it" —
//        grey was too low-contrast on this panel).
//   1.20 (2026-08-07): final layout pass — line 1 now carries everything
//        (version/transport/role-or-IP/lock name/BLE countdown far right);
//        the LOCKED/UNLOCKED color bar moved off line 1 entirely, now a
//        40px bar on line 2 next to the hex readout. See doorlock.ino's own
//        changelog for the exact operator spec.
//   1.21 (2026-08-07): BLE-open countdown gets a filled amber badge + white
//        text again (operator: "BLE 20s in amber background white text"),
//        reapplied after the status line was rebuilt.
//   1.22 (2026-08-08): ozkey-13 F1 — shared-core `ozControlTry()` factored
//        into open/verify + dispatch halves so MQTT can reuse it (F2, not
//        yet wired). See doorlock.ino's own changelog for the detail. No UI
//        change, no behavior change on BLE.
//   1.23 (2026-08-08): ozkey-13 F2-F5 — MQTT `envelope_hex` sealed-command
//        path wired in, DP 21-24 credential frames join DP 1 on the sealed
//        dispatch (role-gated to bond #0), legacy `payload_hex` kept for
//        rollout. See doorlock.ino's own changelog for the full detail. No
//        UI change. NOT YET HARDWARE-VERIFIED.
//   1.24 (2026-08-08): ozkey-13 §8 F7 — Thread UDP relay receive half gets
//        the same `envelope_hex` branch, same F1 core, no live challenge.
//        Closes the gap where sealed grants could never reach a bridged
//        (Thread) lock — which is every bench lock. See doorlock.ino's own
//        changelog for the full detail. No UI change. NOT YET HARDWARE-
//        VERIFIED.
// 1.25   ozkey-17 F8 + U0 — see doorlock.ino's changelog for the full entry.
//        F8: sealed-envelope plaintext is OZKIE semantic JSON; the lock builds
//        the Tuya frame locally at the MCU boundary. U0: per-bond outbound
//        counter for lock->app sealing, in OZ_BOND_REC's spare bytes.
// 🔴 KEEP IN SYNC WITH doorlock/doorlock.ino — THIS FILE IS THE ONE THAT SHIPS
//    ON THE BENCH BOARD.
//
// `make flash BOARD=19` builds THIS sketch (Makefile: SKETCH_19 := doorlock19).
// BOARD=147 builds doorlock/doorlock.ino. The 1.15 note above says this board
// "shares doorlock.ino's FW_VERSION" — it does NOT. Each sketch #defines its
// own, and they have drifted before.
//
// 2026-08-10: bumped 1.30 -> 1.32 only after the operator saw V1.30 on the
// panel of a board that had just been flashed with 1.32 code. The functional
// changes were never missing — R1/R2/R4 (unicast uplink), R5 (roster_epoch)
// and the MCU link dot all live in common/ozdoorlock_core.h, which both
// sketches include — but the ON-SCREEN VERSION LIED, which is worse than a
// missing feature: it makes the panel useless for telling what is running.
//
// This is the SECOND time this exact bug has shipped (see doorlock.ino:161,
// "FW_DISPLAY_VERSION was stuck at V1.21 through the 1.22/1.23 bumps").
// Editing one file's version string and flashing the other is the failure
// mode. Change both, every time.
#define FW_VERSION "doorlock-1.41"
#define FW_DISPLAY_VERSION "V1.41" // shown on-screen: "OZLOCK V1.36 THREAD/WIFI"

// Brief startup splash before the operational dashboard (operator request) —
// shown once at boot, before WiFi/Thread/BLE bring-up starts, so there's
// something on screen immediately rather than a blank/black panel while the
// rest of setup() runs.
void drawSplash() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, C_AMBER);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(60, 60);
  gfx->print("OZLOCK");
  gfx->setTextColor(C_AMBER);
  gfx->setTextSize(1);
  gfx->setCursor(90, 100);
  gfx->print(FW_VERSION);
  gfx->setCursor(40, 120);
  gfx->print("Waveshare ESP32-C6 1.9\"");
  delay(1200);
}

// No RST/INT pin on this board — pure I2C polling. Init sequence matches
// Waveshare's own reference driver exactly (write 0x00 to reg 0x00 a few
// times until ACKed, "switch to normal mode").
void touchInit() {
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(20);
  bool ok = false;
  for (int i = 0; i < 10; i++) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() == 0) { ok = true; break; }
    delay(100);
  }
  Serial.printf("[TOUCH] init addr=0x%02X SDA=%d SCL=%d -> %s\n",
                TOUCH_ADDR, I2C_SDA, I2C_SCL, ok ? "ACK ok" : "NO ACK — touch dead");
}

// Affine fit (least-squares, from 11 real labeled taps) — this board's touch
// panel does NOT share the 1.47" board's simple linear transform (different
// digitizer/LCD alignment). Coefficients validated in dlock19.ino.
void mapTouchRaw(int rawX, int rawY, int &x, int &y) {
  float sx = 0.00684f * rawX - 1.00976f * rawY + 312.580f;
  float sy = 1.04315f * rawX - 0.06231f * rawY + 5.590f;
  x = (int)sx;
  y = (int)sy;
  if (x < 0) x = 0; if (x > PANEL_W - 1) x = PANEL_W - 1;
  if (y < 0) y = 0; if (y > PANEL_H - 1) y = PANEL_H - 1;
}

#include "../common/ozdoorlock_core.h"
