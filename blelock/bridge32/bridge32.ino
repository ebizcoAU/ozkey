/*
 * bridge32 — OZBRIDGE Thread border router bootstrap v0 (2026-07-23)
 * Board  : ESP32-C6 (GEEK-class module — no LCD/touch assumed; status is
 *          serial log + BLE status characteristic only, matching the
 *          headless production plan in ozkey-08 §0.0.1)
 *
 * ROLE (ozkey-08 §0, Mode 2 residential): forms the Thread network the
 * doorlock (threadcomm) joins, and is the Wi-Fi/MQTT uplink for that mesh.
 * One bridge per home; provisioned once, then never touched per-lock.
 *
 * THIS INCREMENT proves BLE-provision -> Wi-Fi join -> Thread network
 * FORMED only. No MQTT, no Thread-side frame relay yet — see
 * blelock/CONTRACT-BRIDGE.md "Not in this increment".
 *
 * Commissioning (Option B, locked 2026-07-23): BANOI writes Wi-Fi creds over
 * BLE, exactly like the lock's existing provision flow. Once the Thread
 * network is formed, the operational dataset is exposed on `info` — BANOI
 * reads it from here and relays it (unmodified) into a threadcomm lock's own
 * provision characteristic. bridge32 never talks to a lock directly; the
 * phone is the courier for both.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OThread.h>
#include <openthread/instance.h> // otInstanceFactoryReset() — see factoryReset()
#include "esp_coexist.h"
#include <OThreadUDP.h>
#include <Arduino_GFX_Library.h>

// ── LCD (GEEK 1.14" ST7789, 135x240) — pins/offsets bench-confirmed in
// blelock/GeekDisplayTest/ (2026-07-24/25); bridge32 was headless-only
// before this. See ozkey-09 gap #9: this is a bench aid, not yet a
// decided production feature. ────────────────────────────────────────────
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_DC 3
#define LCD_RST 4
#define LCD_CS 5
#define LCD_BL 6
#define LCD_PANEL_W 135
#define LCD_PANEL_H 240
#define LCD_OFFSET_X 45
#define LCD_OFFSET_Y 48
Arduino_DataBus *lcdBus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(lcdBus, LCD_RST, 1 /* rotation: landscape 240x135 */, false /* IPS */,
                                       LCD_PANEL_W, LCD_PANEL_H, LCD_OFFSET_X, LCD_OFFSET_Y, LCD_OFFSET_X, LCD_OFFSET_Y);
String lastStatus = "BOOT";
#define LCD_C_BLACK 0x0000
#define LCD_C_WHITE 0xFFFF
#define LCD_C_GREEN 0x07E0
#define LCD_C_RED 0xF800

// LCD idle blank (2026-07-27): bench aid, not a status signal — the screen
// itself goes dark after LCD_IDLE_OFF_MS with nothing to check on it. Only a
// BOOT button press wakes it back up (status changes while it's off do NOT
// wake it — screen state is intentionally independent of the status ladder
// once it's gone dark, matching "only turn on if someone touch the boot
// button").
#define LCD_IDLE_OFF_MS 30000UL
bool lcdOn = true;
unsigned long lastLcdActivityAt = 0;

// ── GATT contract (blelock/CONTRACT-BRIDGE.md) ──────────────────────────────
#define BLE_NAME "OZBRIDGE"
#define SVC_UUID "4f5a4b32-0001-4272-6467-000000000001"
#define CHR_PROVISION "4f5a4b32-0002-4272-6467-000000000001"
#define CHR_STATUS "4f5a4b32-0003-4272-6467-000000000001"
#define CHR_INFO "4f5a4b32-0004-4272-6467-000000000001"
#define FW_VERSION "bridge32-1.0"
#define FW_DISPLAY_VERSION "v1.0" // shown on-screen, doorlock.ino's badge convention

// Thread network defaults — this bridge always FORMS (never joins an
// existing mesh) in v0; it is the only network former in the home.
#define OT_CHANNEL 15

// ── State machine ───────────────────────────────────────────────────────────
enum BridgeState { ST_ADVERTISING, ST_WIFI_JOINING, ST_THREAD_FORMING, ST_OPERATIONAL };
BridgeState state = ST_ADVERTISING;

Preferences prefs; // namespace "bridge32"
String cfgSsid, cfgPass;
String cfgMode, cfgBrokerHost, cfgSiteId; // F1: parsed from provision JSON
uint16_t cfgBrokerPort = 0;
bool wifiProvisioned = false;
String deviceId, macStr;
unsigned long wifiJoinStart = 0;
#define WIFI_JOIN_TIMEOUT_MS 20000UL

// Local hardware escape hatch (2026-07-26): hold BOOT for 5s to factory
// reset even with no app/BLE reachable at all — the orphan case (bridge
// still running its old config, nothing left to trigger a remote reset
// from) needs a way out that doesn't depend on radio range or app state.
#ifndef USER_BUTTON
#define USER_BUTTON BOOT_PIN
#endif
#define FACTORY_RESET_HOLD_MS 5000UL
unsigned long buttonHeldSince = 0;
bool buttonWasDown = false;

