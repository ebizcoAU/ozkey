/*
 * blelock — OZLOCK doorlock emulator v0 (ozkey-08 §10)
 * Board : Waveshare ESP32-C6 Touch-LCD 1.47" (pin map: blelock/HARDWARE.md)
 * Flow  : BLE "OZLOCK" advertise → BANOI writes ProvisionPayload →
 *         WiFi → MQTT :1883 → enroll (ozkey/<site>/locks/<id>/enroll) →
 *         enrollment_ack → OPERATIONAL: 3×4 touch keypad, DPID 21/22
 *         credential frames, heartbeat, door log publishes.
 * Wire  : identical to LockSim (ozlockserv untouched). Frames: ozkey-02 §4 /
 *         ozkey_commissioner DpidFrames (55 AA 00 06 … checksum).
 * Built from the PROVEN test sketches: BLE.ino (advertise+GATT),
 * Wifi.ino (BLE+WiFi coex, BGR panel), Touch.ino (CST816-class @0x63,
 * hw-reset GPIO20, 7-byte read), TicTacToe.ino (grid hit-testing).
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ── Hardware pins (HARDWARE.md, operator-verified) ──────────────────────────
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_RST 22
#define LCD_BL 23
// Touch = CST816-class @0x63 on SDA18/SCL19 with hw reset GPIO20 — the
// values Touch.ino/TicTacToe.ino verified on this batch. (HARDWARE.md's
// original 0x3B/SDA4 section was wrong for this board.)
#define I2C_SDA 18
#define I2C_SCL 19
#define TOUCH_RST 20
#define TOUCH_INT 21
#define TOUCH_ADDR 0x63

// ── BGR-corrected palette (panel is BGR — Wifi.ino finding) ────────────────
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0x001F    // red on BGR panel
#define C_BLUE 0xF800   // blue on BGR panel
#define C_GREEN 0x07E0  // green channel is symmetric
#define C_AMBER 0x051F  // amber (r31 g41 b0) with R/B swapped
#define C_GREY 0x8410
#define C_DIM 0x39E7

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false /*BGR*/, 172, 320, 34, 0, 34, 0);

// ── GATT contract (blelock/CONTRACT.md + ozkey-08 §10.3) ────────────────────
#define BLE_NAME "OZLOCK"
#define SVC_UUID "4f5a4b31-0001-4c4f-434b-000000000001"
#define CHR_PROVISION "4f5a4b31-0002-4c4f-434b-000000000001"
#define CHR_STATUS "4f5a4b31-0003-4c4f-434b-000000000001"
#define CHR_INFO "4f5a4b31-0004-4c4f-434b-000000000001"
#define FW_VERSION "blelock-1.0"

// ── State machine (ozkey-08 §10.5) ──────────────────────────────────────────
enum LockState { ST_ADVERTISING, ST_JOINING, ST_OPERATIONAL };
LockState state = ST_ADVERTISING;

Preferences prefs;   // namespace "blelock" — provisioning + enrollment
Preferences creds;   // namespace "creds"   — PIN slots

// Provisioned config (NVS-backed)
String cfgSsid, cfgPass, cfgBrokerHost, cfgServerIp, cfgSiteId, cfgName, cfgDeviceId;
uint16_t cfgBrokerPort = 1883, cfgServerPort = 4200;
uint32_t cfgHeartbeatS = 60;
bool provisioned = false, enrolled = false;

String deviceId, macStr; // ozk-<machex> minted from factory MAC (§10.2)

// BLE
BLEServer *bleServer = nullptr;
BLECharacteristic *chrStatus = nullptr, *chrInfo = nullptr;
volatile bool bleClientConnected = false;
String provBuf; // chunked-write accumulator (a chunk starting '{' resets it)

// Networking
WiFiClient wifiTcp;
PubSubClient mqtt(wifiTcp);
unsigned long lastHeartbeat = 0, lastMqttAttempt = 0, wifiJoinStart = 0;
unsigned long lastEnrollSent = 0;
uint8_t enrollAttempts = 0;
String topicCommand, topicEnroll, topicHeartbeat, topicLog;

// Keypad / lock UI state
String pinEntry;
String lockStatus = "LOCKED"; // LOCKED | UNLOCKED
unsigned long unlockAt = 0;
const unsigned long UNLOCK_MS = 5000; // proven auto-relock
uint8_t pinFails = 0;
unsigned long lockoutUntil = 0;
bool screenDirty = true;
String joinLine1 = "", joinLine2 = "";
char hlKey = 0;            // key currently drawn highlighted (feedback)
unsigned long hlUntil = 0; // when to repaint it normal

// Touch
bool touchWasDown = false;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────
String asciiOnly(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 32 && c < 127) out += c;
  }
  return out.length() ? out : String("Doorlock");
}

