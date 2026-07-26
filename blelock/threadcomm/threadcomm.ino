/*
 * threadcomm — OZLOCK Thread COMM MODULE v0 (blecomm fork, 2026-07-23)
 * Board  : ESP32-C6 (bench: same Waveshare Touch-LCD board as blecomm,
 *          display/touch unused by this sketch — see "Not in this
 *          increment" in blelock/CONTRACT-BRIDGE.md)
 *
 * ROLE (ozkey-08 §0, Mode 2 residential): the Thread-transport twin of
 * blecomm. Same OZLOCK identity and GATT service/characteristics as the
 * Wi-Fi-direct lock (blelock/CONTRACT.md) — the app treats "add a lock" as
 * one flow; `info.transport` is what tells it which payload shape to write.
 *
 * THIS INCREMENT proves BLE-provision (a Thread dataset, not Wi-Fi creds)
 * -> Thread mesh attach only. No Tuya MCU wire, no frame relay yet.
 *
 * Commissioning (Option B, locked 2026-07-23): BANOI reads the operational
 * dataset off an already-provisioned bridge32 (blelock/bridge32/) and writes
 * it here verbatim. threadcomm never negotiates a join over the air — it is
 * handed the network's credentials directly and attaches.
 */

#include <esp_mac.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OThread.h>
#include <OThreadUDP.h>
#include <Arduino_GFX_Library.h>

// ── LCD (Waveshare ESP32-C6 Touch LCD 1.47", ST7789 172x320) — pins/offsets
// verified in blelock/HARDWARE.md. threadcomm was headless-only before
// this; touch is intentionally not initialized here (no keypad in F1-F6
// scope). ────────────────────────────────────────────────────────────────
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_RST 22
#define LCD_BL 23
Arduino_DataBus *lcdBus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(lcdBus, LCD_RST, 0, true, 172, 320, 34, 0, 34, 0);
String lastStatus = "BOOT";
String lastUdpLine = "";

// ── GATT contract — SAME service/characteristics as blecomm (CONTRACT.md);
//    this is still "OZLOCK" to the app, just a different transport ─────────
#define BLE_NAME "OZLOCK"
#define SVC_UUID "4f5a4b31-0001-4c4f-434b-000000000001"
#define CHR_PROVISION "4f5a4b31-0002-4c4f-434b-000000000001"
#define CHR_STATUS "4f5a4b31-0003-4c4f-434b-000000000001"
#define CHR_INFO "4f5a4b31-0004-4c4f-434b-000000000001"
#define FW_VERSION "threadcomm-0.1"

#define THREAD_JOIN_TIMEOUT_MS 30000UL

// ── State machine ───────────────────────────────────────────────────────────
enum LockState { ST_ADVERTISING, ST_THREAD_JOINING, ST_OPERATIONAL };
LockState state = ST_ADVERTISING;

Preferences prefs; // namespace "threadcomm"
String deviceId, macStr;
unsigned long threadJoinStart = 0;

OpenThread thread;
DataSet otDataset;

// Local hardware escape hatch (2026-07-26, same as bridge32.ino): hold BOOT
// for 5s to factory reset with no app/BLE reachable at all. Unlike
// bridge32's own NVS-only reset, threadcomm's actual "provisioned state" is
// the Thread dataset itself (our own "threadcomm" Preferences namespace is
// otherwise unused — the dataset lives in OpenThread's own NVS storage), so
// this must erase that via the native otInstanceFactoryReset(), not just
// our prefs. That call needs the OpenThread lock, which the Arduino
// wrapper doesn't expose a public helper for, but esp_openthread_lock.h's
// acquire/release functions are already transitively available via
// <OThread.h>.
#ifndef USER_BUTTON
#define USER_BUTTON BOOT_PIN
#endif
#define FACTORY_RESET_HOLD_MS 5000UL
unsigned long buttonHeldSince = 0;
bool buttonWasDown = false;

void factoryReset() {
  Serial.println("[RESET] factory reset — erasing Thread dataset + NVS");
  prefs.begin("threadcomm", false); prefs.clear(); prefs.end();
  otInstance *inst = thread.getInstance();
  if (inst != nullptr && esp_openthread_lock_acquire(portMAX_DELAY)) {
    otInstanceFactoryReset(inst); // erases OT persistent info + platform reset
    esp_openthread_lock_release(); // unreachable if the reset already fired; harmless otherwise
  }
  ESP.restart(); // fallback in case the OT platform reset above didn't fire
                  // (e.g. instance not ready) — still clears our own prefs
}

// F4: Thread-side frame transport, bridge -> lock (realm-local multicast —
// see the matching comment + rationale in blelock/bridge32/bridge32.ino).
// Group/port MUST match bridge32.ino exactly.
const uint8_t OZ_THREAD_GROUP_BYTES[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x4f, 0x5a};
const IPAddress OZ_THREAD_GROUP(IPv6, OZ_THREAD_GROUP_BYTES);
const uint16_t OZ_THREAD_UDP_PORT = 5052;
#define OZ_UDP_RX_BUF 512

