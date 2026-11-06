// dlc6v2 — an OZKIE v2 lock on the bench's ESP32-C6 boards (ozkey-55 F5).
//
// The same ozcore engine as AK1037, on its second port (ozcore/port/esp32c6),
// so OZKIE v2 runs lock → bridge → server → dashboard before the AK1037 kit.
// No display, motor or keypad: the motor is log-only (every move ends
// "unconfirmed") and the keypad/card come from the serial shell.
//
// The v1 sketches (dlch584m_19/147) are untouched; one C6 board stays on v1
// for the regression gate (ozkey-55 F5).
//
// Serial shell (115200, one command per line), as on AK1037:
//   oz.touch           someone is at the door (opens the advertising window)
//   oz.code 123456     the owner typed the pairing code
//   oz.key 482611#     keys on the keypad ('*' clears, '#' enters)
//   oz.card 04a1b2c3   a card's UID at the reader
//   oz.state           lock state, last move, claimed, connected
//   oz.wipe CONFIRM    erase the lock's identity and bonds, then reboot
//
// Build: make -C blelock compile BOARD=dlc6v2   (stages ozcore first)

#include <Arduino.h>
#include "esp_mac.h"
#include "oz_port.h"
#include "oz_lock.h"
#include "oz_port_esp32c6.h"
#include "oz_gatt_esp32c6.h"

#define FW_VERSION "dlc6v2-0.5"

static oz_lock lock;

static void actuate(void *app, int unlock)
{
  (void)app;
  oz_port_log("actuate: %s", unlock ? "UNLOCK" : "lock");
  oz_gatt_c6_actuate(unlock);
}

static void prompt(void *app, int on)
{
  (void)app;
  oz_gatt_c6_pair_prompt(on);
  oz_port_log("keypad: %s", on ? "enter the pairing code (oz.key NNNNNN#)" : "pairing prompt off");
}

static void cmd(String line)
{
  line.trim();
  if (line == "oz.touch") {
    oz_gatt_c6_touch();
  } else if (line.startsWith("oz.code ")) {
    oz_gatt_c6_pair_code((uint32_t)strtoul(line.substring(8).c_str(), NULL, 10));
  } else if (line.startsWith("oz.key ")) {
    for (char k : line.substring(7)) oz_gatt_c6_key(k);
  } else if (line.startsWith("oz.card ")) {
    String h = line.substring(8);
    uint8_t id[16];
    size_t n = 0;
    bool ok = h.length() >= 2 && h.length() % 2 == 0 && h.length() <= 32;
    for (size_t i = 0; ok && i < h.length(); i += 2) {
      char b[3] = { h[i], h[i + 1], 0 };
      char *end;
      unsigned long v = strtoul(b, &end, 16);
      if (*end) ok = false;
      else id[n++] = (uint8_t)v;
    }
    if (!ok || oz_gatt_c6_card(id, n) != OZ_OK) Serial.println("usage: oz.card 04a1b2c3   (UID in hex, 1..16 bytes)");
  } else if (line == "oz.state") {
    static const char *const ls[] = { "not fully locked", "locked", "unlocked" };
    static const char *const mv[] = { "none", "ok", "unconfirmed", "JAMMED" };
    uint8_t s = lock.lock_state, m = lock.last_move;
    Serial.printf("oz.state: %s %s, %s, last move %s, %s\n", lock.device_id, FW_VERSION,
                  s < 3 ? ls[s] : "?", m < 4 ? mv[m] : "?",
                  oz_lock_claimed(&lock) ? (oz_gatt_c6_connected() ? "claimed, connected" : "claimed")
                                         : (oz_gatt_c6_connected() ? "UNCLAIMED, connected" : "UNCLAIMED"));
  } else if (line == "oz.wipe CONFIRM") {
    Serial.println(oz_esp32c6_kv_wipe() == OZ_OK ? "oz.wipe: done, rebooting" : "oz.wipe: FAILED");
    delay(200);
    ESP.restart();
  } else if (line.length()) {
    Serial.println("commands: oz.touch | oz.code N | oz.key K… | oz.card HEX | oz.state | oz.wipe CONFIRM");
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // a closed USB CDC must never block the loop (v1 lesson)
  delay(300);
  Serial.printf("[FW] %s\n", FW_VERSION);

  static const oz_lock_hooks hooks = { actuate, prompt, NULL };
  uint8_t a[8];  /* 6 used; room for an 8-byte MAC API */
  char id[4 + 12 + 1];
  /* device_id = "ozk-" + the BASE MAC (eFuse), as v1 does: the same id as the
   * board's USB serial number, the bench map and the server's lock row. The BLE
   * address is base + 2 on ESP32-C6; using it gave the board a second identity
   * on v2 (ozk-…ba instead of ozk-…b8, found on LockB 2026-11-05).
   * Not esp_efuse_mac_get_default(): on C6 it returns the 8-byte EUI-64
   * (ac:eb:e6:ff:fe:3a:ca:b8), which overran a[6] in dlc6v2-0.2. */
  esp_read_mac(a, ESP_MAC_WIFI_STA);
  snprintf(id, sizeof id, "ozk-%02x%02x%02x%02x%02x%02x", a[0], a[1], a[2], a[3], a[4], a[5]);

  if (oz_esp32c6_init() != OZ_OK || oz_lock_init(&lock, id, FW_VERSION, &hooks) != OZ_OK) {
    Serial.println("oz.start: init FAILED (kv or lock); halting");
    for (;;) delay(1000);
  }
  if (oz_gatt_c6_start(&lock) != OZ_OK) {
    Serial.println("oz.start: GATT start FAILED; halting");
    for (;;) delay(1000);
  }
  Serial.printf("oz.start: %s %s\n", id, oz_lock_claimed(&lock) ? "claimed" : "UNCLAIMED");
}

void loop()
{
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) cmd(buf);
      buf = "";
    } else if (buf.length() < 80) {
      buf += c;
    }
  }
  delay(10);
}