String isoNow() {
  time_t now = time(nullptr);
  if (now < 1600000000) return String(""); // NTP not yet synced
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return String(buf);
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Status ladder (notify BANOI over BLE + serial log)
// ─────────────────────────────────────────────────────────────────────────────
void notifyStatus(const char *wire) {
  Serial.printf("[STATUS] %s\n", wire);
  if (chrStatus != nullptr) {
    chrStatus->setValue((uint8_t *)wire, strlen(wire));
    if (bleClientConnected) chrStatus->notify();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Screens (rotation 5 landscape 320×172, BGR palette)
// ─────────────────────────────────────────────────────────────────────────────
void drawAdvertising() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, 320, 172, C_AMBER);
  gfx->setTextColor(C_AMBER);
  gfx->setTextSize(1);
  gfx->setCursor(15, 12);
  gfx->println("OZLOCK DOORLOCK EMULATOR");
  gfx->setCursor(15, 28);
  gfx->print("BLE: ");
  gfx->print(bleClientConnected ? "APP CONNECTED" : "ADVERTISING...");
  gfx->setTextSize(3);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(52, 70);
  gfx->println("OZLOCK");
  gfx->setTextSize(1);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(15, 120);
  gfx->print("device_id: ");
  gfx->println(deviceId);
  gfx->setCursor(15, 136);
  gfx->print("mac: ");
  gfx->println(macStr);
  gfx->setCursor(15, 152);
  gfx->setTextColor(C_DIM);
  gfx->println("Open BANOI > Doorlock to pair");
  gfx->setCursor(15, 162);
  gfx->println("reset: tap * then 5 (left edge)");
}

void drawJoining() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, 320, 172, C_BLUE);
  gfx->setTextColor(C_BLUE);
  gfx->setTextSize(1);
  gfx->setCursor(15, 12);
  gfx->println("OZLOCK - CONNECTING");
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(15, 36);
  gfx->println(asciiOnly(cfgName.length() ? cfgName : deviceId));
  gfx->setTextSize(1);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(15, 70);
  gfx->println(joinLine1); // WiFi line (incl. IP address — operator req)
  gfx->setCursor(15, 88);
  gfx->println(joinLine2); // server line
  gfx->setCursor(15, 120);
  gfx->setTextColor(C_DIM);
  gfx->print("device_id: ");
  gfx->println(deviceId);
  gfx->setCursor(15, 152);
  gfx->println("reset: tap * then 5 (left edge)");
}

// Keypad geometry — FULL-SCREEN 3-row × 4-column grid. The touch panel's
// VERTICAL axis is coarse/filtered while the horizontal one is fast and
// accurate (bench calibration), so the layout leans on width: only 3 rows
// on the bad axis, 4 columns on the good one. Every cell is maximal and
// hit-testing has NO dead zones. Background colour IS the lock status:
// RED = LOCKED, GREEN = UNLOCKED. 12px top strip: name + PIN dots + the
// live key indicator + link dot.
const int KP_Y = 12;      // top strip height
const int KP_ROW_H = 53;  // 12 + 3×53 = 171 — grid fills the panel
const char KP_KEYS[3][4] = {
  {'1','2','3','4'},
  {'5','6','7','8'},
  {'*','9','0','#'},
};

uint16_t lockBg() { return lockStatus == "UNLOCKED" ? C_GREEN : C_RED; }
uint16_t lockFg() { return lockStatus == "UNLOCKED" ? C_BLACK : C_WHITE; }

void drawKey(int r, int c, bool hl) {
  int x = 2 + c * 80, y = KP_Y + r * KP_ROW_H + 2;
  int w = 76, h = KP_ROW_H - 4;
  gfx->fillRoundRect(x, y, w, h, 8, hl ? C_WHITE : C_BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(hl ? C_BLACK : C_WHITE);
  gfx->setCursor(x + w / 2 - 9, y + h / 2 - 11);
  gfx->print(KP_KEYS[r][c]);
}

void drawKeypad() {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++) drawKey(r, c, false);
}

// Pressed-key feedback (operator): the registered key flashes WHITE for
// 200ms so a mis-decode is visible immediately.
void highlightKey(char k) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      if (KP_KEYS[r][c] == k) {
        drawKey(r, c, true);
        hlKey = k;
        hlUntil = millis() + 200;
        return;
      }
}

void unhighlightKey() {
  if (!hlKey) return;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      if (KP_KEYS[r][c] == hlKey) drawKey(r, c, false);
  hlKey = 0;
}

void drawPinDots() {
  gfx->fillRect(0, 0, 320, KP_Y, lockBg());
  gfx->setTextSize(1);
  gfx->setTextColor(lockFg());
  gfx->setCursor(4, 2);
  String nm = asciiOnly(cfgName.length() ? cfgName : deviceId);
  if (nm.length() > 14) nm = nm.substring(0, 14);
  gfx->print(nm);
  gfx->setCursor(150, 2);
  for (size_t i = 0; i < pinEntry.length(); i++) gfx->print('*');
  // link dot far right: black = online, amber = offline
  bool up = (WiFi.status() == WL_CONNECTED) && mqtt.connected();
  gfx->fillCircle(312, 6, 4, up ? C_BLACK : C_AMBER);
}

