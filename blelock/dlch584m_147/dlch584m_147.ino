/*
 * dlch584m_147 — CH584M DOORLOCK SIMULATOR on the 1.47" board (LockB /
 * DoorB, CST816 @0x63, N8 8MB). Sibling of dlch584m_19; SAME firmware role,
 * DIFFERENT physical hardware.
 *
 * Per XFtposDecisions-138 / docs/ozkey-45.md. Read ozkey-45 before changing
 * this file's scope — the emulation tiers are recorded there and in
 * dlch584m_19.ino's header, and are deliberately not duplicated here.
 *
 * ── WHY A SEPARATE SKETCH AND NOT A `BOARD=` VARIANT OF dlch584m_19 ────────
 * The two dev kits share nothing at the pin level. This board has: a
 * different LCD pin map and ST7789 constructor, a different touch IC at a
 * different address (CST816 @0x63 vs CST8xx @0x15), a REAL TOUCH_RST/
 * TOUCH_INT pair (so `HAS_TOUCH_INT` — this is the only board that can wake
 * from light sleep on touch), ACTIVE-HIGH backlight (opposite of the 1.9"),
 * `LCD_ROTATION 5`, a 172 px panel (not 170), a BGR palette where RED and
 * BLUE are swapped, and its own touch coordinate transform. Every one of
 * those is copied VERBATIM from doorlock/doorlock.ino — the file that has
 * been running on this exact unit — and none of it is re-derived here.
 *
 * ── WHAT MAKES THIS THE CH584M SIMULATOR ──────────────────────────────────
 * `#define OZ_NO_MCU` below, exactly as in dlch584m_19. In the shared core
 * that one flag means: no Tuya UART (Serial1/SRDY/MRDY never brought up,
 * `mcu=DOWN` is EXPECTED here, not a fault), DOWN verbs applied locally
 * instead of transmitted, no Thread and no Wi-Fi brought up at all (CH584M
 * has neither radio), continuous BLE advertising from boot so the bridge can
 * reach it at any time, and a panel that shows `BLE: CONNECTED/NOT CONNECTED`
 * instead of a mesh role. Crypto, bonds and the sealed-envelope path are
 * UNTOUCHED.
 *
 * 🔴 LockB's NVS still holds its production provisioning (`xport=wifi`, its
 * bond, its broker creds). Nothing here rewrites it — `make flash BOARD=147`
 * restores LockB exactly as it was. That is deliberate: this board is
 * borrowed, not repurposed.
 *
 * 🔴 KEEP FW_VERSION IN SYNC WITH dlch584m_19.ino. Same logical firmware on
 * two boards, so it carries the same version string — the same convention
 * doorlock.ino/doorlock19.ino follow, and the same trap: each sketch #defines
 * its own copy, and those two DRIFTED twice before anyone noticed the panel
 * was reporting a build that was not running. Change both, every time.
 */

// This board IS its own MCU — no external DL-MCU to forward Tuya DP frames to
// (ozkey-45). Must be defined BEFORE including ozdoorlock_core.h.
#define OZ_NO_MCU 1

