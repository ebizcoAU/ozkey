/*
 * GeekDisplayTest — ESP32-C6-GEEK display + QR bring-up (2026-07-24)
 * Board: Waveshare ESP32-C6-GEEK (1.14" ST7789 LCD, 240x135, N16 flash)
 *
 * ⚠ PIN MAP IS UNVERIFIED. Waveshare has not published an official GEEK
 * pinout (docs.waveshare.com/ESP32-C6-GEEK omits GPIO numbers entirely), and
 * a Home Assistant community thread reports MULTIPLE hardware revisions
 * with DIFFERENT wiring for the same product name. The pins below are one
 * real GEEK owner's "IDF Demo variant" numbers — the best evidence found,
 * but not guaranteed to match this specific board. Same situation blelock's
 * own HARDWARE.md warns about (the touch controller address was wrong for
 * one batch) — this sketch exists to bench-confirm before anything is
 * written to HARDWARE.md or bridge32.ino.
 *
 * WHAT TO REPORT BACK after flashing:
 *   - Screen blank / backlight not even on?        -> wrong BL or no power
 *   - Backlight on but no picture (blank/garbage)?  -> wrong DC/CS/SCK/MOSI
 *   - Picture appears but colors look swapped
 *     (e.g. red shows as blue)?                     -> BGR vs RGB (easy fix)
 *   - Picture appears cropped/shifted/mirrored?      -> offset or rotation
 *   - Everything looks right?                        -> tell me, and try
 *     scanning the QR screen with a phone camera from a few distances.
 *
 * If this candidate pinout is wrong, report exactly what you see (or
 * "nothing") and I'll try the alternate set commented below.
 */

#include <Arduino_GFX_Library.h>
#include "qrcode.h"

// ── Candidate pins (source: HA community thread, "IDF Demo variant") ───────
#define LCD_SCK 1
#define LCD_DIN 2  // MOSI
#define LCD_DC  3
#define LCD_RST 4
#define LCD_CS  5
#define LCD_BL  6

// ── If the above shows nothing, try this alternate set instead (comment
//    the block above, uncomment this one, reflash) — a second revision
//    reported in the same thread, exact numbers not confirmed, placeholder
//    shape only:
// #define LCD_SCK 7
// #define LCD_DIN 6
// #define LCD_DC  15
// #define LCD_CS  14
// #define LCD_RST 21
// #define LCD_BL  22

// Panel: 1.14" ST7789, native 135x240. Offset values (40/53) are typical for
// this exact panel SKU (seen on the well-known Pico-LCD-1.14 family) — not
// GEEK-specific confirmation, just the standard starting point for this
// controller+glass combo. If the picture is shifted, this is the first
// thing to adjust.
#define PANEL_W 135
#define PANEL_H 240
// v0 (40/53) left a ~white sliver on the left edge and along the bottom
// edge — the addressed window sat a few px short on both axes. Nudged +5
// on X (shift window right so col 0 reaches the true left edge) and -5 on
// Y (shift window up so the bottom edge stops falling outside it). Not a
// datasheet value — empirically tuned against this specific unit; if a
// sliver remains, keep nudging in the same direction by the remaining px.
#define PANEL_OFFSET_X 45
#define PANEL_OFFSET_Y 48

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED   0xF800
#define C_GREEN 0x07E0
#define C_BLUE  0x001F

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 1 /* rotation: landscape 240x135 */,
                                       false /* IPS */, PANEL_W, PANEL_H,
                                       PANEL_OFFSET_X, PANEL_OFFSET_Y,
                                       PANEL_OFFSET_X, PANEL_OFFSET_Y);

// ── QR test payload — dummy string only, per plan (not a real device_id) ───
#define QR_TEXT "GEEK-HELLO-WORLD-BRIDGE-TEST-SIXTY-FOUR-CHARS-PAYLOAD-SCAN-CHECK-DEVICE1"
#define QR_VERSION 3   // Settled config: 72 chars, V3 (29x29 modules, 116px
                        // square, 4px/module). Confirmed scannable, and
                        // leaves ~5 chars of headroom below V3-L's hard
                        // 77-char ceiling before anything would force a
                        // jump to V4 — which we confirmed fails to scan on
                        // this screen (132px square, only 3px margin left).