void drawOperational() {
  gfx->fillScreen(lockBg()); // the colour is the state — RED/GREEN
  drawPinDots();
  drawKeypad();
}

// Full-screen colour IS the state signal (operator: RED locked, GREEN open).
void drawFlash(const char *msg, uint16_t bg, uint16_t fg) {
  hlKey = 0; // flash owns the screen — cancel any pending key un-highlight
  gfx->fillScreen(bg);
  gfx->setTextSize(4);
  gfx->setTextColor(fg);
  int16_t x = 160 - (int)strlen(msg) * 12;
  gfx->setCursor(x > 0 ? x : 4, 70);
  gfx->println(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// NVS
// ─────────────────────────────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("blelock", true);
  provisioned = prefs.getBool("prov", false);
  enrolled = prefs.getBool("enrolled", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgBrokerHost = prefs.getString("bhost", "");
  cfgBrokerPort = prefs.getUShort("bport", 1883);
  cfgServerIp = prefs.getString("sip", "");
  cfgServerPort = prefs.getUShort("sport", 4200);
  cfgSiteId = prefs.getString("site", "lab");
  cfgName = prefs.getString("name", "");
  cfgHeartbeatS = prefs.getUInt("hb", 60);
  prefs.end();
}

void saveConfig() {
  prefs.begin("blelock", false);
  prefs.putBool("prov", provisioned);
  prefs.putBool("enrolled", enrolled);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("bhost", cfgBrokerHost);
  prefs.putUShort("bport", cfgBrokerPort);
  prefs.putString("sip", cfgServerIp);
  prefs.putUShort("sport", cfgServerPort);
  prefs.putString("site", cfgSiteId);
  prefs.putString("name", cfgName);
  prefs.putUInt("hb", cfgHeartbeatS);
  prefs.end();
}

void factoryReset() {
  Serial.println("[RESET] factory reset — wiping NVS");
  prefs.begin("blelock", false); prefs.clear(); prefs.end();
  creds.begin("creds", false); creds.clear(); creds.end();
  drawFlash("RESET", C_AMBER, C_BLACK);
  delay(800);
  ESP.restart();
}

// PIN slots: creds key "s<slot>" = "<pin>|<startUnix>|<endUnix>"
void storePin(uint16_t slot, const String &pin, uint32_t startU, uint32_t endU) {
  creds.begin("creds", false);
  char key[8]; snprintf(key, sizeof(key), "s%u", slot);
  creds.putString(key, pin + "|" + String(startU) + "|" + String(endU));
  creds.end();
  // v0 bench: PIN in the clear on serial (TESTING.md known limits) — the
  // "what PIN did the lock actually receive?" question must be answerable.
  Serial.printf("[CRED] slot %u stored pin='%s' (valid %u..%u)\n",
                slot, pin.c_str(), startU, endU);
}

void deletePin(uint16_t slot) {
  creds.begin("creds", false);
  char key[8]; snprintf(key, sizeof(key), "s%u", slot);
  creds.remove(key);
  creds.end();
  Serial.printf("[CRED] slot %u revoked\n", slot);
}

// returns matched slot or -1
int checkPin(const String &entry) {
  creds.begin("creds", true);
  time_t now = time(nullptr);
  int matched = -1;
  int slots = 0;
  for (uint16_t slot = 0; slot <= 64 && matched < 0; slot++) {
    char key[8]; snprintf(key, sizeof(key), "s%u", slot);
    String v = creds.getString(key, "");
    if (!v.length()) continue;
    slots++;
    int p1 = v.indexOf('|'), p2 = v.lastIndexOf('|');
    if (p1 < 0 || p2 <= p1) continue;
    String pin = v.substring(0, p1);
    uint32_t startU = strtoul(v.substring(p1 + 1, p2).c_str(), nullptr, 10);
    uint32_t endU = strtoul(v.substring(p2 + 1).c_str(), nullptr, 10);
    if (pin != entry) continue;
    // honor validity window only when the clock is synced
    if (now > 1600000000 && (now < (time_t)startU || now > (time_t)endU)) {
      Serial.printf("[PIN] slot %u matches but OUTSIDE window (now=%lu not in %u..%u)\n",
                    slot, (unsigned long)now, startU, endU);
      continue;
    }
    matched = slot;
  }
  creds.end();
  // Bench diagnostics: say WHY a PIN was rejected, not just "denied".
  if (matched < 0) {
    Serial.printf("[PIN] entered='%s' -> NO MATCH (%d slot(s) stored, now=%lu)\n",
                  entry.c_str(), slots, (unsigned long)now);
  } else {
    Serial.printf("[PIN] entered='%s' -> slot %d MATCH\n", entry.c_str(), matched);
  }
  return matched;
}

// ─────────────────────────────────────────────────────────────────────────────
// DPID frame parse (ozkey-02 §4 / DpidFrames — the hardware truth)
// 55 AA 00 06 <len:2BE> <dpid> <type> <len:2BE> <value…> <checksum>
// ─────────────────────────────────────────────────────────────────────────────
void handleDpidFrame(const uint8_t *f, size_t n) {
  if (n < 8 || f[0] != 0x55 || f[1] != 0xAA) { Serial.println("[DPID] bad header"); return; }
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += f[i];
  if (sum != f[n - 1]) { Serial.println("[DPID] bad checksum"); return; }
  uint8_t dpid = f[6], type = f[7];
  uint16_t vlen = ((uint16_t)f[8] << 8) | f[9];
  const uint8_t *v = f + 10;
  if (10 + vlen + 1 > n) { Serial.println("[DPID] length overflow"); return; }

  if (dpid == 21 && type == 0x00 && vlen >= 11) { // add temp PIN
    uint16_t slot = ((uint16_t)v[0] << 8) | v[1];
    uint16_t pinLen = vlen - 2 - 8;
    String pin;
    for (uint16_t i = 0; i < pinLen; i++) pin += (char)v[2 + i];
    uint32_t startU = ((uint32_t)v[2+pinLen] << 24) | ((uint32_t)v[3+pinLen] << 16) | ((uint32_t)v[4+pinLen] << 8) | v[5+pinLen];
    uint32_t endU = ((uint32_t)v[6+pinLen] << 24) | ((uint32_t)v[7+pinLen] << 16) | ((uint32_t)v[8+pinLen] << 8) | v[9+pinLen];
    storePin(slot, pin, startU, endU);
    publishLog("key_synced", (String("slot ") + slot).c_str());
    screenDirty = true;
  } else if (dpid == 22 && type == 0x00 && vlen >= 2) { // revoke PIN
    uint16_t slot = ((uint16_t)v[0] << 8) | v[1];
    deletePin(slot);
    publishLog("key_revoked", (String("slot ") + slot).c_str());
  } else if (dpid == 1 && type == 0x01 && vlen >= 1 && v[0] == 0x01) { // remote unlock
    doUnlock("remote unlock");
  } else {
    Serial.printf("[DPID] unhandled dpid=%u type=%u len=%u\n", dpid, type, vlen);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT wire (LockSim-identical; ozlockserv untouched)
// ─────────────────────────────────────────────────────────────────────────────
void publishLog(const char *result, const char *detail) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["result"] = result;
  doc["detail"] = detail;
  String ts = isoNow();
  if (ts.length()) doc["ts"] = ts;
  String out; serializeJson(doc, out);
  mqtt.publish(topicLog.c_str(), out.c_str());
  Serial.printf("[LOG->] %s %s\n", result, detail);
}

void publishHeartbeat() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  String out; serializeJson(doc, out);
  mqtt.publish(topicHeartbeat.c_str(), out.c_str());
}

void publishEnroll() {
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  if (cfgName.length()) doc["name"] = cfgName;
  String out; serializeJson(doc, out);
  mqtt.publish(topicEnroll.c_str(), out.c_str());
  lastEnrollSent = millis();
  enrollAttempts++;
  Serial.printf("[ENROLL->] attempt %u\n", enrollAttempts);
}

void doUnlock(const char *via) {
  lockStatus = "UNLOCKED";
  unlockAt = millis();
  pinEntry = "";
  pinFails = 0;
  publishLog("granted", via);
  drawFlash("UNLOCKED", C_GREEN, C_BLACK); // whole screen GREEN = open
  screenDirty = false; // flash owns the screen until relock redraw
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String body; body.reserve(length);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  Serial.printf("[MQTT<-] %s %s\n", topic, body.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  const char *op = doc["op"] | (const char *)nullptr;
  if (op && (strcmp(op, "factory_reset") == 0 || strcmp(op, "unpair") == 0)) {
    // Server-initiated unpair (BANOI "Gỡ khoá" → DELETE /locks/:id) — wipe
    // and return to ADVERTISING so the lock is immediately re-pairable.
    Serial.println("[MQTT<-] factory_reset (unpaired by app/server)");
    factoryReset();
    return;
  }
  if (op && strcmp(op, "enrollment_ack") == 0) {
    enrolled = true;
    const char *label = doc["label"] | "";
    if (!cfgName.length() && strlen(label)) cfgName = label;
    if (doc["heartbeat_s"].is<uint32_t>()) cfgHeartbeatS = doc["heartbeat_s"].as<uint32_t>();
    // broker creds stored for the day the broker enforces them (lab: open)
    prefs.begin("blelock", false);
    prefs.putString("buser", doc["broker_username"] | "");
    prefs.putString("bsecret", doc["broker_secret"] | "");
    prefs.end();
    saveConfig();
    notifyStatus("ENROLLED");
    state = ST_OPERATIONAL;
    screenDirty = true;
    return;
  }
  if (op && strcmp(op, "enrollment_nack") == 0) {
    Serial.printf("[ENROLL] NACK: %s\n", (const char *)(doc["error"] | "?"));
    notifyStatus("ENROLL_FAIL");
    joinLine2 = "Server refused: pairing not registered";
    screenDirty = true;
    return;
  }
  // command envelope {action, grant_id, payload_hex}. ⚠ OZLOCK publishes
  // SPACED hex ("55 AA 00 06 …", toSpacedHex) — the original strict parser
  // bailed on the first space, silently dropping EVERY credential frame
  // (grants showed "synced" server-side while the lock stayed at 0 slots).
  // Parse hex pairs, skipping whitespace, like LockSim does.
  const char *hex = doc["payload_hex"] | (const char *)nullptr;
  if (hex) {
    static uint8_t frame[256];
    size_t fn = 0;
    int hi = -1;
    for (const char *p = hex; *p && fn < sizeof(frame); p++) {
      if (*p == ' ' || *p == ':') continue;
      int v = hexNibble(*p);
      if (v < 0) { Serial.println("[DPID] bad hex in payload_hex"); return; }
      if (hi < 0) {
        hi = v;
      } else {
        frame[fn++] = (hi << 4) | v;
        hi = -1;
      }
    }
    if (fn >= 4) handleDpidFrame(frame, fn);
  }
}

void ensureMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 4000) return;
  lastMqttAttempt = millis();
  if (state == ST_JOINING) { joinLine2 = "Server: connecting..."; screenDirty = true; notifyStatus("BROKER_JOINING"); }
  Serial.printf("[MQTT] connecting %s:%u as %s\n", cfgBrokerHost.c_str(), cfgBrokerPort, deviceId.c_str());
  mqtt.setServer(cfgBrokerHost.c_str(), cfgBrokerPort);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(onMqttMessage);
  if (mqtt.connect(deviceId.c_str())) {
    mqtt.subscribe(topicCommand.c_str(), 1);
    Serial.println("[MQTT] connected + subscribed command topic");
    if (state == ST_JOINING) {
      notifyStatus("BROKER_OK");
      joinLine2 = "Server: OK - enrolling...";
      screenDirty = true;
      enrollAttempts = 0;
      publishEnroll();
    } else {
      publishHeartbeat(); // flush any queued grants fast after reconnect
    }
  } else if (state == ST_JOINING) {
    notifyStatus("BROKER_FAIL");
    joinLine2 = "Server: KHONG TOI DUOC";
    screenDirty = true;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Provisioning (BLE write → JOINING)
// ─────────────────────────────────────────────────────────────────────────────
void applyProvision(JsonDocument &doc) {
  String mode = doc["mode"] | "";
  if (mode != "ozkey-cloud" && mode != "ozkey-local") { notifyStatus("ENROLL_FAIL"); return; }
  String pid = doc["device_id"] | "";
  if (pid.length() && pid != deviceId) {
    Serial.printf("[PROV] device_id mismatch (%s != %s)\n", pid.c_str(), deviceId.c_str());
    notifyStatus("ENROLL_FAIL");
    return;
  }
  cfgSsid = (const char *)(doc["ssid"] | "");
  cfgPass = (const char *)(doc["password"] | "");
  cfgBrokerHost = (const char *)(doc["broker_host"] | "");
  cfgBrokerPort = doc["broker_tcp_port"] | 1883;
  cfgServerIp = (const char *)(doc["server_ip"] | "");
  cfgServerPort = doc["server_port"] | 4200;
  cfgSiteId = (const char *)(doc["site_id"] | "lab");
  cfgName = (const char *)(doc["name"] | "");
  cfgHeartbeatS = doc["heartbeat_s"] | 60;
  if (!cfgSsid.length() || !cfgBrokerHost.length()) { notifyStatus("ENROLL_FAIL"); return; }

  provisioned = true;
  enrolled = false;
  saveConfig();
  buildTopics();

  state = ST_JOINING;
  joinLine1 = "WiFi: joining " + cfgSsid + "...";
  joinLine2 = "Server: " + cfgBrokerHost + ":" + String(cfgBrokerPort);
  screenDirty = true;
  notifyStatus("WIFI_JOINING");
  Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(),
                cfgPass.length());
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  wifiJoinStart = millis();
}

class ProvisionCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String chunk = String(c->getValue().c_str());
    if (!chunk.length()) return;
    if (chunk[0] == '{') provBuf = chunk; else provBuf += chunk;
    JsonDocument doc;
    if (deserializeJson(doc, provBuf) == DeserializationError::Ok) {
      Serial.println("[PROV] payload complete");
      provBuf = "";
      applyProvision(doc);
    }
  }
};

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleClientConnected = true;
    screenDirty = true;
    notifyStatus("BLE_OK");
  }
  void onDisconnect(BLEServer *) override {
    bleClientConnected = false;
    screenDirty = true;
    if (state == ST_ADVERTISING) { delay(300); BLEDevice::startAdvertising(); }
  }
};

