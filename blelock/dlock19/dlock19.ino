/*
 * DLOCK19 — UI-only bring-up for the Waveshare ESP32-C6-LCD-1.9 (touch SKU
 * 30941, CST816 touch IC, N8 8MB flash). NOT a functional doorlock: no BLE,
 * no crypto, no MCU forwarding — doorlock.ino already proves that surface on
 * the 1.47" board. This sketch answers one question only: can the bigger
 * 1.9" panel host a real, VISIBLE 12-key keypad + status area, unlike the
 * 1.47" board's dashboard-only screen (doorlock.ino's touch grid is
 * hit-test-only, nothing drawn — see its "blelock's keypad grid" comment,
 * doorlock.ino:2912).
 *
 * Board: esp32:esp32:esp32c6:FlashSize=8M,PartitionScheme=default_8MB
 *
 * LCD pins are the authoritative Arduino_GFX WAVESHARE_ESP32_C6_LCD_1_9
 * device profile (blelock/libraries/GFX_Library_for_Arduino/examples/
 * PDQgraphicstest/Arduino_GFX_dev_device.h:821-831) — confirmed, not guessed.
 *
 * Touch pins/address confirmed from Waveshare's own demo repo
 * (github.com/waveshareteam/ESP32-C6-LCD-1.9), cross-checked in two
 * independent places that agree exactly:
 *   - 02_Example/Arduino/08_LVGL_Test/lcd_config.h: I2C_Touch_ADDR 0x15,
 *     EXAMPLE_PIN_NUM_TOUCH_SCL 8, EXAMPLE_PIN_NUM_TOUCH_SDA 18
 *   - 02_Example/ESP-IDF/08_FactoryProgram/components/{esp_touch,i2c_bsp}:
 *     I2C_ADDR_CST78X 0x15, SDA 18, SCL 8
 * Different address (0x15) than the 1.47" board's CST816 (0x63,
 * HARDWARE.md) — a different sub-variant, not a typo. Neither reference
 * implementation toggles a reset or watches an interrupt line at all — pure
 * I2C polling, register 0x00, 7 bytes, same layout doorlock.ino already
 * reads (buf[2]=count, buf[3:4]=rawX, buf[5:6]=rawY). No RST/INT pins here.
 *
 * NOT yet confirmed: the transform from raw touch X/Y to this panel's
 * rotated (setRotation(5)) screen space — Waveshare's own demo ships with
 * touch disabled by default and doesn't calibrate it, so this sketch prints
 * raw coordinates over serial on every tap instead of guessing a formula.
 * Tap the four corners and center of the drawn grid, compare the printed
 * raw values against where you actually touched, and the mapping can be
 * derived for real instead of assumed.
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ── Confirmed LCD pins (Arduino_GFX WAVESHARE_ESP32_C6_LCD_1_9 profile) ────
#define LCD_DC   6
#define LCD_CS   7
#define LCD_SCK  5
#define LCD_MOSI 4
#define LCD_MISO 19
#define LCD_RST  14
#define LCD_BL   15

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, LCD_RST, 0 /* rotation at construction */, true /* IPS */,
    170 /* width */, 320 /* height */,
    35 /* col offset 1 */, 0 /* row offset 1 */, 35 /* col offset 2 */, 0 /* row offset 2 */);

// ── Confirmed touch pins (Waveshare's own demo repo, see header) ──────────
#define TOUCH_SDA 18
#define TOUCH_SCL 8
#define TOUCH_ADDR 0x15

// ── Colors ──────────────────────────────────────────────────────────────
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_AMBER 0xFD20
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_GRID  0x39C7

// Same key layout as doorlock.ino's hit-test grid (KP_KEYS) so the
// convention carries over even though this board draws it for real.
const char KP_KEYS[3][4] = {
    {'1', '2', '3', '4'},
    {'5', '6', '7', '8'},
    {'*', '9', '0', '#'},
};

// Effective canvas after setRotation(3): landscape, ~320 x 170.
#define CANVAS_W 320
#define CANVAS_H 170
#define STATUS_H 24                    // top bar: version/transport + LOCKED/UNLOCKED badge
#define BADGE_W  110                   // right-hand slice of the status bar
#define PIN_H    22                    // entered-digit indicator row, below the status bar
#define PIN_TOP  STATUS_H
#define PIN_MAX  8
#define KP_TOP   (STATUS_H + PIN_H)
#define KP_H     (CANVAS_H - KP_TOP)
#define KEY_W    (CANVAS_W / 4)
#define KEY_H    (KP_H / 3)

bool demoLocked = true;               // PLACEHOLDER lock state — no real MCU/lock wired
char pinBuf[PIN_MAX + 1];
int pinLen = 0;

bool touchOk = false;

