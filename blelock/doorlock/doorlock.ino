/*
 * doorlock — Waveshare ESP32-C6 Touch-LCD 1.47" (pin map: blelock/HARDWARE.md)
 *
 * 2026-08-07: refactored to a thin per-board file over
 * blelock/common/ozdoorlock_core.h, the shared crypto/BLE/bond/MCU-
 * forwarding/Wi-Fi/Thread/dispatch logic — previously this file WAS that
 * logic (doorlock19.ino was forked from it 2026-08-06); now both boards
 * include one shared implementation. See ozdoorlock_core.h's own header
 * for exactly what a per-board file must define before including it.
 *
 * This board keeps its original touch driver (real TOUCH_RST/TOUCH_INT
 * pins, CST816 @0x63) and its original linear coordinate transform —
 * unchanged from before the refactor. No behavior change intended; the
 * historical per-firmware-version changelog now lives in
 * ozdoorlock_core.h since it documents the shared logic's history.
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Hardware pins (HARDWARE.md, operator-verified) ──────────────────────────
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_RST 22
#define LCD_BL 23
#define I2C_SDA 18
#define I2C_SCL 19
#define TOUCH_RST 20
#define TOUCH_INT 21
#define TOUCH_ADDR 0x63
#define HAS_TOUCH_INT // real INT/RST pair on this board — usable as a light-sleep wake source

// Tuya MCU bus → LockSim Mode B (wire-verified 2026-07-19)
#define TUYA_TX_PIN 16  // -> CP2102 RXD
#define TUYA_RX_PIN 17  // <- CP2102 TXD

// §0.2 wake lines (Tuya keep-alive contract): active low, answer-before-
// transmit, 10 s serial-idle release. GPIO1-4 reserved (SPI/SD). SRDY on an
// LP pin (deep-sleep-wake capable). GPIO8 is a C6 strapping pin — MRDY
// idles HIGH so boot is unaffected; remap on real lock hw if its MCU pulls
// this line low at reset.
#define SRDY_PIN 7  // MCU → module: "module, wake" / held low = MCU awake
#define MRDY_PIN 8  // module → MCU: "MCU, wake" / held low = module awake

// Mechanical factory-reset button (hold 5s). 2026-08-07: made explicit here —
// this used to silently inherit the ESP32-C6 toolchain's generic BOOT_PIN
// (GPIO9, esp32-hal.h) with no board-specific verification at all, same gap
// as the 1.9" board. Kept at 9 (unchanged behavior) but NOT independently
// confirmed against this board's actual schematic/HARDWARE.md — flag if this
// turns out to be wrong on real hardware, same as SRDY/MRDY's own caveat.
#define USER_BUTTON 9

// ── BGR-corrected palette (panel is BGR) ────────────────────────────────────
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0x001F
#define C_BLUE 0xF800
#define C_GREEN 0x07E0
#define C_AMBER 0x051F
#define C_GREY 0x8410
#define C_DIM 0x39E7

// ── Display geometry + rotation + backlight polarity ───────────────────────
#define PANEL_W 320
#define PANEL_H 172
#define LCD_ROTATION 5
#define LCD_BL_ON  HIGH
#define LCD_BL_OFF LOW

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false /*BGR*/, 172, 320, 34, 0, 34, 0);

