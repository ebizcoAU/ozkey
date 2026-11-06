// ozstatus_lcd19.h — dlc6v2's one status page (F5-DISPLAY-1) on the Waveshare
// ESP32-C6 Touch-LCD 1.9" (ST7789 170x320, IPS). Pins, panel offsets, rotation
// and backlight polarity are copied from the v1 dlch584m_19.ino (same physical
// board; LockB). Read-only: the page shows state, it never changes it, and the
// touch panel is not used.
//
// Fields: FW version, device_id, locked/unlocked, claimed/UNCLAIMED, bridge link.
// The page is redrawn only when a field changes (polled every 250 ms from loop()).
// Reads of the lock are racy (the OZKIE task owns it), as for oz.state: a stale
// value lasts one poll.
#pragma once
#include <Arduino_GFX_Library.h>

#define S19_LCD_DC   6
#define S19_LCD_CS   7
#define S19_LCD_SCK  5
#define S19_LCD_DIN  4
#define S19_LCD_MISO 19
#define S19_LCD_RST  14
#define S19_LCD_BL   15   // ACTIVE-LOW on this board
#define S19_W 320
#define S19_H 170

#define S19_BLACK 0x0000
#define S19_WHITE 0xFFFF
#define S19_RED   0xF800
#define S19_GREEN 0x07E0
#define S19_AMBER 0xFD20
#define S19_GREY  0x8410

static Arduino_DataBus *s19_bus = new Arduino_HWSPI(S19_LCD_DC, S19_LCD_CS, S19_LCD_SCK, S19_LCD_DIN, S19_LCD_MISO);
static Arduino_GFX *s19_gfx = new Arduino_ST7789(s19_bus, S19_LCD_RST, 0, true, 170, 320, 35, 0, 35, 0);
static bool s19_ok;

typedef struct {
  uint8_t lock_state;   // 0 not fully locked, 1 locked, 2 unlocked
  uint8_t claimed;
  uint8_t bridge;       // oz_gatt_c6_bridge_link(): 0 none, 1 relay (no bridge auth), 2 authenticated
} s19_view;

static void s19_begin()
{
  pinMode(S19_LCD_BL, OUTPUT);
  digitalWrite(S19_LCD_BL, HIGH);   // off while the panel initialises
  s19_ok = s19_gfx->begin();
  if (!s19_ok) {
    Serial.println("[LCD] ST7789 begin FAILED: status page off");
    return;
  }
  s19_gfx->setRotation(3);
  s19_gfx->fillScreen(S19_BLACK);
  digitalWrite(S19_LCD_BL, LOW);
  Serial.println("[LCD] status page on (1.9\" ST7789)");
}

static void s19_row(int y, const char *label, const char *value, uint16_t colour)
{
  s19_gfx->fillRect(0, y, S19_W, 26, S19_BLACK);
  s19_gfx->setTextSize(1);
  s19_gfx->setTextColor(S19_GREY);
  s19_gfx->setCursor(10, y + 9);
  s19_gfx->print(label);
  s19_gfx->setTextSize(2);
  s19_gfx->setTextColor(colour);
  s19_gfx->setCursor(92, y + 5);
  s19_gfx->print(value);
}

static void s19_draw(const char *fw, const char *device_id, const s19_view *v, bool full)
{
  if (!s19_ok)
    return;
  if (full) {
    s19_gfx->fillScreen(S19_BLACK);
    s19_gfx->drawRect(0, 0, S19_W, S19_H, S19_AMBER);
    s19_gfx->setTextSize(1);
    s19_gfx->setTextColor(S19_AMBER);
    s19_gfx->setCursor(10, 8);
    s19_gfx->print("OZKIE v2 lock");
    s19_gfx->setTextColor(S19_WHITE);
    s19_gfx->setCursor(10, 22);
    s19_gfx->print(fw);
    s19_gfx->setCursor(10, 36);
    s19_gfx->print(device_id);
  }
  static const char *const ls[] = { "NOT LOCKED", "LOCKED", "UNLOCKED" };
  static const uint16_t lc[] = { S19_AMBER, S19_GREEN, S19_RED };
  uint8_t s = v->lock_state < 3 ? v->lock_state : 0;
  s19_row(54, "door", ls[s], lc[s]);
  s19_row(84, "owner", v->claimed ? "claimed" : "UNCLAIMED", v->claimed ? S19_GREEN : S19_AMBER);
  static const char *const bl[] = { "none", "relay, no bond", "bridge (bond)" };
  static const uint16_t bc[] = { S19_GREY, S19_AMBER, S19_GREEN };
  uint8_t b = v->bridge < 3 ? v->bridge : 0;
  s19_row(114, "bridge", bl[b], bc[b]);
}