// 2026-09-03 — ported from dlch584m_19.ino, same reasoning (XFtposDecisions-153):
// this board's own touch driver (touchInit()/mapTouchRaw() above) predates the
// 2.19 removal and was never missing — only the actual register read was. See
// touchRead() below, right before the shared-core include, for why it's now
// filled in with real confidence rather than a guess.
#define OZ_KEYPAD_LIVE 1

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Hardware pins — VERBATIM from doorlock/doorlock.ino (this unit's own
// proven file). See HARDWARE.md for provenance. ────────────────────────────
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

// VESTIGIAL on this build — CH584M has no external DL-MCU and nothing is
// wired to these. Defined only because ozdoorlock_core.h's per-board contract
// requires them; kept at doorlock.ino's numbers so they cannot collide with a
// pin this board actually uses.
#define TUYA_TX_PIN 16
#define TUYA_RX_PIN 17
#define SRDY_PIN 7
#define MRDY_PIN 8

#define USER_BUTTON 9

// ── Palette — BGR on this panel, so RED/BLUE are SWAPPED vs the 1.9" board.
// Not a typo: doorlock.ino has carried these exact values since the panel was
// characterised, and the ST7789 below is constructed with `false /*BGR*/`.
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0x001F
#define C_BLUE 0xF800
#define C_GREEN 0x07E0
#define C_AMBER 0x051F
#define C_GREY 0x8410
#define C_DIM 0x39E7

// ── Display geometry + rotation + backlight polarity ──────────────────────
#define PANEL_W 320
#define PANEL_H 172       // 172 here; the 1.9" panel is 170
#define LCD_ROTATION 5    // 5 here; the 1.9" board uses 3
#define LCD_BL_ON  HIGH   // ACTIVE-HIGH; the 1.9" board is active-LOW
#define LCD_BL_OFF LOW

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false /*BGR*/, 172, 320, 34, 0, 34, 0);

// ── Firmware version — SHARED with dlch584m_19.ino, see the header ────────
#define FW_VERSION "dlch584m-0.28"
#define OZ_DEVICE_TYPE "doorlock"

static const char *fwDisplayVersion() {
  static char buf[16];
  const char *dash = strchr(FW_VERSION, '-');
  snprintf(buf, sizeof(buf), "V%s", dash ? dash + 1 : FW_VERSION);
  return buf;
}
#define FW_DISPLAY_VERSION fwDisplayVersion()

// No splash on this board — doorlock.ino never had one (the 1.9" board's was
// an operator request specific to that panel). Kept empty rather than adding
// one, so this stays a faithful copy of what runs on this unit.
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

// Simple linear transform — this panel's own, NOT the 1.9" board's affine
// fit. NOTE: x is deliberately not clamped (it never was in doorlock.ino);
// preserved exactly rather than "fixed", to keep this a faithful copy.
void mapTouchRaw(int rawX, int rawY, int &x, int &y) {
  x = PANEL_W - rawY;
  y = 180 - (rawX * 6) / 5;
  if (y < 0) y = 0;
  if (y > PANEL_H - 1) y = PANEL_H - 1;
}

// ── touchRead() — 2026-09-03, ported (XFtposDecisions-153), not written fresh.
//
// This exact register layout (buf[2]=touch count, X packed 4-bit-high/8-bit-low
// across buf[3:4], Y the same across buf[5:6]) is not a guess or a port from a
// sibling chip — it is the SAME shared `touchReadRegs()`/`touchRead()` that
// lived in this file's own `ozdoorlock_core.h` and ran on THIS EXACT BOARD
// (CST816@0x63, real TOUCH_RST/TOUCH_INT) for months before being removed at
// doorlock-2.19 (2026-08-23, operator directive — the removal was about the
// gesture's WIRING to a since-replaced presence gate, not about the driver
// being wrong). `git show de99d97^:blelock/common/ozdoorlock_core.h` has the
// original, including edge-triggering and a "silent failure" heartbeat this
// simpler bench version deliberately leaves out for now — level-triggered is
// enough for ozKeypadPoll()'s own edge-detection (g_kpWasDown) one layer up.
//
// Returns true with rawX/rawY populated while a finger is down; false
// otherwise (no touch, or the controller didn't ACK).
bool touchRead(int &rawX, int &rawY) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false; // no ACK — same as touchInit()'s failure case
  Wire.requestFrom(TOUCH_ADDR, 7);
  if (Wire.available() < 7) return false;
  uint8_t buf[7];
  for (int i = 0; i < 7; i++) buf[i] = Wire.read();
  const uint8_t touchCount = buf[2];
  if (touchCount == 0 || touchCount > 5) return false; // 0 = no finger; >5 = garbage frame
  rawX = ((buf[3] & 0x0F) << 8) | buf[4];
  rawY = ((buf[5] & 0x0F) << 8) | buf[6];
  return true;
}

#include "../common/ozdoorlock_core.h"