void startBle() {
  BLEDevice::init(BLE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCB());
  BLEService *svc = bleServer->createService(SVC_UUID);

  BLECharacteristic *prov = svc->createCharacteristic(CHR_PROVISION, BLECharacteristic::PROPERTY_WRITE);
  prov->setCallbacks(new ProvisionCB());

  chrStatus = svc->createCharacteristic(CHR_STATUS,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chrStatus->addDescriptor(new BLE2902());
  chrStatus->setValue("BLE_OK");

  chrInfo = svc->createCharacteristic(CHR_INFO, BLECharacteristic::PROPERTY_READ);
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  doc["name"] = cfgName;
  String info; serializeJson(doc, info);
  chrInfo->setValue(info.c_str());

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising as OZLOCK");
}

void buildTopics() {
  String base = "ozkey/" + cfgSiteId + "/locks/" + deviceId + "/";
  topicCommand = base + "command";
  topicEnroll = base + "enroll";
  topicHeartbeat = base + "heartbeat";
  topicLog = base + "log";
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch keypad
// ─────────────────────────────────────────────────────────────────────────────
void touchInit() {
  // Hardware power-reset of the touch processor (Touch.ino verified —
  // replaces the old 0x3B I2C wake sequence, which this batch ignores).
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(100);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  // Boot probe — err=0 means the CST816 ACKed at 0x63; anything else and
  // the keypad is dead hardware-side, not a firmware mapping issue.
  Wire.beginTransmission(TOUCH_ADDR);
  int err = Wire.endTransmission();
  Serial.printf("[TOUCH] probe 0x%02X err=%d %s\n", TOUCH_ADDR, err,
                err == 0 ? "(ACK ok)" : "(NO ACK — touch dead)");
}

static bool touchReadRegs(uint8_t *buf) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 7) < 7) return false;
  for (int i = 0; i < 7; i++) buf[i] = Wire.read();
  return true;
}

