/*
 * dlch584m_19 — CH584M DOORLOCK SIMULATOR on the Waveshare ESP32-C6
 * Touch-LCD 1.9" (touch SKU 30941, CST8xx @0x15, N8 8MB) — SAME physical
 * board as doorlock19.ino, different firmware ROLE.
 *
 * Per XFtposDecisions-138 (PM directive, 2026-08-29, P0): the production
 * lock MCU is moving from ESP32-C6+Tuya-DL-MCU to WCH's CH584M, a
 * BLE-native, all-in-one chip with no external DL-MCU (native TouchKey,
 * NFC/RFID, motor driver, LED driver, BLE — see
 * docs/DoorLockHW/DL_CH584M_PINOUT.md). Real CH584M samples are not in hand
 * yet (DL manufacturers won't share hardware mapping before samples ship),
 * so this board stands in for CH584M until then. Design/emulation-scope
 * proposal: docs/ozkey-45.md — read that before changing this file's scope,
 * do not silently re-derive it.
 *
 * 🟢 TUYA/MCU-FORWARDING REMOVED (2026-08-29) — this board no longer speaks
 * Tuya DP frames to anything. `#define OZ_NO_MCU` below tells the shared
 * core "this board IS its own MCU" (ozkey-45): Serial1/SRDY/MRDY are never
 * brought up, `tuyaWirePump()`/`ozAwaitMcuAck()` are no-ops/instant-true,
 * and `tuyaWireSend()` — the ONE chokepoint every dispatch path already
 * funneled through — applies a DOWN frame's effect LOCALLY instead of
 * transmitting it (unlock resolves straight to `markDoorUnlocked()`, the
 * CONFIRMED/authoritative path, not the amber ISSUED one, because there is
 * no separate MCU whose word could outrank ours). This was done as a
 * guarded addition (`#ifdef OZ_NO_MCU` at each site) inside
 * `ozdoorlock_core.h` itself, NOT a fork — `doorlock.ino`/`doorlock19.ino`
 * do not define the flag and are byte-identical in behavior to before.
 * 🔴 CRYPTO/BOND/ENVELOPE LOGIC IS UNTOUCHED — this change only affects
 * "what happens once a verb is authorized and would have gone out over
 * UART," never the authorization path itself (operator's explicit
 * instruction, 2026-08-29).
 *
 * STILL NOT DONE — the new XF-138 GATT service (Command/Status/OTA/
 * Handshake UUIDs) is not yet added; this board still only serves the
 * existing OZKIE `4f5a4b31-...` service. Two PM decisions are still open
 * (ozkey-45.md §1: dual GATT service or bridge-only; this board's
 * touch-wake limitation is a simulator gap, not a CH584M one) —
 * deliberately not guessed at here.
 *
 * WHAT'S THE SAME AS doorlock19.ino, AND WHY: identical physical board
 * (Waveshare ESP32-C6 Touch-LCD 1.9"), so LCD pins/driver, touch IC/pins/
 * driver, palette, panel geometry, and splash screen are copied verbatim,
 * not re-derived. See doorlock19.ino's own header for the hardware
 * provenance (Arduino_GFX WAVESHARE_ESP32_C6_LCD_1_9 profile, Waveshare's
 * own demo repo for touch pins) if any of these ever need re-confirming.
 *
 * WHAT'S DIFFERENT FROM doorlock19.ino:
 *   - Own firmware version namespace (`dlch584m-0.x`) — this is a distinct
 *     firmware ROLE (a simulator), not the same logical firmware as
 *     doorlock.ino/doorlock19.ino, so it does not share their version
 *     counter (same reasoning doorlock19.ino's own 1.15 note gives for why
 *     THOSE two boards DO share one — this one is a genuinely different
 *     thing, not a third physical unit of the same thing).
 *   - `TUYA_TX_PIN`/`TUYA_RX_PIN`/`SRDY_PIN`/`MRDY_PIN` are defined only
 *     because `ozdoorlock_core.h`'s per-board contract requires them —
 *     they are VESTIGIAL on this board. CH584M has no external DL-MCU to
 *     wake/talk to over UART; these pins are wired to nothing. Left as
 *     dead defines rather than modifying the shared core's contract in
 *     this pass — revisit when the local-dispatch surgery lands.
 *
 * ── ozkey-45.md §3 emulation scope (embedded here so the plan travels with
 *    the code, not just the doc) ──────────────────────────────────────────
 *   Tier 1, faithful — build for real:
 *     keypad (already have it, this board), BLE GATT (already have the
 *     OZKIE service; XF-138's new Command/Status/OTA/Handshake UUIDs are
 *     NOT YET ADDED), bolt/motor state (simulate on-screen + serial log,
 *     same pattern as DP 47 in LockSim), battery ADC (simulate via a UI/
 *     serial control, same pattern as locksim's DP 45 battery_percentage
 *     feature).
 *   Tier 2, logical/UI stand-in only — dev kit has no real hardware for
 *     these: RFID/NFC (tap-card gesture), LED status (color blocks on the
 *     TFT), tamper (long-press or a second GPIO button), speaker
 *     (log-line/screen-flash).
 *   Tier 3, worth real hardware instead of simulating: fingerprint — it's
 *     an external UART module on CH584M's own reference design too, so
 *     wiring the same module to this board's spare UART gets real sensor
 *     behavior for a small hardware buy, if enrollment/matching matters
 *     for this milestone.
 *   Explicitly NOT emulated: the real board's "optional LCD" is a tiny
 *     4-segment/2-common display, not graphical — this dev kit's color
 *     touchscreen is a development-visibility superset, not a stand-in for
 *     it. Also skipped: SWD/crystal/VBAT-divider (CH584M-silicon-specific,
 *     irrelevant at the OZKIE/BLE protocol layer this simulator tests) and
 *     the reset button (already covered by the shared core's `USER_BUTTON`
 *     contract, used unchanged below).
 */