void checkFactoryResetButton() {
  bool down = digitalRead(USER_BUTTON) == LOW;
  if (down && !buttonWasDown) {
    buttonHeldSince = millis();
    lcdWake(); // any press wakes the screen — a 5s hold decides factory reset below
  } else if (down && buttonWasDown) {
    unsigned long held = millis() - buttonHeldSince;
    if (held >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[RESET] BOOT held 5s — factory reset");
      gfx->fillScreen(LCD_C_BLACK);
      gfx->setTextColor(LCD_C_WHITE);
      gfx->setTextSize(2);
      gfx->setCursor(4, 40);
      gfx->println("FACTORY RESET");
      factoryReset(); // wipes NVS + ESP.restart(); does not return
    } else if (held > 800 && (held / 500) % 2 == 0) {
      // cheap ~500ms-granularity progress blink, no extra timer needed
      Serial.printf("[RESET] holding BOOT... %lus/5s\n", held / 1000);
    }
  }
  buttonWasDown = down;
}

OpenThread thread;
DataSet otDataset;
bool threadFormed = false;

// F2: MQTT client — declared here (not down in the F2 section below) so
// refreshInfo()'s broker_connected check compiles; C++ needs the
// declaration before any use in the same translation unit, and Arduino's
// auto-prototype generation only covers functions, not globals (compile
// error caught 2026-07-26: "'mqttClient' was not declared in this scope").
WiFiClient wifiNetClient;
PubSubClient mqttClient(wifiNetClient);

// BLE
BLEServer *bleServer = nullptr;
BLECharacteristic *chrStatus = nullptr, *chrInfo = nullptr;
volatile bool bleClientConnected = false;
String provBuf;

// ─────────────────────────────────────────────────────────────────────────────
// NVS
// ─────────────────────────────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("bridge32", true);
  wifiProvisioned = prefs.getBool("prov", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgMode = prefs.getString("mode", "");
  cfgBrokerHost = prefs.getString("bhost", "");
  cfgBrokerPort = prefs.getUShort("bport", 0);
  cfgSiteId = prefs.getString("site", "");
  prefs.end();
}

void saveConfig() {
  prefs.begin("bridge32", false);
  prefs.putBool("prov", wifiProvisioned);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("mode", cfgMode);
  prefs.putString("bhost", cfgBrokerHost);
  prefs.putUShort("bport", cfgBrokerPort);
  prefs.putString("site", cfgSiteId);
  prefs.end();
}

void factoryReset() {
  Serial.println("[RESET] factory reset — wiping app config");
  prefs.begin("bridge32", false); prefs.clear(); prefs.end();
  // BUG FIX (2026-07-27, live bench): this only ever cleared the "bridge32"
  // Preferences namespace (WiFi creds/mode/broker) — the OpenThread stack
  // persists its own dataset in a separate NVS namespace it manages
  // internally, so every "factory reset" today still resumed the old
  // (possibly stale, from a run where begin() was silently broken) Thread
  // dataset instead of forming fresh. otInstanceFactoryReset() is the
  // correct call — it erases OpenThread's own persistent info and triggers
  // its own platform reset (does not return on success).
  if (thread) {
    Serial.println("[RESET] erasing OpenThread persistent info");
    otInstanceFactoryReset(thread.getInstance());
  }
  ESP.restart(); // fallback in case otInstanceFactoryReset() ever returns
}

// ─────────────────────────────────────────────────────────────────────────────
// LCD status (bench aid — see the header comment near the pin defines)
// ─────────────────────────────────────────────────────────────────────────────
void drawStatus() {
  // Two-state screen (2026-07-27), styled after doorlock.ino's bordered
  // bench screens (drawAdvertising/drawJoining: accent border + header, big
  // centered title, dim footer, hint line) — scaled down to this panel's
  // 240x135. Success is deliberately the plainer of the two: no border, no
  // ladder text, just a clean white "nothing to check on here" screen —
  // the opposite of the busy red transition screen.
  bool allUp = (WiFi.status() == WL_CONNECTED) && threadFormed;

  if (allUp) {
    gfx->fillScreen(LCD_C_WHITE);
    gfx->setTextWrap(false);
    gfx->setTextColor(LCD_C_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(170, 10);
    gfx->println(FW_DISPLAY_VERSION);
    gfx->setTextSize(3); // reverted from 3 (2026-07-27) — too big
    gfx->setCursor(4, 12);
    gfx->println("OZBRIDGE");
    gfx->setTextSize(2);
    gfx->setCursor(4, 48);
    gfx->println("WIFI:   OK");
    gfx->setCursor(4, 66);
    gfx->println("THREAD: OK");
    gfx->setCursor(4, 84);
    gfx->print("IP ");
    gfx->println(WiFi.localIP().toString());
    gfx->setCursor(4, 102);
    if (cfgSiteId.length()) {
      gfx->print("site ");
      gfx->println(cfgSiteId);
    }
    gfx->setTextSize(1);
    gfx->setCursor(4, 122); // tightened from 106 (2026-07-27)
    gfx->println(deviceId);
    return;
  }

  // Reworked (2026-07-27, user pass): dropped the separate header line and
  // the factory-reset hint (screen real estate) and the device_id line ("no
  // use") — version now lives on the title line instead of its own row.
  gfx->fillScreen(LCD_C_BLACK);
  gfx->drawRect(0, 0, 240, 135, LCD_C_RED);
  gfx->setTextWrap(false);
  gfx->setTextColor(LCD_C_RED);
  gfx->setTextSize(2); // bumped from 1 — was unreadable
  gfx->setCursor(6, 10);
  gfx->println(lastStatus); // no "STATUS: " label — longest value (17 chars)
                             // already fills the width at size2 with it
  gfx->setTextColor(LCD_C_WHITE);
  gfx->setCursor(6, 44);
  gfx->print("OZBRIDGE ");
  gfx->println(FW_DISPLAY_VERSION);
  gfx->setTextSize(2);
  gfx->setTextColor(LCD_C_RED);
  gfx->setCursor(6, 90);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->print("IP ");
    gfx->println(WiFi.localIP().toString());
  }
  gfx->setCursor(6, 112);
  if (cfgSiteId.length()) {
    gfx->print("site ");
    gfx->println(cfgSiteId);
  } else if (cfgMode.length()) {
    gfx->print("mode ");
    gfx->println(cfgMode);
  }
}