// ── Firmware version ────────────────────────────────────────────────────────
// BUMP THE MINOR ON EVERY FLASHED CHANGE (operator directive 2026-08-02).
// Full changelog 1.0-1.12 now lives in ozdoorlock_core.h (shared history).
//   1.13 (2026-08-07): refactored onto the shared ozdoorlock_core.h — no
//        functional change, pure restructuring (see file header).
//   1.14 (2026-08-07): drawn keypad (dlock19-style) replaces the invisible
//        hit-test grid; hex-command readout below the LOCKED/UNLOCKED badge
//        (Serial1/LockSim wire write stays real, hex is additive); Net/IP/
//        Owner text dropped from the LCD to fit the keypad (still in serial
//        + app); USER_BUTTON made an explicit, board-owned #define (was
//        silently inheriting the toolchain's generic BOOT_PIN).
//   1.15 (2026-08-07): UNIFIED VERSION SCHEME — doorlock.ino and
//        doorlock19.ino now share one FW_VERSION/FW_DISPLAY_VERSION string
//        instead of two diverging counters ("doorlock-1.x" vs
//        "doorlock19-1.x"), since they've shared byte-identical core logic
//        since the 2026-08-07 refactor and there's no real reason left to
//        version them separately (operator directive). Also the layout
//        overlap fix and thin color-bar door-status indicator — see
//        ozdoorlock_core.h's own changelog for the shared-logic details.
//   1.16 (2026-08-07): fixed a real bug shipped in 1.15 — drawHexReadout()
//        was still drawing at y=STATUS_H (same y as the color bar), so its
//        black background painted over the bar every redraw (operator-
//        caught: "big red line... overlay and disappear"). Also, after
//        seeing 1.15 on real hardware: status text merged back to one line,
//        color bar shrunk to 30px with the BLE countdown moved onto that
//        row instead of its own line, keypad reduced 4x3->4x2 (no real PIN
//        backend, taps only exercise touch zones), hex readout now 2 lines
//        with the freed space, filled blue keypad buttons + filled amber
//        BLE-open badge (operator: "employ blue amber etc...to improve
//        your UI").
//   1.17 (2026-08-07): third layout pass after seeing 1.16 on real hardware
//        — status text + color bar + BLE countdown merged onto ONE row
//        (was two: text alone, then bar+BLE), bar shrunk 30px->60px per
//        operator spec but now paired with compact "BLE Ns" text (not "BLE
//        open Ns", which didn't fit this row's trailing space) so it still
//        fits; hex readout is now one bigger line (textSize 2, was two
//        lines of smaller text) trading max visible bytes for readability.
//        Also fixed the 3s periodic refresh causing visible flicker
//        (operator: "REMOVE THE 3S screen update..it causes bad
//        flicker..can u update a word rather than a whole screen") — split
//        drawStatusLine() out of drawOperational() so the periodic tick
//        repaints only that one row directly, never a full fillScreen().
//   1.18 (2026-08-07): fourth pass on the hex readout after real-hardware
//        feedback — textSize back to 1 (size 2 didn't fit horizontally,
//        "1 step smaller"), and the line now leads with the lock's own name
//        (first 6 chars of cfgName, falling back to deviceId, same
//        convention as drawJoining()) before the hex bytes, using the ~100px
//        of spare width the smaller text left on that row.
//   1.19 (2026-08-07): lock name on the hex line widened 6->10 chars, and
//        C_GREY swapped for C_WHITE (operator: "as I cant hardly see it" —
//        grey was too low-contrast on this panel).
//   1.20 (2026-08-07): final layout pass — line 1 now carries everything
//        (version/transport/role-or-IP/lock name/BLE countdown far right);
//        the LOCKED/UNLOCKED color bar moved off line 1 entirely, now a
//        40px bar on line 2 next to the hex readout (name removed from
//        line 2 to avoid showing it twice).
//   1.21 (2026-08-07): BLE-open countdown gets a filled amber badge + white
//        text again (operator: "BLE 20s in amber background white text"),
//        reapplied after the status line was rebuilt.
#define FW_VERSION "doorlock-1.21"
#define FW_DISPLAY_VERSION "V1.21" // shown on-screen: "OZLOCK V1.21 THREAD/WIFI"

// This board shows no startup splash (never did, pre-refactor) — no-op so
// the shared core's unconditional drawSplash() call is a well-defined hook.
void drawSplash() {}

void touchInit() {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(100);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  Wire.beginTransmission(TOUCH_ADDR);
  int err = Wire.endTransmission();
  Serial.printf("[TOUCH] probe 0x%02X err=%d %s\n", TOUCH_ADDR, err,
                err == 0 ? "(ACK ok)" : "(NO ACK — touch dead)");
}

// Original simple linear transform — unchanged from before the refactor.
// NOTE: x is not clamped here (never was); only y is. Preserved exactly,
// not "fixed", to keep this refactor behavior-neutral.
void mapTouchRaw(int rawX, int rawY, int &x, int &y) {
  x = PANEL_W - rawY;
  y = 180 - (rawX * 6) / 5;
  if (y < 0) y = 0;
  if (y > PANEL_H - 1) y = PANEL_H - 1;
}

#include "../common/ozdoorlock_core.h"