// This board IS its own MCU — no external DL-MCU exists to forward Tuya DP
// frames to (ozkey-45). Must be defined BEFORE including ozdoorlock_core.h;
// see that file's own `#ifdef OZ_NO_MCU` sites for exactly what changes.
#define OZ_NO_MCU 1

// 2026-09-01, bench PIN-entry testing (operator's ask) — SEPARATE from
// OZ_NO_MCU on purpose. dlch584m_147 shares OZ_NO_MCU but has a different
// touch controller entirely (CST816@0x63/GPIO20-21 vs this board's
// CST8xx@0x15, no RST/INT) and no `touchRead()` of its own — gating the new
// keypad-polling code on OZ_NO_MCU alone would either fail to link on that
// board or, worse, compile against the wrong chip's assumptions. This board
// only.
#define OZ_KEYPAD_LIVE 1

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Hardware pins — SAME physical board as doorlock19.ino, copied verbatim.
// See that file's header for provenance (Arduino_GFX WAVESHARE_ESP32_C6_LCD_1_9
// profile + Waveshare's own demo repo), not re-derived here. ────────────────
#define LCD_DC   6
#define LCD_CS   7
#define LCD_SCK  5
#define LCD_DIN  4
#define LCD_MISO 19
#define LCD_RST  14
#define LCD_BL   15   // ACTIVE-LOW on this board
#define I2C_SDA  18
#define I2C_SCL  8
#define TOUCH_ADDR 0x15 // CST8xx
// No TOUCH_RST/TOUCH_INT on this board — pure I2C polling (see doorlock19.ino).

// VESTIGIAL on this board — CH584M has no external DL-MCU, nothing is wired
// to these pins. Defined only because ozdoorlock_core.h's per-board contract
// requires them; `mcu=DOWN` on this board is expected, not a fault. Same
// numbers as doorlock19.ino purely so they don't collide with any real pin
// this board actually uses.
#define TUYA_TX_PIN 16
#define TUYA_RX_PIN 17
#define SRDY_PIN 20
#define MRDY_PIN 21

// Mechanical factory-reset button (hold 5s) — same physical board, same
// confirmed GPIO as doorlock19.ino.
#define USER_BUTTON 9

// ── Palette — same confirmed profile as doorlock19.ino (standard RGB565) ──
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xF800
#define C_BLUE  0x001F
#define C_GREEN 0x07E0
#define C_AMBER 0xFD20
#define C_GREY  0x8410
#define C_DIM   0x39E7