// Wakes the backlight + redraws immediately (button press, or right after
// boot). A no-op backlight-write if it's already on.
void lcdWake() {
  digitalWrite(LCD_BL, HIGH);
  lcdOn = true;
  lastLcdActivityAt = millis();
  drawStatus();
}

// ─────────────────────────────────────────────────────────────────────────────
// Status ladder (notify BANOI over BLE + serial log + LCD)
// ─────────────────────────────────────────────────────────────────────────────
void notifyStatus(const char *wire) {
  Serial.printf("[STATUS] %s\n", wire);
  lastStatus = wire;
  // Wakes the screen (2026-07-27, revised) — every status ladder step is
  // either the app talking to it (BLE_OK) or a real WiFi/Thread/broker state
  // change, exactly the kind of activity the user wants visible on the
  // panel rather than dark. See mqttMessageReceived() for the other
  // wake trigger (routine data-plane traffic with no ladder step of its own).
  lcdWake();
  if (chrStatus != nullptr) {
    chrStatus->setValue((uint8_t *)wire, strlen(wire));
    if (bleClientConnected) {
      chrStatus->notify();
      // App-observed bug, 2026-07-26: a fast-resolving ladder (WIFI_OK ->
      // THREAD_OK -> BROKER_OK completed in ~325ms on the bench) can fire
      // notifies faster than the phone's BLE stack transmits them, so a
      // later one silently overwrites an earlier one before it's sent —
      // BANOI's commissioning sheet missed BROKER_OK entirely and showed
      // "failed to join" even though the bridge had already succeeded.
      // Give each notify room to actually land before the next one fires.
      delay(150);
    }
  }
}

// hex helpers — dataset fields are exposed to the app as lowercase hex
String bytesToHex(const uint8_t *b, size_t n) {
  String out; out.reserve(n * 2);
  char buf[3];
  for (size_t i = 0; i < n; i++) { snprintf(buf, sizeof(buf), "%02x", b[i]); out += buf; }
  return out;
}

