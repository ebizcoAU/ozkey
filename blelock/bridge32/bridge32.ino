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
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OThread.h>

// ── GATT contract (blelock/CONTRACT-BRIDGE.md) ──────────────────────────────
#define BLE_NAME "OZBRIDGE"
#define SVC_UUID "4f5a4b32-0001-4272-6467-000000000001"
#define CHR_PROVISION "4f5a4b32-0002-4272-6467-000000000001"
#define CHR_STATUS "4f5a4b32-0003-4272-6467-000000000001"
#define CHR_INFO "4f5a4b32-0004-4272-6467-000000000001"
#define FW_VERSION "bridge32-0.1"

// Thread network defaults — this bridge always FORMS (never joins an
// existing mesh) in v0; it is the only network former in the home.
#define OT_CHANNEL 15

// ── State machine ───────────────────────────────────────────────────────────
enum BridgeState { ST_ADVERTISING, ST_WIFI_JOINING, ST_THREAD_FORMING, ST_OPERATIONAL };
BridgeState state = ST_ADVERTISING;

Preferences prefs; // namespace "bridge32"
String cfgSsid, cfgPass;
bool wifiProvisioned = false;
String deviceId, macStr;
unsigned long wifiJoinStart = 0;
#define WIFI_JOIN_TIMEOUT_MS 20000UL

OpenThread thread;
DataSet otDataset;
bool threadFormed = false;

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
  prefs.end();
}

void saveConfig() {
  prefs.begin("bridge32", false);
  prefs.putBool("prov", wifiProvisioned);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.end();
}

void factoryReset() {
  Serial.println("[RESET] factory reset — wiping NVS");
  prefs.begin("bridge32", false); prefs.clear(); prefs.end();
  ESP.restart();
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
  doc["thread_role"] = thread ? OpenThread::otGetStringDeviceRole() : "disabled";
  if (threadFormed) {
    const DataSet &ds = thread.getCurrentDataSet();
    (void)ds; // fields pulled via the typed getters below, not the raw struct
    doc["network_name"] = thread.getNetworkName();
    doc["ext_pan_id"] = bytesToHex(thread.getExtendedPanId(), 8);
    doc["network_key"] = bytesToHex(thread.getNetworkKey(), 16);
    doc["channel"] = thread.getChannel();
    char panHex[5]; snprintf(panHex, sizeof(panHex), "%04x", thread.getPanId());
    doc["pan_id"] = panHex;
  }
  String out; serializeJson(doc, out);
  chrInfo->setValue(out.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread network formation — bridge32 is always the former in v0 (single
// bridge per home; no "join an existing mesh" path yet).
// ─────────────────────────────────────────────────────────────────────────────
void formThreadNetwork() {
  state = ST_THREAD_FORMING;
  notifyStatus("THREAD_FORMING");

  OpenThread::begin(false); // false = don't auto-start; we commit a dataset first
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

  thread.start();
  thread.networkInterfaceUp();
  threadFormed = true;
  state = ST_OPERATIONAL;
  notifyStatus("THREAD_OK");
  refreshInfo();
}

// ─────────────────────────────────────────────────────────────────────────────
// Provisioning (BLE write -> Wi-Fi join -> Thread form)
// ─────────────────────────────────────────────────────────────────────────────
void applyProvision(JsonDocument &doc) {
  String pid = doc["device_id"] | "";
  if (pid.length() && pid != deviceId) {
    Serial.printf("[PROV] device_id mismatch (%s != %s)\n", pid.c_str(), deviceId.c_str());
    notifyStatus("WIFI_FAIL");
    return;
  }
  cfgSsid = (const char *)(doc["ssid"] | "");
  cfgPass = (const char *)(doc["password"] | "");
  if (!cfgSsid.length()) { notifyStatus("WIFI_FAIL"); return; }

  wifiProvisioned = true;
  saveConfig();

  state = ST_WIFI_JOINING;
  notifyStatus("WIFI_JOINING");
  Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(), cfgPass.length());
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
    notifyStatus("BLE_OK");
  }
  void onDisconnect(BLEServer *) override {
    bleClientConnected = false;
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

  WiFi.mode(WIFI_STA);
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
  static bool wasWifiConnected = false;
  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (state == ST_WIFI_JOINING) {
    if (wifiConnected) {
      Serial.printf("[WiFi] joined, IP=%s\n", WiFi.localIP().toString().c_str());
      notifyStatus("WIFI_OK");
      formThreadNetwork(); // -> ST_OPERATIONAL on success
    } else if (millis() - wifiJoinStart > WIFI_JOIN_TIMEOUT_MS) {
      Serial.println("[WiFi] join timeout");
      notifyStatus("WIFI_FAIL");
      state = ST_ADVERTISING; // stay connectable, accept a re-write (never one-shot)
    }
  }

  if (state == ST_OPERATIONAL && wasWifiConnected && !wifiConnected) {
    Serial.println("[WiFi] link dropped — Thread mesh keeps running independently");
  }
  wasWifiConnected = wifiConnected;

  delay(50);
}