// ── Display geometry + rotation + backlight polarity — same board ─────────
#define PANEL_W 320
#define PANEL_H 170
#define LCD_ROTATION 3
#define LCD_BL_ON  LOW
#define LCD_BL_OFF HIGH

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation at construction */, true /* IPS */,
    170 /* width */, 320 /* height */,
    35 /* col offset 1 */, 0 /* row offset 1 */, 35 /* col offset 2 */, 0 /* row offset 2 */);

// ── Firmware version — own namespace, see file header for why ─────────────
#define FW_VERSION "dlch584m-0.28"
// XF-124 F1 — device kind for Nexus's registry. Nexus's N1-N5 (XFtposDecisions-138)
// already registered device_type="doorlock", firmware_id="fw-doorlock-ch584m-r1"
// for the real CH584M model; this simulator identifies as the same device_type
// so App/server-side matching logic (ozkey-45.md's open question re: identity
// fields) has something real to test against once it's built.
#define OZ_DEVICE_TYPE "doorlock"

static const char *fwDisplayVersion() {
  static char buf[16];
  const char *dash = strchr(FW_VERSION, '-');
  snprintf(buf, sizeof(buf), "V%s", dash ? dash + 1 : FW_VERSION);
  return buf;
}
#define FW_DISPLAY_VERSION fwDisplayVersion()

// Splash — identical to doorlock19.ino's, relabeled for this role. See that
// file's 1.71 changelog entry for why there is deliberately no delay() here.
void drawSplash() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, C_AMBER);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(30, 60);
  gfx->print("CH584M SIM");
  gfx->setTextColor(C_AMBER);
  gfx->setTextSize(1);
  gfx->setCursor(90, 100);
  gfx->print(FW_VERSION);
  gfx->setCursor(40, 120);
  gfx->print("Waveshare ESP32-C6 1.9\"");
}

// Touch init — same board, same sequence as doorlock19.ino.
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

// Same affine fit as doorlock19.ino — same physical touch panel.
void mapTouchRaw(int rawX, int rawY, int &x, int &y) {
  float sx = 0.00684f * rawX - 1.00976f * rawY + 312.580f;
  float sy = 1.04315f * rawX - 0.06231f * rawY + 5.590f;
  x = (int)sx;
  y = (int)sy;
  if (x < 0) x = 0; if (x > PANEL_W - 1) x = PANEL_W - 1;
  if (y < 0) y = 0; if (y > PANEL_H - 1) y = PANEL_H - 1;
}

// ── touchRead() — 2026-09-01, bench-only PIN-entry work (operator's ask) ────
//
// touchInit()/mapTouchRaw() above have existed since before touch input was
// globally removed at firmware 2.19 (see ozdoorlock_core.h's own header on
// why: the gesture that removal killed had been wired to a panel PRODUCTION
// DOES NOT HAVE, the same class of mistake XF-58's presence gate made once
// already). Neither of them ever actually READ a touch point — init only
// pings the controller into "normal mode", and the coordinate transform has
// nothing to transform without a raw reading feeding it. This function is
// the piece that was never there: the actual I2C register read.
//
// UPDATE 2026-09-03 (XFtposDecisions-153): this register layout is stronger
// than "modeled on a sibling chip" — checked git history, and this exact
// byte layout (byte[2] = touch count, X packed 4-bit-high/8-bit-low across
// byte[3:4], Y the same across byte[5:6]) is the SAME shared touchReadRegs()/
// touchRead() that lived in ozdoorlock_core.h and ran on THIS EXACT BOARD
// (CST8xx@0x15, pure I2C polling, this unit's own doorlock19.ino) for months
// before being removed at doorlock-2.19 (2026-08-23) — not a guess ported
// from the 1.47" board's chip, a driver this physical unit already ran.
// `git show de99d97^:blelock/common/ozdoorlock_core.h` has the original,
// including edge-triggering and a "silent failure" heartbeat this simpler
// bench version leaves out — level-triggered is enough for ozKeypadPoll()'s
// own edge-detection one layer up. Still worth a real-finger bench pass to
// confirm nothing about this board's specific wiring drifted since 2.19,
// but this is no longer an unverified guess.
//
// Returns true with rawX/rawY populated while a finger is down; false
// otherwise (no touch, or the controller didn't ACK — same "touch dead"
// condition touchInit() already detects at boot).
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