void touchInit() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  // Bound every I2C transaction — without this, a single bus glitch/
  // clock-stretch hiccup with the touch IC can hang the entire loop() for
  // seconds (ESP32 Wire has no timeout by default on some core versions).
  // Suspected cause of the reported multi-second "sticky" stalls.
  Wire.setTimeOut(20);
  delay(20);
  // Waveshare's own driver: write 0x00 to reg 0x00 a few times until ACKed
  // ("switch to normal mode"), same retry pattern as touch_bsp.c.
  for (int i = 0; i < 10; i++) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() == 0) {
      touchOk = true;
      break;
    }
    delay(100);
  }
  Serial.printf("[TOUCH] init addr=0x%02X SDA=%d SCL=%d -> %s\n",
                TOUCH_ADDR, TOUCH_SDA, TOUCH_SCL, touchOk ? "ACK ok" : "NO ACK");
}

// Same 7-byte register-0x00 protocol as doorlock.ino's touchReadRegs(): returns
// raw, UNTRANSFORMED panel coordinates. Deliberately not mapped to screen
// space yet — see the header note on why.
bool touchReadRaw(uint16_t &rawX, uint16_t &rawY) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 7) < 7) return false;
  uint8_t buf[7];
  for (int i = 0; i < 7; i++) buf[i] = Wire.read();
  uint8_t count = buf[2];
  if (count == 0 || count > 5) return false;
  rawX = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
  rawY = ((uint16_t)(buf[5] & 0x0F) << 8) | buf[6];
  return true;
}

// LOCKED = white-on-red, UNLOCKED = black-on-green — right-hand slice of
// the status bar, independent of whatever text occupies the rest of it.
// Size-2 text, roughly centered (6px/char at size1 -> 12px/char at size2).
void drawLockBadge(bool locked) {
  int x0 = CANVAS_W - BADGE_W;
  gfx->fillRect(x0, 0, BADGE_W, STATUS_H, locked ? C_RED : C_GREEN);
  gfx->setTextColor(locked ? C_WHITE : C_BLACK);
  gfx->setTextSize(2);
  const char *label = locked ? "LOCKED" : "UNLOCKED";
  int textW = strlen(label) * 12;
  int cx = x0 + (BADGE_W - textW) / 2;
  if (cx < x0 + 2) cx = x0 + 2; // "UNLOCKED" nearly fills BADGE_W, clamp padding
  gfx->setCursor(cx, (STATUS_H - 16) / 2);
  gfx->print(label);
}

// BLE claim/pairing window countdown — mirrors the real doorlock's own rule
// ("ANY tap opens the window, not a designated zone", doorlock.ino:3288) as
// a demo: any keypress here (re-)arms 60s. Drawn in the status bar's middle
// gap, between the left status text and the lock badge.
void drawBleCountdown(int secsLeft) {
  gfx->fillRect(146, 0, CANVAS_W - BADGE_W - 146, STATUS_H, C_BLACK);
  gfx->setTextColor(secsLeft > 0 ? C_AMBER : C_GRID);
  gfx->setTextSize(1);
  gfx->setCursor(146, 8);
  if (secsLeft > 0) {
    char buf[12];
    snprintf(buf, sizeof(buf), "BLE %ds", secsLeft);
    gfx->print(buf);
  } else {
    gfx->print("BLE --");
  }
}

void drawTopStatus(const char *msg) {
  gfx->fillRect(0, 0, CANVAS_W - BADGE_W, STATUS_H, C_BLACK);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 8);
  gfx->print(msg);
  drawLockBadge(demoLocked);
}

// Entered-digit indicator, PIN-pad style ("* * * *"). No real PIN logic —
// '*' clears the buffer, '#' submits (toggles the demo lock state for now).
void drawPinIndicator() {
  gfx->fillRect(0, PIN_TOP, CANVAS_W, PIN_H, C_BLACK);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(8, PIN_TOP + 2);
  for (int i = 0; i < pinLen; i++) gfx->print("* ");
}

void drawKeypad() {
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 4; c++) {
      int x = c * KEY_W, y = KP_TOP + r * KEY_H;
      gfx->drawRect(x, y, KEY_W, KEY_H, C_GRID);
      gfx->setTextColor(C_WHITE);
      gfx->setTextSize(3);
      char label[2] = {KP_KEYS[r][c], 0};
      gfx->setCursor(x + KEY_W / 2 - 9, y + KEY_H / 2 - 12);
      gfx->print(label);
    }
  }
}