bool hexToBytes(const String &hex, uint8_t *out, size_t expectLen) {
  if ((size_t)hex.length() != expectLen * 2) return false;
  for (size_t i = 0; i < expectLen; i++) {
    char hi = hex[i * 2], lo = hex[i * 2 + 1];
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nib(hi), l = nib(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// info characteristic — refreshed whenever Thread state changes
// ─────────────────────────────────────────────────────────────────────────────
void refreshInfo() {
  if (!chrInfo) return;
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  doc["transport"] = "bridge";
  doc["mode"] = cfgMode; // F1: stored personality ("mqtt-uplink" | "matter-bridge")
  doc["thread_role"] = thread ? OpenThread::otGetStringDeviceRole() : "disabled";

  // Per-stage breakdown (live, read on demand — 2026-07-26): the app was
  // relying solely on the fast BLE notify ladder to know progress, and a
  // missed notify left it thinking the bridge failed even when it hadn't.
  // These booleans are live checks, not cached — reading `info` at any time
  // (a plain GATT read, no notify timing involved) tells the app exactly
  // which stage is actually done right now.
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["thread_formed"] = threadFormed;
  doc["broker_connected"] = mqttClient.connected();
  doc["last_status"] = lastStatus;

  if (threadFormed) {
    // CRASH FIX (2026-07-26, live bench): thread.getExtendedPanId()/getNetworkKey()
    // return nullptr if OpenThread's internal lock isn't ready yet (esp_openthread_
    // lock_acquire can fail right after thread.start(), before the OT task has fully
    // spun up) — bytesToHex() then dereferenced that null pointer and crashed
    // (Guru Meditation, Load access fault) on the very first real Wi-Fi-join ->
    // Thread-form run. DataSet's own getters below are plain struct-field reads,
    // no lock involved, never null — worst case briefly-empty fields, never a crash.
    const DataSet &ds = thread.getCurrentDataSet();
    doc["network_name"] = ds.getNetworkName();
    doc["ext_pan_id"] = bytesToHex(ds.getExtendedPanId(), 8);
    doc["network_key"] = bytesToHex(ds.getNetworkKey(), 16);
    doc["channel"] = ds.getChannel();
    char panHex[5]; snprintf(panHex, sizeof(panHex), "%04x", ds.getPanId());
    doc["pan_id"] = panHex;
  }
  String out; serializeJson(doc, out);
  chrInfo->setValue(out.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// F2: MQTT uplink (mode=mqtt-uplink only). Topic per the PM-adopted
// server-side-bridge-aggregation routing design (2026-07-25): ozlockserv
// resolves a Thread lock's bridge_id and publishes to this bridge's own
// topic instead of the lock's device-scoped one; the payload carries the
// target lock's device_id so this bridge can demux over Thread (F4).
// S4 (ozlockserv schema/routing) is a separate, server-side task — this
// bridge already subscribes to the correct final topic, so no further
// firmware change is needed once S4 ships.
// ─────────────────────────────────────────────────────────────────────────────
String mqttCommandTopic;
unsigned long mqttLastAttempt = 0;
#define MQTT_RETRY_MS 5000UL

void mqttMessageReceived(char *topic, byte *payload, unsigned int len) {
  // Incoming WiFi/MQTT traffic — no ladder status step of its own, so wake
  // the screen explicitly here (2026-07-27).
  lcdWake();
  String body;
  body.reserve(len);
  for (unsigned int i = 0; i < len; i++) body += (char)payload[i];
  Serial.printf("[MQTT] << %s : %s\n", topic, body.c_str());

  // F4: distill to {target, payload} for the Thread hop. Accepts either the
  // lean v0 shape (a bench `mosquitto_pub` test) or the richer ozlockserv
  // queue envelope (device_id/payload_hex) — S4 pins the exact server-side
  // shape; both are handled defensively until then.
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    Serial.println("[MQTT] payload not valid JSON, dropped");
    return;
  }
  String target = (const char *)(doc["target"] | "");
  if (!target.length()) target = (const char *)(doc["device_id"] | "");
  String payloadHex = (const char *)(doc["payload"] | "");
  if (!payloadHex.length()) payloadHex = (const char *)(doc["payload_hex"] | "");
  forwardOverThread(target, payloadHex);
}

// ─────────────────────────────────────────────────────────────────────────────
// F4: Thread-side frame transport, bridge -> lock. Neither device discovers
// the other over the air (CONTRACT-BRIDGE.md) and the bridge has no
// point-to-point route to a specific lock's Mesh-Local address yet, so v0
// uses a realm-local multicast group: every threadcomm lock on the mesh
// listens and filters by its own device_id in "target". Group/port MUST
// match threadcomm.ino exactly. Port avoids CoAP (5683/5684) and Thread
// TMF CoAP (61631); last two group bytes are the "OZ" motif.
// ─────────────────────────────────────────────────────────────────────────────
const uint8_t OZ_THREAD_GROUP_BYTES[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x4f, 0x5a};
const IPAddress OZ_THREAD_GROUP(IPv6, OZ_THREAD_GROUP_BYTES);
const uint16_t OZ_THREAD_UDP_PORT = 5052;

OThreadUDP threadUdp;
bool threadUdpReady = false;
unsigned long threadUdpLastAttempt = 0;
#define THREAD_UDP_RETRY_MS 3000UL

// threadUdpBegin() can fail right after THREAD_OK — same family as the
// bytesToHex crash: OpenThread's internal lock isn't always ready the
// instant Thread forms (live-bench finding, 2026-07-26: "[UDP] socket
// FAILED" reproduced on a fresh boot's first formation). It fails
// gracefully here (unlike the null-deref case), so the fix is a retry, not
// a null-check — called once at THREAD_OK and then re-polled from loop()
// until it succeeds.
void threadUdpBegin() {
  if (threadUdpReady) return;
  threadUdpLastAttempt = millis();
  threadUdpReady = threadUdp.begin(OZ_THREAD_UDP_PORT) != 0; // sender: plain unicast bind
  Serial.printf("[UDP] socket %s on port %u\n", threadUdpReady ? "open" : "FAILED",
                OZ_THREAD_UDP_PORT);
}

void forwardOverThread(const String &target, const String &payloadHex) {
  if (!target.length() || !payloadHex.length()) {
    Serial.println("[UDP] drop — command missing target/payload");
    return;
  }
  if (!threadUdpReady) {
    Serial.println("[UDP] drop — socket not open");
    return;
  }
  JsonDocument doc;
  doc["target"] = target;
  doc["payload"] = payloadHex;
  String out;
  serializeJson(doc, out);

  if (!threadUdp.beginPacket(OZ_THREAD_GROUP, OZ_THREAD_UDP_PORT)) {
    Serial.println("[UDP] beginPacket failed");
    return;
  }
  threadUdp.write((const uint8_t *)out.c_str(), out.length());
  if (!threadUdp.endPacket()) {
    Serial.println("[UDP] endPacket failed");
    return;
  }
  Serial.printf("[UDP] >> %s\n", out.c_str());
}

void mqttConnect() {
  if (mqttClient.connected()) return;
  Serial.printf("[MQTT] connecting to %s:%u as %s\n", cfgBrokerHost.c_str(), cfgBrokerPort,
                deviceId.c_str());
  notifyStatus("BROKER_JOINING");
  if (mqttClient.connect(deviceId.c_str())) {
    Serial.println("[MQTT] connected");
    notifyStatus("BROKER_OK");
    mqttClient.subscribe(mqttCommandTopic.c_str());
    Serial.printf("[MQTT] subscribed %s\n", mqttCommandTopic.c_str());
  } else {
    Serial.printf("[MQTT] connect failed, rc=%d\n", mqttClient.state());
    notifyStatus("BROKER_FAIL");
  }
  mqttLastAttempt = millis();
}

void mqttBegin() {
  mqttCommandTopic = "ozkey/" + cfgSiteId + "/bridges/" + deviceId + "/command";
  mqttClient.setServer(cfgBrokerHost.c_str(), cfgBrokerPort);
  mqttClient.setCallback(mqttMessageReceived);
  mqttConnect();
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread network formation — bridge32 is always the former in v0 (single
// bridge per home; no "join an existing mesh" path yet).
// ─────────────────────────────────────────────────────────────────────────────
void formThreadNetwork() {
  state = ST_THREAD_FORMING;
  notifyStatus("THREAD_FORMING");

  // COEX SETTLE (2026-07-27, live bench): this runs the instant WL_CONNECTED
  // is observed — zero gap before the 802.15.4 radio init competes with WiFi
  // for the same shared 2.4GHz RF frontend on the C6. Same radio-coexistence
  // stress already implicated in a BLE reset loop elsewhere in this file
  // (see the WIFI_JOIN_TIMEOUT_MS comment above) produced this symptom:
  // esp_openthread_lock_acquire "mutex is not ready" + platform_deinit "not
  // initialized", then otGetDeviceRole() stuck Disabled — OpenThread's
  // platform init was racing WiFi's own driver/event-loop settling.
  delay(500);

  OpenThread::begin(false); // false = don't auto-start; we commit a dataset first
  // DIAGNOSTIC (2026-07-27): begin()'s own worker task can fail its stack
  // init fast (radio/netif bring-up) and silently return with the platform
  // never actually up — the code below never checked this before charging
  // into hasActiveDataset()/commitDataSet() regardless. If this prints 0,
  // the failure is inside begin() itself, not in the dataset calls that
  // follow (which would just be downstream symptoms of an instance that was
  // never really initialized).
  Serial.printf("[THREAD] begin() otStarted=%d\n", (int)(bool)thread);
  if (thread.hasActiveDataset()) {
    Serial.println("[THREAD] resuming persisted dataset");
  } else {
    Serial.println("[THREAD] no persisted dataset — forming a new network");
    otDataset.initNew();
    String name = "OZ-" + macStr.substring(9); // last 2 octets, colon-stripped below
    name.replace(":", "");
    otDataset.setNetworkName(name.c_str());

    uint8_t extPanId[8], networkKey[16];
    for (int i = 0; i < 8; i++) extPanId[i] = (uint8_t)esp_random();
    for (int i = 0; i < 16; i++) networkKey[i] = (uint8_t)esp_random();
    otDataset.setExtendedPanId(extPanId);
    otDataset.setNetworkKey(networkKey);
    otDataset.setChannel(OT_CHANNEL);
    uint16_t panId = (uint16_t)(esp_random() & 0x3FFF) + 1; // avoid 0x0000/0xFFFF
    otDataset.setPanId(panId);

    thread.commitDataSet(otDataset);
    Serial.printf("[THREAD] formed: name=%s channel=%u panId=0x%04x\n",
                  name.c_str(), OT_CHANNEL, panId);
  }

  // ORDER FIX (2026-07-27, live bench): otThreadSetEnabled()'s own header doc
  // (openthread/thread.h) documents OT_ERROR_INVALID_STATE = "the network
  // interface was not up" — it requires otIp6SetEnabled(true) (networkInterfaceUp())
  // to run FIRST. This was calling thread.start() (-> otThreadSetEnabled)
  // before networkInterfaceUp() (-> otIp6SetEnabled), guaranteeing that call
  // fails every time — role stuck Disabled forever, no matter how clean the
  // dataset or how correctly the platform initialized (both already verified
  // fine this session). Its log_e() never surfaced because Core Debug Level
  // was filtering it, which is why this took so long to pin down.
  thread.networkInterfaceUp();
  thread.start();

  // RACE FIX (2026-07-27, live bench): getCurrentDataSet() can return
  // zeroed fields for a short window right after start()/networkInterfaceUp()
  // — the OpenThread task hasn't attached yet, so refreshInfo() below would
  // publish an all-zero dataset on `info`. The app caches whatever it reads
  // there (never re-syncs), so it silently poisoned a doorlock provision with
  // ext_pan_id/network_key/channel/pan_id all zero (doorlock correctly
  // rejected ch=0 as malformed). Wait for actual attachment first.
  //
  // TIMEOUT BUMP (2026-07-27): with the start()/networkInterfaceUp() call
  // order now fixed, the device correctly reaches Detached (not stuck
  // Disabled) but 5s wasn't long enough for OpenThread's own MLE attach/
  // parent-search state machine to give up and form as Leader — observed
  // still Detached at the 5000ms mark on live hardware. 20s comfortably
  // covers that on a single-node network with nothing to attach to.
  unsigned long attachStart = millis();
  ot_device_role_t role;
  do {
    delay(50);
    role = OpenThread::otGetDeviceRole();
  } while (role != OT_ROLE_LEADER && role != OT_ROLE_ROUTER && role != OT_ROLE_CHILD &&
           millis() - attachStart < 20000);
  Serial.printf("[THREAD] attach role=%s after %lums\n",
                OpenThread::otGetStringDeviceRole(), millis() - attachStart);

  if (role != OT_ROLE_LEADER && role != OT_ROLE_ROUTER && role != OT_ROLE_CHILD) {
    // Fail closed (CONTRACT-BRIDGE.md's documented THREAD_FAIL, never actually
    // sent before this fix) — declaring THREAD_OK here would let refreshInfo()
    // publish whatever getCurrentDataSet() has (unattached, likely garbage)
    // for the app to cache and hand to a doorlock.
    Serial.println("[THREAD] attach failed — not publishing dataset");
    notifyStatus("THREAD_FAIL");
    return;
  }

  threadFormed = true;
  state = ST_OPERATIONAL;
  notifyStatus("THREAD_OK");
  refreshInfo();
  threadUdpBegin(); // F4 — open regardless of personality; only mqtt-uplink sends on it for now

  // Personality dispatch — Thread network formation is common to both;
  // only the post-THREAD_OK behavior differs (blelock/CONTRACT-BRIDGE.md).
  if (cfgMode == "matter-bridge") {
    // F5: Personality A stub — no functional Matter-over-Wi-Fi stack yet.
    Serial.println("[MODE] matter-bridge — Matter bridge mode not yet implemented");
  } else if (cfgMode == "mqtt-uplink") {
    mqttBegin(); // F2
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Provisioning (BLE write -> Wi-Fi join -> Thread form)
// ─────────────────────────────────────────────────────────────────────────────
// F1: mode is the personality selector (blelock/CONTRACT-BRIDGE.md
// "Provision payload"). Validated before anything else is applied, so a
// bad payload never touches the Wi-Fi/NVS state left by a prior good one.
bool validModePayload(JsonDocument &doc, String &modeOut, String &hostOut,
                       uint16_t &portOut, String &siteOut) {
  modeOut = (const char *)(doc["mode"] | "");
  if (modeOut != "mqtt-uplink" && modeOut != "matter-bridge") return false;

  hostOut = (const char *)(doc["broker_host"] | "");
  portOut = (uint16_t)(doc["broker_tcp_port"] | 0);
  siteOut = (const char *)(doc["site_id"] | "");
  if (modeOut == "mqtt-uplink" &&
      (!hostOut.length() || portOut == 0 || !siteOut.length())) {
    return false; // broker_host/broker_tcp_port/site_id are REQUIRED for mqtt-uplink
  }
  return true;
}

void applyProvision(JsonDocument &doc) {
  // Reset sentinel (2026-07-26): reuses this same write characteristic —
  // same pattern threadcomm uses to tell a Thread dataset from Wi-Fi creds
  // by field presence, just a dedicated key here since "reset" can't be
  // confused with any real provision field. Needed so removing a bridge in
  // BANOI can actually tell the physical device to let go, instead of
  // leaving a fully-configured orphan still running with stale credentials
  // (bridge32.ino had a factoryReset() with no caller anywhere before this).
  if (doc["reset"] | false) {
    String pid = doc["device_id"] | "";
    if (pid.length() && pid != deviceId) {
      Serial.printf("[PROV] reset requested for wrong device_id (%s != %s), ignoring\n",
                    pid.c_str(), deviceId.c_str());
      return;
    }
    Serial.println("[PROV] factory reset requested over BLE");
    factoryReset(); // wipes NVS + ESP.restart(); does not return
    return;
  }

  String pid = doc["device_id"] | "";
  if (pid.length() && pid != deviceId) {
    Serial.printf("[PROV] device_id mismatch (%s != %s)\n", pid.c_str(), deviceId.c_str());
    notifyStatus("WIFI_FAIL");
    return;
  }

  String mode, brokerHost, siteId;
  uint16_t brokerPort;
  if (!validModePayload(doc, mode, brokerHost, brokerPort, siteId)) {
    Serial.printf("[PROV] rejected — mode missing/invalid or broker fields incomplete "
                  "(mode='%s')\n", mode.c_str());
    notifyStatus("PAYLOAD_REJECTED");
    return;
  }

  cfgSsid = (const char *)(doc["ssid"] | "");
  cfgPass = (const char *)(doc["password"] | "");
  if (!cfgSsid.length()) { notifyStatus("WIFI_FAIL"); return; }

  cfgMode = mode;
  cfgBrokerHost = brokerHost;
  cfgBrokerPort = brokerPort;
  cfgSiteId = siteId;
  wifiProvisioned = true;
  saveConfig();

  Serial.printf("[PROV] mode=%s site=%s broker=%s:%u\n", cfgMode.c_str(), cfgSiteId.c_str(),
                cfgBrokerHost.c_str(), cfgBrokerPort);

  // RACE FIX (2026-07-26, live bench, round 2): wifiJoinStart must be fresh
  // BEFORE state flips to ST_WIFI_JOINING — loop() (main task) can be
  // scheduled between any two statements here (BLE callback task) and would
  // otherwise see the new state with the OLD wifiJoinStart (from a join
  // minutes ago), making millis()-wifiJoinStart instantly exceed the
  // timeout and fire a false "[WiFi] join timeout" before WiFi.begin() even
  // ran. Set wifiJoinStart first so it's already fresh the moment state
  // becomes observable.
  wifiJoinStart = millis();
  state = ST_WIFI_JOINING;
  notifyStatus("WIFI_JOINING");
  // Tell the broker we're leaving BEFORE yanking Wi-Fi, so a re-provision
  // doesn't leave a zombie session for the broker to reap on its own
  // keepalive timeout (live-bench finding, 2026-07-26: showed up in the
  // Mosquitto log as "disconnected: exceeded timeout" well after the fact).
  if (mqttClient.connected()) mqttClient.disconnect();
  WiFi.disconnect(true); // clean slate — a re-provision may land mid a prior attempt
  Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(), cfgPass.length());
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
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

// Regenerates `info` at read-time so it always reflects live Wi-Fi/Thread/
// broker state, not whatever refreshInfo() last happened to push (2026-07-26
// app-sync fix). A plain GATT read has no notify-timing risk.
class InfoCB : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *) override { refreshInfo(); }
};

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleClientConnected = true;
    notifyStatus("BLE_OK");
  }
  void onDisconnect(BLEServer *) override {
    bleClientConnected = false;
    // BUG FIX (2026-07-26, live bench): this used to only restart
    // advertising while state==ST_ADVERTISING. Once provisioned/operational,
    // ANY client connecting and disconnecting (a status check, our own
    // reset-scan code, a second BANOI session) permanently stopped
    // advertising until the next reboot — "keep BLE up even post-provision"
    // (this file's own header comment) requires restarting unconditionally.
    delay(300);
    BLEDevice::startAdvertising();
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
  chrInfo->setCallbacks(new InfoCB());
  refreshInfo();

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising as OZBRIDGE");
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** bridge32 v0 — OZBRIDGE Thread border router bootstrap ***");
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

  // EVENT-LOOP RACE FIX (2026-07-27, live bench): OThread.cpp's worker task
  // calls esp_event_loop_create_default() with a strict `!= ESP_OK` check —
  // it does NOT tolerate ESP_ERR_INVALID_STATE ("already created"), unlike
  // arduino-esp32's own Network/WiFi stack (NetworkEvents.cpp), which
  // explicitly accepts that as fine. Since WiFi always comes up first in this
  // sketch and claims the default event loop itself, OpenThread's own
  // creation call was guaranteed to fail every time formThreadNetwork() ran
  // later — this is what "esp_openthread_lock_acquire: mutex is not ready" /
  // otGetDeviceRole() stuck Disabled actually traced back to (confirmed by
  // reading OThread.cpp + NetworkEvents.cpp directly, not a coex/timing
  // issue — the earlier settle-delay and esp_coex_wifi_i154_enable() fixes
  // were both no-ops against this). Claiming the event loop here, before
  // WiFi ever touches it, flips who's "first" — WiFi's own creation call
  // afterward already handles ESP_ERR_INVALID_STATE gracefully.
  // OpenThread::begin() is idempotent (early-returns if already started), so
  // the existing call later in formThreadNetwork() stays a safe no-op.
  OpenThread::begin(false);
  Serial.printf("[THREAD] early begin() otStarted=%d\n", (int)(bool)thread);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  gfx->begin();
  // PANEL FIX (2026-07-27, live bench): Arduino_ST7789::begin() defaults to
  // invertDisplay(false), but this panel needs inversion ON — without this,
  // every color comes out as its complement (black bg shows white, white
  // title shows black, red shows cyan-ish/yellow depending on which "red"
  // value was tried). Confirmed by the math matching exactly: BLACK
  // (0,0,0) complements to WHITE, standard-RGB565 RED (0xF800) complements
  // to CYAN ("light blue"), then BLUE (0x001F, my wrong first attempt at a
  // fix) complements to YELLOW — precisely what was reported. This is the
  // actual fix; the LCD_C_RED value swap that preceded it was chasing the
  // wrong cause and has been reverted.
  gfx->invertDisplay(true);
  drawStatus(); // deviceId isn't set yet — redrawn again once it is, below

  pinMode(USER_BUTTON, INPUT_PULLUP); // hold 5s -> factory reset, any state

  WiFi.mode(WIFI_STA);

  // COEX FIX (2026-07-27, live bench): bridge32 is the one sketch running
  // WiFi and 802.15.4 (Thread) concurrently on the C6's single shared
  // 2.4GHz radio, and nothing was ever telling the coexistence arbiter both
  // radios would be in play. Without this, OpenThread::begin()'s platform
  // init silently failed to stand up its own task/lock later in
  // formThreadNetwork() — "esp_openthread_lock_acquire: mutex is not ready",
  // otGetDeviceRole() stuck Disabled — and a fixed settle delay there did
  // NOT fix it (same failure, just shifted later by the delay amount),
  // which is what ruled out a plain timing race and pointed here instead.
  esp_err_t coexErr = esp_coex_wifi_i154_enable();
  Serial.printf("[COEX] wifi_i154_enable -> %d\n", (int)coexErr);

  WiFi.onEvent(
      [](WiFiEvent_t e, WiFiEventInfo_t info) {
        Serial.printf("[WiFi] disconnected, reason=%d\n",
                      (int)info.wifi_sta_disconnected.reason);
      },
      WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  macStr = WiFi.macAddress();
  String machex = macStr; machex.replace(":", ""); machex.toLowerCase();
  deviceId = "ozb-" + machex;
  Serial.printf("[ID] device_id=%s mac=%s\n", deviceId.c_str(), macStr.c_str());
  lastLcdActivityAt = millis(); // idle-off clock starts now, not at cold boot
  drawStatus();

  loadConfig();

  if (wifiProvisioned) {
    state = ST_WIFI_JOINING;
    Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(), cfgPass.length());
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    wifiJoinStart = millis();
    startBle(); // keep BLE up even post-provision — info/status stay readable
  } else {
    state = ST_ADVERTISING;
    startBle();
  }
}

void loop() {
  checkFactoryResetButton();

  // Idle auto-off only once both radios are actually up (2026-07-27) — stay
  // lit through the whole pairing/forming ladder no matter how long it
  // takes, so nothing gets missed mid-bring-up; only blank once there's
  // genuinely nothing left to watch.
  bool bothUp = (WiFi.status() == WL_CONNECTED) && threadFormed;
  if (bothUp && lcdOn && millis() - lastLcdActivityAt > LCD_IDLE_OFF_MS) {
    digitalWrite(LCD_BL, LOW);
    lcdOn = false;
  }

  static bool wasWifiConnected = false;
  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (state == ST_WIFI_JOINING) {
    // RACE FIX (2026-07-26, live bench): applyProvision() runs on the BLE
    // callback task and this loop() runs on the main task, with no
    // synchronization between them. A re-provision (already-connected bridge
    // gets a fresh SSID/password over BLE) could have this check see
    // WiFi.status()==WL_CONNECTED left over from the PRIOR join, racing ahead
    // of the BLE task's own new WiFi.begin() call — state left ST_WIFI_JOINING
    // before the real new join outcome was known, so the join's actual
    // success/failure was silently dropped ("update never took"). Requiring
    // the SSID match closes the window regardless of exact task scheduling.
    if (wifiConnected && WiFi.SSID() == cfgSsid) {
      Serial.printf("[WiFi] joined, IP=%s\n", WiFi.localIP().toString().c_str());
      notifyStatus("WIFI_OK");
      formThreadNetwork(); // -> ST_OPERATIONAL on success
    } else if (millis() - wifiJoinStart > WIFI_JOIN_TIMEOUT_MS) {
      Serial.println("[WiFi] join timeout");
      WiFi.disconnect(true); // stop the driver's own retry loop — a bad SSID/password
                              // must not keep hammering the radio in the background
                              // (coex-stresses BLE, was implicated in a reset loop)
      notifyStatus("WIFI_FAIL");
      state = ST_ADVERTISING; // stay connectable, accept a re-write (never one-shot)
    }
  }

  if (state == ST_OPERATIONAL && wasWifiConnected && !wifiConnected) {
    Serial.println("[WiFi] link dropped — Thread mesh keeps running independently");
  }
  wasWifiConnected = wifiConnected;

  if (state == ST_OPERATIONAL && cfgMode == "mqtt-uplink" && wifiConnected) {
    if (mqttClient.connected()) {
      mqttClient.loop();
    } else if (millis() - mqttLastAttempt > MQTT_RETRY_MS) {
      mqttConnect();
    }
  }

  if (state == ST_OPERATIONAL && !threadUdpReady &&
      millis() - threadUdpLastAttempt > THREAD_UDP_RETRY_MS) {
    threadUdpBegin();
  }

  delay(50);
}