void drawColorTest() {
  Serial.println("[TEST] fillScreen RED — should look RED, not blue");
  gfx->fillScreen(C_RED);
  delay(1500);
  Serial.println("[TEST] fillScreen GREEN");
  gfx->fillScreen(C_GREEN);
  delay(1500);
  Serial.println("[TEST] fillScreen BLUE");
  gfx->fillScreen(C_BLUE);
  delay(1500);
}

void drawInfoScreen() {
  gfx->fillScreen(C_BLACK);
  gfx->setCursor(4, 4);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(1);
  gfx->println("GEEK bring-up v0");
  gfx->setCursor(4, 16);
  gfx->printf("pins: dc%d cs%d sck%d\n", LCD_DC, LCD_CS, LCD_SCK);
  gfx->setCursor(4, 26);
  gfx->printf("din%d rst%d bl%d\n", LCD_DIN, LCD_RST, LCD_BL);
  gfx->setCursor(4, 40);
  gfx->setTextColor(C_GREEN);
  gfx->println("if this reads clean,");
  gfx->setCursor(4, 50);
  gfx->println("pins are correct.");
  gfx->setCursor(4, 70);
  gfx->setTextColor(C_WHITE);
  gfx->println("QR test in 5s ->");
}

void drawQrScreen() {
  uint8_t qrData[qrcode_getBufferSize(QR_VERSION)];
  QRCode qr;
  if (qrcode_initText(&qr, qrData, QR_VERSION, ECC_LOW, QR_TEXT) != 0) {
    Serial.println("[QR] initText FAILED — QR_TEXT doesn't fit QR_VERSION's capacity");
    return;
  }

  // Size the module to fit the screen's shorter on-glass dimension (135px
  // tall in this landscape rotation — NOT PANEL_H=240, which is the
  // pre-rotation native height and was clipping the QR's top/bottom rows,
  // including the corner finder patterns scanners rely on first).
  const int SCREEN_W = 240; // landscape on-glass width after rotation
  const int SCREEN_H = 135; // landscape on-glass height after rotation
  // No quiet-zone reservation here — fillScreen(WHITE) below already covers
  // the full glass, so every QR module gets the biggest integer pixel size
  // that fits SCREEN_H (135/qr.size, printed to Serial below). A fractional
  // module size isn't renderable without soft/uneven edges that hurt
  // scanning more than a smaller code would.
  int moduleSizePx = SCREEN_H / qr.size;
  if (moduleSizePx < 1) moduleSizePx = 1;
  int qrPx = qr.size * moduleSizePx;
  int originX = (SCREEN_W - qrPx) / 2;
  // Flush against the bottom edge (0px margin below, remainder above) —
  // shifted south per operator request; this formula self-adjusts as
  // qrPx changes with QR_VERSION/module size.
  int originY = SCREEN_H - qrPx;

  gfx->fillScreen(C_WHITE);
  for (int y = 0; y < qr.size; y++) {
    for (int x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        gfx->fillRect(originX + x * moduleSizePx, originY + y * moduleSizePx,
                      moduleSizePx, moduleSizePx, C_BLACK);
      }
    }
  }
  Serial.printf("[QR] \"%s\" rendered, %d modules @ %dpx/module (~%d px square)\n",
                QR_TEXT, qr.size, moduleSizePx, qrPx);
  Serial.println("[QR] try scanning now — report distance/success");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n*** GeekDisplayTest v0 — ESP32-C6-GEEK bring-up ***");
  Serial.printf("[PINS] DC=%d CS=%d SCK=%d DIN=%d RST=%d BL=%d (candidate, unverified)\n",
                LCD_DC, LCD_CS, LCD_SCK, LCD_DIN, LCD_RST, LCD_BL);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  Serial.println("[LCD] backlight pin driven HIGH — if the panel has any");
  Serial.println("      light at all, this pin is at least in the right area");

  if (!gfx->begin()) {
    Serial.println("[LCD] gfx->begin() FAILED — check DC/CS/SCK/DIN pins");
    return;
  }
  Serial.println("[LCD] gfx->begin() OK — running color test");

  drawColorTest();
  drawInfoScreen();
  delay(5000);
  drawQrScreen();
}

void loop() {
  // Static after setup — re-power the board to re-run the sequence.
  delay(1000);
}