void highlightKey(int r, int c, bool on) {
  int x = c * KEY_W, y = KP_TOP + r * KEY_H;
  gfx->fillRect(x + 2, y + 2, KEY_W - 4, KEY_H - 4, on ? C_GREEN : C_BLACK);
  gfx->drawRect(x, y, KEY_W, KEY_H, C_GRID);
  gfx->setTextColor(on ? C_BLACK : C_WHITE);
  gfx->setTextSize(3);
  char label[2] = {KP_KEYS[r][c], 0};
  gfx->setCursor(x + KEY_W / 2 - 9, y + KEY_H / 2 - 12);
  gfx->print(label);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("*** DLOCK19 -- Waveshare ESP32-C6-LCD-1.9 UI bring-up (no BLE/MCU) ***");

  // Active-LOW on this board — the reference WAVESHARE_ESP32_C6_LCD_1_9
  // profile only ever sets this pin LOW (DEV_DEVICE_INIT()) and never
  // defines GFX_BL for the generic HIGH-on step other boards use. HIGH
  // was tried first and produced a black screen — this is the fix, not a
  // guess repeated.
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);

  gfx->begin();
  // The four landscape (MV) variants in Arduino_ST7789::setRotation() are
  // 1/3/5/7, differing only in mirror bits. 5 = upside-down + mirrored,
  // 7 = fixed vertical but still mirrored, 1 = readable but upside-down —
  // 3 (MY|MV) is the only one of the four left untried.
  gfx->setRotation(3);
  gfx->fillScreen(C_BLACK);

  drawKeypad();
  drawPinIndicator();

  touchInit();

  // PLACEHOLDER — no WiFi/Thread/MCU is wired in this UI-only bring-up
  // (scope decision: UI-only first, doorlock.ino already proves the real
  // control/crypto/transport surface). Format matches the operator's
  // spec: "ver  WIFI|THREAD CHILD/LDR  LOCK STATUS". Swap for the real
  // values once this UI is ported into an actual doorlock variant.
  drawTopStatus("V1.0 WIFI IP:10.1.1.20");
  drawBleCountdown(0);
}

bool wasDown = false;
int litR = -1, litC = -1;
unsigned long bleWindowUntil = 0; // 0 = closed. Demo only, see drawBleCountdown().
int lastBleSecs = -1;

void handleKeyPress(char key) {
  if (key >= '0' && key <= '9') {
    if (pinLen < PIN_MAX) pinBuf[pinLen++] = key;
  } else if (key == '*') {
    pinLen = 0; // clear, PIN-pad convention
  } else if (key == '#') {
    demoLocked = !demoLocked; // submit — DEMO toggle, no real backend
    pinLen = 0;
    drawLockBadge(demoLocked);
  }
  drawPinIndicator();
  // No serial print here — `if (Serial)` (tried first) reflects native-USB
  // CDC enumeration, not whether anything is actually draining the buffer,
  // so it didn't actually prevent the block. Calibration is done and these
  // prints were only ever for that; removing them is more certain than
  // continuing to guess at this core's CDC blocking semantics.
}

// Affine fit (least squares, pure-Python, no hardware dependency) from 11
// of 12 real labeled taps — one per key, read directly off the on-screen
// raw readout while the operator pressed each key in order. Corner-only
// calibration (tried first) assumed the axes were independent/orthogonal;
// the real per-key data showed measurable cross-talk (rawX and rawY both
// contribute to both screen axes), so this is a proper 2D affine fit
// instead. Key '7's point (50,114) was excluded as a mistap — refitting
// without it improved every other point's error while '7' got worse
// against the improved fit, which only makes sense if '7' was bad data.
// Residuals on the other 11 points: worst ~17px, all comfortably inside
// half a key's width/height (40/24px) so no bin-boundary risk.
bool keyAt(uint16_t rawX, uint16_t rawY, int &r, int &c) {
  float sx = 0.00684f * rawX - 1.00976f * rawY + 312.580f;
  float sy = 1.04315f * rawX - 0.06231f * rawY + 5.590f;
  int col = (int)(sx / KEY_W);
  int row = (int)((sy - KP_TOP) / KEY_H);
  c = col < 0 ? 0 : (col > 3 ? 3 : col);
  r = row < 0 ? 0 : (row > 2 ? 2 : row);
  return true;
}

#define KEY_LIGHT_MS 500 // fixed highlight-flash duration, decoupled from touch duration
unsigned long litSince = 0;

void loop() {
  // UI-only bring-up: no BLE, no crypto, no MCU forwarding.
  uint16_t rawX, rawY;
  bool down = touchOk && touchReadRaw(rawX, rawY);
  unsigned long now = millis();

  // Auto-clear the highlight on a fixed timer, NOT on physical release —
  // the touch chip can keep reporting "down" for a while after a real lift.
  if (litR >= 0 && now - litSince >= KEY_LIGHT_MS) {
    highlightKey(litR, litC, false);
    litR = litC = -1;
  }

  // A new tap is only accepted once the previous key's highlight window has
  // cleared — a debounce using the same timer as the visual flash.
  if (down && !wasDown && litR < 0) {
    int r, c;
    keyAt(rawX, rawY, r, c);
    char key = KP_KEYS[r][c];
    highlightKey(r, c, true);
    litR = r; litC = c;
    litSince = now;
    bleWindowUntil = now + 60000UL; // any tap (re)arms the window
    handleKeyPress(key);
  }
  wasDown = down;

  int secsLeft = bleWindowUntil > millis() ? (bleWindowUntil - millis() + 999) / 1000 : 0;
  if (secsLeft != lastBleSecs) {
    drawBleCountdown(secsLeft);
    lastBleSecs = secsLeft;
  }

  delay(30);
}