// Tap = decoded on RELEASE, using the LAST coordinates sampled during the
// press. Bench finding: this controller low-pass-filters the short axis —
// the X register converges toward the finger over ~100ms (early reads
// return a blend with the PREVIOUS touch), while Y refreshes fast. So we
// sample every loop while held (skipping the first, fully-stale one) and
// trust the final settled value.
int lastTapX = 0, lastTapY = 0;
uint8_t tapSamples = 0;

bool touchRead(int &tx, int &ty, bool &held) {
  uint8_t buf[7];
  if (!touchReadRegs(buf)) return false;
  uint8_t count = buf[2]; // CST816 map: active point count at reg 0x02
  bool down = (count > 0 && count <= 5);
  held = down;
  if (down) {
    if (touchWasDown) { // skip the first sample of a press (stale regs)
      int rawX = ((buf[3] & 0x0F) << 8) | buf[4];
      int rawY = ((buf[5] & 0x0F) << 8) | buf[6];
      lastTapX = 320 - rawY; // landscape transform for rotation 5
      // Vertical axis: empirically INVERTED + 1.2× scaled on this batch —
      // y = 180 − 1.2·rawX fit all 9 calibration taps (2026-07-17 bench,
      // rawX spans ~8..140 bottom→top).
      int y = 180 - (rawX * 6) / 5;
      if (y < 0) y = 0;
      if (y > 171) y = 171;
      lastTapY = y;
      if (tapSamples < 255) tapSamples++;
    }
    touchWasDown = true;
    return false;
  }
  if (!touchWasDown) return false;
  // release edge — emit the settled position
  touchWasDown = false;
  uint8_t n = tapSamples;
  tapSamples = 0;
  if (n == 0) return false; // too brief to get past the stale sample
  tx = lastTapX;
  ty = lastTapY;
  Serial.printf("[TOUCH] release %d,%d (%u samples)\n", tx, ty, n);
  return true;
}