OThreadUDP threadUdp;
bool threadUdpReady = false;
unsigned long threadUdpLastAttempt = 0;
#define THREAD_UDP_RETRY_MS 3000UL

// BLE
BLEServer *bleServer = nullptr;
BLECharacteristic *chrStatus = nullptr, *chrInfo = nullptr;
volatile bool bleClientConnected = false;
String provBuf;

// ─────────────────────────────────────────────────────────────────────────────
// LCD status (bench aid — see the header comment near the pin defines)
// ─────────────────────────────────────────────────────────────────────────────
#define LCD_C_BLACK 0x0000
#define LCD_C_WHITE 0xFFFF
#define LCD_C_GREEN 0x07E0
#define LCD_C_CYAN 0x07FF

void drawStatus() {
  gfx->fillScreen(LCD_C_BLACK);
  gfx->setTextWrap(false);
  gfx->setTextColor(LCD_C_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(4, 4);
  gfx->println("OZLOCK");
  gfx->setTextSize(1);
  gfx->setCursor(4, 26);
  gfx->println("(transport=thread)");
  gfx->setCursor(4, 40);
  gfx->println(deviceId);
  gfx->setTextColor(LCD_C_GREEN);
  gfx->setCursor(4, 58);
  gfx->println(lastStatus);
  gfx->setTextColor(LCD_C_CYAN);
  gfx->setCursor(4, 76);
  gfx->println(lastUdpLine);
}

void checkFactoryResetButton() {
  bool down = digitalRead(USER_BUTTON) == LOW;
  if (down && !buttonWasDown) {
    buttonHeldSince = millis();
  } else if (down && buttonWasDown) {
    unsigned long held = millis() - buttonHeldSince;
    if (held >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[RESET] BOOT held 5s — factory reset");
      gfx->fillScreen(LCD_C_BLACK);
      gfx->setTextColor(LCD_C_WHITE);
      gfx->setTextSize(2);
      gfx->setCursor(4, 40);
      gfx->println("FACTORY RESET");
      factoryReset(); // does not return
    } else if (held > 800 && (held / 500) % 2 == 0) {
      Serial.printf("[RESET] holding BOOT... %lus/5s\n", held / 1000);
    }
  }
  buttonWasDown = down;
}

// ─────────────────────────────────────────────────────────────────────────────
// Status ladder
// ─────────────────────────────────────────────────────────────────────────────
void notifyStatus(const char *wire) {
  Serial.printf("[STATUS] %s\n", wire);
  lastStatus = wire;
  drawStatus();
  if (chrStatus != nullptr) {
    chrStatus->setValue((uint8_t *)wire, strlen(wire));
    if (bleClientConnected) {
      chrStatus->notify();
      // Same fix as bridge32.ino (2026-07-26): a fast-resolving status
      // ladder can overwrite an unsent notify before the phone's BLE stack
      // transmits it, so the app misses a state entirely. Give it room.
      delay(150);
    }
  }
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
// info characteristic
// ─────────────────────────────────────────────────────────────────────────────
void refreshInfo() {
  if (!chrInfo) return;
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  doc["transport"] = "thread";
  doc["thread_role"] = thread ? OpenThread::otGetStringDeviceRole() : "disabled";
  String out; serializeJson(doc, out);
  chrInfo->setValue(out.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// F4: Thread-side frame transport, receive half. threadUdpBegin() opens a
// multicast listen socket once Thread is attached; parsePacket() is polled
// from loop(). Every lock on the mesh gets every datagram and filters by
// its own device_id in "target" (no per-lock addressing yet — v0 scope).
//
// Can fail right after THREAD_OK — same family as the bridge32 bytesToHex
// crash: OpenThread's internal lock isn't always ready the instant Thread
// attaches (live-bench finding, 2026-07-26, reproduced on bridge32's own
// threadUdpBegin()). Fails gracefully here, so the fix is a retry from
// loop(), not a null-check.
// ─────────────────────────────────────────────────────────────────────────────
void threadUdpBegin() {
  if (threadUdpReady) return;
  threadUdpLastAttempt = millis();
  threadUdpReady = threadUdp.beginMulticast(OZ_THREAD_GROUP, OZ_THREAD_UDP_PORT) != 0;
  Serial.printf("[UDP] multicast socket %s on [%s]:%u\n", threadUdpReady ? "open" : "FAILED",
                OZ_THREAD_GROUP.toString().c_str(), OZ_THREAD_UDP_PORT);
}

void pollThreadUdp() {
  if (!threadUdpReady) return;
  int n = threadUdp.parsePacket();
  if (n <= 0) return;

  char buf[OZ_UDP_RX_BUF];
  int got = threadUdp.read(buf, (n < (int)sizeof(buf) - 1) ? n : (int)sizeof(buf) - 1);
  buf[got] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    Serial.println("[UDP] payload not valid JSON, dropped");
    return;
  }
  String target = (const char *)(doc["target"] | "");
  String payloadHex = (const char *)(doc["payload"] | "");
  if (target != deviceId) {
    Serial.printf("[UDP] ignoring datagram for %s (not us)\n", target.c_str());
    return;
  }
  Serial.printf("[UDP] << target=%s payload=%s\n", target.c_str(), payloadHex.c_str());
  // F4 scope ends here — handing payloadHex to the Tuya UART relay is a later increment.
  lastUdpLine = "RX: " + payloadHex;
  drawStatus();
}

// ─────────────────────────────────────────────────────────────────────────────
// Provisioning (BLE write -> Thread dataset commit -> attach)
// distinguished from a Wi-Fi payload by the presence of "network_key"
// (blelock/CONTRACT-BRIDGE.md "threadcomm — GATT service")
// ─────────────────────────────────────────────────────────────────────────────
void applyProvision(JsonDocument &doc) {
  String pid = doc["device_id"] | "";
  if (pid.length() && pid != deviceId) {
    Serial.printf("[PROV] device_id mismatch (%s != %s)\n", pid.c_str(), deviceId.c_str());
    notifyStatus("THREAD_FAIL");
    return;
  }
  String name = (const char *)(doc["network_name"] | "");
  String extPanHex = (const char *)(doc["ext_pan_id"] | "");
  String keyHex = (const char *)(doc["network_key"] | "");
  String panHex = (const char *)(doc["pan_id"] | "");
  int channel = doc["channel"] | 0;

  uint8_t extPanId[8], networkKey[16];
  if (!name.length() || channel < 11 || channel > 26 ||
      !hexToBytes(extPanHex, extPanId, 8) || !hexToBytes(keyHex, networkKey, 16) ||
      panHex.length() != 4) {
    Serial.println("[PROV] malformed Thread dataset payload");
    notifyStatus("THREAD_FAIL");
    return;
  }
  uint16_t panId = (uint16_t)strtoul(panHex.c_str(), nullptr, 16);

  otDataset.initNew();
  otDataset.setNetworkName(name.c_str());
  otDataset.setExtendedPanId(extPanId);
  otDataset.setNetworkKey(networkKey);
  otDataset.setChannel((uint8_t)channel);
  otDataset.setPanId(panId);
  thread.commitDataSet(otDataset);

  Serial.printf("[THREAD] dataset committed: name=%s channel=%d panId=0x%04x\n",
                name.c_str(), channel, panId);

  state = ST_THREAD_JOINING;
  notifyStatus("THREAD_JOINING");
  thread.start();
  thread.networkInterfaceUp();
  threadJoinStart = millis();
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
    // BUG FIX (2026-07-26, bridge32 bench finding): restart unconditionally
    // — gating on state==ST_ADVERTISING meant any client connecting after
    // provisioning permanently stopped advertising on disconnect.
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
  refreshInfo();

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising as OZLOCK (transport=thread)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** threadcomm v0 — OZLOCK Thread comm module ***");
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  gfx->begin();
  gfx->setRotation(5); // landscape, matches blelock/HARDWARE.md's touch transform
  drawStatus();         // deviceId isn't set yet — redrawn again once it is, below

  pinMode(USER_BUTTON, INPUT_PULLUP); // hold 5s -> factory reset, any state

  // Factory base MAC read directly (esp_read_mac) — this sketch never starts
  // the Wi-Fi driver, so WiFi.macAddress() can't be relied on for an ID here.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  macStr = macBuf;
  char machexBuf[13];
  snprintf(machexBuf, sizeof(machexBuf), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceId = "ozk-" + String(machexBuf);
  Serial.printf("[ID] device_id=%s mac=%s\n", deviceId.c_str(), macStr.c_str());
  drawStatus();

  OpenThread::begin(false); // don't auto-start; wait for a committed dataset

  if (thread.hasActiveDataset()) {
    Serial.println("[THREAD] resuming persisted dataset");
    state = ST_THREAD_JOINING;
    thread.start();
    thread.networkInterfaceUp();
    threadJoinStart = millis();
  } else {
    state = ST_ADVERTISING;
  }
  startBle(); // BLE stays up in every state — info/status must stay readable
}

void loop() {
  checkFactoryResetButton();

  static ot_device_role_t lastRole = OT_ROLE_DISABLED;

  if (state == ST_THREAD_JOINING) {
    ot_device_role_t role = OpenThread::otGetDeviceRole();
    if (role == OT_ROLE_CHILD || role == OT_ROLE_ROUTER || role == OT_ROLE_LEADER) {
      Serial.printf("[THREAD] attached as %s\n", OpenThread::otGetStringDeviceRole());
      state = ST_OPERATIONAL;
      notifyStatus("THREAD_OK");
      refreshInfo();
      threadUdpBegin();
    } else if (millis() - threadJoinStart > THREAD_JOIN_TIMEOUT_MS) {
      Serial.println("[THREAD] attach timeout");
      notifyStatus("THREAD_FAIL");
      state = ST_ADVERTISING; // stay connectable, accept a re-written dataset
    }
  }

  ot_device_role_t role = OpenThread::otGetDeviceRole();
  if (role != lastRole) {
    Serial.printf("[THREAD] role change: %s -> %s\n", otRoleString[lastRole], otRoleString[role]);
    lastRole = role;
    refreshInfo();
  }

  pollThreadUdp();

  delay(50);
}