char keyAt(int tx, int ty) {
  // Full-coverage grid: every pixel maps to the nearest key — no dead
  // zones, maximum tolerance for the coarse touch axis.
  int r = ty <= KP_Y ? 0 : (ty - KP_Y) / KP_ROW_H;
  if (r > 2) r = 2;
  if (r < 0) r = 0;
  int c = tx * 4 / 320;
  if (c > 3) c = 3;
  if (c < 0) c = 0;
  return KP_KEYS[r][c];
}

// THE one factory-reset method (operator: single method, no waiting):
// '*' pressed while the PIN entry is EMPTY arms it ("RESET? 5=Y"), '5'
// fires. '*' with digits typed just clears them (normal), so a guest
// clearing and retrying a PIN can never trip it. Works on every screen —
// the keypad touch zones are evaluated even where keys aren't drawn.
bool resetArm = false;
unsigned long lastKeyAt = 0; // 5s idle → clear half-typed entry (retry fresh)

void handleKey(char k) {
  if (lockoutUntil && millis() < lockoutUntil) return; // lockout active
  lastKeyAt = millis();
  if (resetArm) {
    resetArm = false;
    if (k == '5') { factoryReset(); return; }
    drawPinDots(); // wipe the strip prompt, back to normal entry
    return;
  }
  if (k == '*') {
    if (!pinEntry.length()) {
      resetArm = true;
      // small prompt in the top strip (operator: no full-screen flash)
      gfx->setTextSize(1);
      gfx->setTextColor(lockFg());
      gfx->setCursor(252, 2);
      gfx->print("RESET? 5");
      return;
    }
    pinEntry = "";
    drawPinDots();
    return;
  }
  if (k == '#') {
    if (!pinEntry.length()) return;
    int slot = checkPin(pinEntry);
    if (slot >= 0) {
      doUnlock((String("PIN slot ") + slot).c_str());
    } else {
      pinFails++;
      pinEntry = ""; // clear the stale entry immediately
      // UI first, network after — publishLog on a degraded link must never
      // delay the operator's feedback.
      drawFlash("WRONG PIN", C_RED, C_WHITE);
      publishLog("denied", "wrong PIN");
      delay(1200);
      if (pinFails >= 5) {
        lockoutUntil = millis() + 60000UL;
        publishLog("lockout", "5 wrong PINs — 60s");
      }
      screenDirty = true; // redraw keypad with empty PIN dots
    }
    return;
  }
  // Digit. Entry model is "<4 digits>#" (operator: 4-digit PINs for easy
  // testing): typing past the max is an invalid entry — clear and start
  // fresh with the new digit.
  if (pinEntry.length() >= 4) pinEntry = "";
  pinEntry += k;
  drawPinDots();
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** blelock v1 — OZLOCK doorlock emulator (ozkey-08 §10) ***");
  // Compile stamp — the definitive "is my new sketch on the board?" check.
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  gfx->begin();
  gfx->setRotation(5);
  gfx->fillScreen(C_BLACK);

  touchInit();

  WiFi.mode(WIFI_STA); // needed so the factory MAC is readable pre-join
  // Join-failure diagnostics: the disconnect reason tells apart the cases the
  // screen can't (201 NO_AP_FOUND = SSID invisible/5GHz-only; 15 4WAY
  // handshake timeout / 2 AUTH_EXPIRE = wrong password; 210 NO_AP_FOUND_W_
  // COMPATIBLE_SECURITY = WPA3-only AP).
  WiFi.onEvent(
      [](WiFiEvent_t e, WiFiEventInfo_t info) {
        Serial.printf("[WiFi] disconnected, reason=%d\n",
                      (int)info.wifi_sta_disconnected.reason);
      },
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  macStr = WiFi.macAddress();
  String machex = macStr; machex.replace(":", ""); machex.toLowerCase();
  deviceId = "ozk-" + machex;
  Serial.printf("[ID] device_id=%s mac=%s\n", deviceId.c_str(), macStr.c_str());

  loadConfig();
  buildTopics();

  if (provisioned) {
    // RECONNECT path: straight to network from NVS, no BLE (CONTRACT: stops
    // advertising once provisioned; factory reset re-opens)
    state = enrolled ? ST_OPERATIONAL : ST_JOINING;
    joinLine1 = "WiFi: joining " + cfgSsid + "...";
    joinLine2 = "Server: " + cfgBrokerHost + ":" + String(cfgBrokerPort);
    Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(),
                  cfgPass.length());
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    wifiJoinStart = millis();
  } else {
    state = ST_ADVERTISING;
    startBle();
  }
  screenDirty = true;
}

void loop() {
  // ── WiFi progress (JOINING ladder + NTP once up) ──────────────────────────
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  wl_status_t ws = WiFi.status();
  if (ws != lastWifi) {
    lastWifi = ws;
    Serial.printf("[WiFi] status=%d\n", (int)ws);
    if (ws == WL_CONNECTED) {
      configTime(0, 0, "pool.ntp.org"); // validity windows + log ts
      if (state == ST_JOINING) {
        notifyStatus("WIFI_OK");
        joinLine1 = "WiFi: OK - IP " + WiFi.localIP().toString();
        screenDirty = true;
      }
      Serial.printf("[WiFi] up, IP %s\n", WiFi.localIP().toString().c_str());
    }
  }
  if (state == ST_JOINING && ws != WL_CONNECTED && provisioned &&
      wifiJoinStart && millis() - wifiJoinStart > 25000) {
    wifiJoinStart = 0;
    notifyStatus("WIFI_FAIL");
    joinLine1 = "WiFi FAILED (wrong password?)";
    screenDirty = true;
    // stay re-provisionable: if BLE never started this boot, start it
    if (bleServer == nullptr) startBle();
  }

  // ── MQTT + enroll retry ───────────────────────────────────────────────────
  if (provisioned) ensureMqtt();
  if (state == ST_JOINING && mqtt.connected() && !enrolled &&
      lastEnrollSent && millis() - lastEnrollSent > 8000 && enrollAttempts < 5) {
    publishEnroll();
  }

  // ── heartbeat ─────────────────────────────────────────────────────────────
  if (mqtt.connected() && millis() - lastHeartbeat > cfgHeartbeatS * 1000UL) {
    lastHeartbeat = millis();
    publishHeartbeat();
  }

  // ── auto-relock ───────────────────────────────────────────────────────────
  if (lockStatus == "UNLOCKED" && millis() - unlockAt >= UNLOCK_MS) {
    lockStatus = "LOCKED";
    publishLog("relocked", "auto 5s");
    screenDirty = true;
  }

  // ── lockout expiry ────────────────────────────────────────────────────────
  if (lockoutUntil && millis() >= lockoutUntil) {
    lockoutUntil = 0;
    pinFails = 0;
    screenDirty = true;
  }

  // ── keypad idle timeout (operator): entry abandoned half-way — no key for
  // 5s — clears itself (and cancels a pending RESET? prompt) so the next
  // guest starts fresh.
  if (state == ST_OPERATIONAL && lastKeyAt && millis() - lastKeyAt > 5000) {
    lastKeyAt = 0;
    if (resetArm) {
      resetArm = false;
      screenDirty = true; // flash owns the screen — full redraw
    }
    if (pinEntry.length()) {
      pinEntry = "";
      drawPinDots();
    }
  }

  // ── touch (OPERATIONAL keypad + '#' long-press factory reset) ────────────
  {
    int tx, ty; bool held = false;
    bool newTouch = touchRead(tx, ty, held);
    // Live indicator: while the finger is down, show which key the lock
    // currently sees (top strip) — the reading converges over ~100ms, so
    // the operator can watch it settle before releasing.
    static char candShown = 0;
    if (state == ST_OPERATIONAL && !resetArm) {
      char cand = (held && tapSamples > 0) ? keyAt(lastTapX, lastTapY) : 0;
      if (cand != candShown) {
        candShown = cand;
        gfx->fillRect(220, 0, 24, KP_Y, lockBg());
        if (cand) {
          gfx->setTextSize(1);
          gfx->setTextColor(lockFg());
          gfx->setCursor(228, 2);
          gfx->print(cand);
        }
      }
    }
    if (newTouch) {
      char k = keyAt(tx, ty);
      Serial.printf("[TOUCH] %d,%d -> key '%c'\n", tx, ty, k ? k : '-');
      if (state == ST_OPERATIONAL) {
        if (k) {
          if (!resetArm) highlightKey(k); // white flash = what registered
          handleKey(k);
        }
      } else if (k) {
        // Same single reset method on every screen: '*' then '5' (the
        // keypad zones apply even where keys aren't drawn — hint printed
        // on the ADVERTISING/CONNECTING screens). Arms silently.
        if (resetArm) {
          resetArm = false;
          if (k == '5') factoryReset();
        } else if (k == '*') {
          resetArm = true;
          Serial.println("[RESET] armed — tap 5 to wipe");
        }
      }
    }
    if (hlKey && millis() > hlUntil) unhighlightKey();
  }

  // ── periodic monitor line (operator: attach serial anytime, see state) ────
  static unsigned long lastMon = 0;
  if (millis() - lastMon > 10000) {
    lastMon = millis();
    const char *st = state == ST_OPERATIONAL ? "OPERATIONAL"
                     : state == ST_JOINING   ? "JOINING"
                                             : "ADVERTISING";
    Serial.printf("[MON] %s wifi=%s ip=%s mqtt=%s lock=%s heap=%u\n", st,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.localIP().toString().c_str(),
                  mqtt.connected() ? "up" : "down", lockStatus.c_str(),
                  (unsigned)ESP.getFreeHeap());
  }

  // ── screen ────────────────────────────────────────────────────────────────
  if (screenDirty) {
    screenDirty = false;
    if (lockoutUntil && millis() < lockoutUntil) {
      drawFlash("LOCKED 60s", C_RED, C_WHITE);
    } else if (state == ST_ADVERTISING) drawAdvertising();
    else if (state == ST_JOINING) drawJoining();
    else drawOperational();
  }

  delay(15);
}
