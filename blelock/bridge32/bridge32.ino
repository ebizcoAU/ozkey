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
#include <openthread/instance.h>   // otInstanceFactoryReset() — see factoryReset()
#include <openthread/thread_ftd.h> // otChildInfo / otThreadGetChildInfoByIndex
                                   // (FTD-only API) — see logThreadChildren()
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
// TEMP (operator request 2026-08-06): disabled for bench diagnostics — a
// short BOOT press to wake the screen also opens the 60s ownership claim
// window (see checkFactoryResetButton()), which contaminates any ownership
// test. Restore to 60000UL once the bridge-ownership investigation is done.
#define LCD_IDLE_OFF_MS 0xFFFFFFFFUL

// RX activity flash (2026-07-28, operator request): when a command arrives over
// MQTT, briefly swap the two footer lines (site / device_id) for a "received"
// banner, then restore them. Makes relay traffic visible on the panel itself —
// which matters because this board's USB-CDC serial has been unreliable to
// capture, while the LCD has been trustworthy all along.
#define LCD_RX_FLASH_MS 5000UL
unsigned long lcdRxFlashUntil = 0;
bool lcdRxFlashActive = false; // so loop() can redraw once when it lapses
String lcdRxMsg;
bool lcdOn = true;
unsigned long lastLcdActivityAt = 0;

// ── GATT contract (blelock/CONTRACT-BRIDGE.md) ──────────────────────────────
#define BLE_NAME "OZBRIDGE"
#define SVC_UUID "4f5a4b32-0001-4272-6467-000000000001"
#define CHR_PROVISION "4f5a4b32-0002-4272-6467-000000000001"
#define CHR_STATUS "4f5a4b32-0003-4272-6467-000000000001"
#define CHR_INFO "4f5a4b32-0004-4272-6467-000000000001"
// Version bumped on every flashed change (operator directive 2026-08-02) — it
// ships on `info.fw`, so the app and servers can tell which contract a device
// speaks, and a serial capture maps to a known build.
//   1.0  initial border-router bootstrap, MQTT uplink, Thread dataset restore
//   1.1  log the formed network's name/ext_pan/channel/pan at THREAD_OK — the
//        missing half of the "is the lock even on my mesh?" question
//   1.2  ONE version per build, in one place. The boot banner carried its own
//        "bridge32 v0 —" directly above `[FW] bridge32-1.x`, so every capture
//        showed two disagreeing versions; the banner now names the product only
//        and FW_VERSION is the single source (operator, 2026-08-02).
//        FW_DISPLAY_VERSION was left at "v1.0" through the 1.1 bump, so the LCD
//        badge disagreed with `info.fw` and with the serial banner. Bumping
//        rather than quietly correcting to "v1.1": two different binaries both
//        answering "bridge32-1.1" is exactly the ambiguity the version rule
//        exists to prevent, and 1.1 is already flashed and captured.
//   1.3  [MESH] now reports OUR role/rloc16/partition and, when we are a Child,
//        our PARENT — replacing "children attached: 0 (0 = the lock is NOT on
//        this mesh)", which was false and misdirected a full day of debugging.
//        Bench truth 2026-08-02: bridge and lock shared ext_pan the whole time
//        with children==0, because the lock was Leader and parented the bridge.
// 1.4 (2026-08-06): [XF-47] bridge ownership guard actually implemented.
// applyProvision() accepted a write from ANY BLE client, forever, with no
// app_id check at all — the guard CONTRACT-BRIDGE.md documents as decided
// since 2026-07-30 was never built. Confirmed exploitable on real hardware
// (a second, unrelated BANOI identity successfully reconfigured an
// already-owned bridge) before this fix. Adds: persisted owner_app_id
// (prefs "owner", cleared only by factory reset), a short-BOOT-press ~60s
// claim window (same pattern as doorlock.ino's touch/BOOT window), and the
// exact guard table on both the normal provision path and the `reset`
// sentinel (previously ungated too — a remote factory-reset was just as
// reachable by anyone as the provision hijack).
// 1.5 (2026-08-06): bench-only — LCD_IDLE_OFF_MS disabled (see its
// definition). No wire/logic change.
// 1.6 (2026-08-07): USER_BUTTON made an explicit, board-owned #define (was
// silently inheriting the toolchain's generic BOOT_PIN, same gap found on
// the doorlock boards). No behavior change on this board — GPIO9 unchanged,
// still not independently hardware-verified here.
#define FW_VERSION "bridge32-1.6"
#define FW_DISPLAY_VERSION "v1.6" // shown on-screen, doorlock.ino's badge convention

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
// This is the ONLY reset path with no radio/app up at all, same reasoning
// as the doorlock boards — so it must be a real, per-board-verified pin,
// never inherited from the toolchain's generic default.
//
// 2026-08-07: this used to silently fall back to the ESP32-C6 core's generic
// BOOT_PIN (GPIO9, esp32-hal.h) with no board-specific verification — same
// gap found and fixed on doorlock.ino/doorlock19.ino this session (the 1.9"
// board's BOOT-hold reset turned out not to work). Kept at 9 here too
// (unchanged behavior) but NOT independently confirmed against this board's
// actual schematic — flag if wrong on real hardware.
#define USER_BUTTON 9
#define FACTORY_RESET_HOLD_MS 5000UL
unsigned long buttonHeldSince = 0;
bool buttonWasDown = false;

// [XF-47] Bridge ownership guard (CONTRACT-BRIDGE.md "Bridge ownership guard")
// — DECIDED 2026-07-30, never actually implemented until now (found
// 2026-08-06 during M4 bench testing: a second, unrelated BANOI identity
// was able to open bridge32's provision characteristic and successfully
// reconfigure an already-owned, already-deployed bridge — no app_id check,
// no claim window, nothing gating it at all). `applyProvision()` used to
// accept a write from any BLE client at any time, forever, per the "keep BLE
// up even post-provision" note further down — exactly the exposure this
// guard exists to close. Confirmed exploitable on real hardware before
// being fixed, not merely inferred from reading the code.
#define BRIDGE_CLAIM_WINDOW_MS 60000UL
unsigned long claimWindowUntil = 0;
bool claimWindowOpen() { return claimWindowUntil && (long)(millis() - claimWindowUntil) < 0; }
String ownerAppId; // "" = unowned. Cleared only by factory reset (prefs.clear()).

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
  } else if (!down && buttonWasDown) {
    // Released. A short press opens the claim window; a long one already
    // wiped the device and never got here — same pattern as doorlock.ino's
    // checkFactoryResetButton().
    unsigned long held = millis() - buttonHeldSince;
    if (held >= 60UL && held < FACTORY_RESET_HOLD_MS) {
      claimWindowUntil = millis() + BRIDGE_CLAIM_WINDOW_MS;
      Serial.printf("[CLAIM] window OPEN %lus (short BOOT press)\n", BRIDGE_CLAIM_WINDOW_MS / 1000);
    }
  }
  buttonWasDown = down;
}

OpenThread thread;
DataSet otDataset;

// ── Thread dataset RESTORE (2026-07-28, operator request) ───────────────────
// Until now bridge32 could only ever SELF-FORM: on a factory reset (or on a
// replacement unit) it invented a random ext-PAN-ID + network key, which meant
// every doorlock already provisioned onto the OLD mesh was orphaned and had to
// be re-paired over BLE at each door. Bit us live 2026-07-28 after a bridge
// delete from the app.
// The app already holds everything needed — it reads this bridge's dataset off
// the `info` characteristic (network_name / ext_pan_id / network_key / channel
// / pan_id) and stores it to provision locks in the first place. So it can now
// hand the SAME dataset back, and every existing lock rejoins on its own with
// no trip to the door. Field names deliberately match what refreshInfo()
// publishes, so it is a straight round-trip.
bool haveRestoreDataset = false;
String rdNetworkName, rdExtPanHex, rdNetworkKeyHex, rdPanIdHex;
uint16_t rdChannel = 0;
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
  ownerAppId = prefs.getString("owner", "");
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
  prefs.putString("owner", ownerAppId);
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
    // Footer: normally site + device_id; briefly the RX banner instead.
    // device_id bumped size1 -> size2 (2026-07-28, operator: unreadable on this
    // 240x135 panel). At size2 the full "ozb-98a316a7e638" is 16 chars * 12px =
    // 192px, so it still fits the 240px width — the "ozb-" prefix is kept
    // because it is what appears verbatim in the MQTT topic you type at the
    // bench. The site line drops to size1 instead: "lab" is short, static, and
    // the least useful thing on the screen.
    if (lcdRxFlashActive && millis() < lcdRxFlashUntil) {
      gfx->setCursor(4, 100);
      gfx->println("<< CMD RX");
      gfx->setCursor(4, 118);
      gfx->println(lcdRxMsg);
    } else {
      gfx->setTextSize(1);
      gfx->setCursor(4, 100);
      if (cfgSiteId.length()) {
        gfx->print("site ");
        gfx->println(cfgSiteId);
      }
      gfx->setTextSize(2);
      gfx->setCursor(4, 114);
      gfx->println(deviceId);
    }
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

  // Show it on the panel for LCD_RX_FLASH_MS, then the footer restores itself
  // (loop() redraws once when lcdRxFlashUntil lapses).
  lcdRxMsg = target.length() ? target : String("(no target)");
  lcdRxFlashUntil = millis() + LCD_RX_FLASH_MS;
  lcdRxFlashActive = true;
  lcdWake(); // redraws immediately with the banner

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

// DIAGNOSTIC (2026-07-28, temporary — remove once the answer is known).
// ff03::1 = realm-local ALL-NODES, which every Thread node subscribes to
// automatically, with no explicit subscription needed. The doorlock's socket
// is bound to IN6ADDR_ANY:5052, so it accepts a datagram sent to EITHER group
// without any doorlock-side change. Sending to both isolates the last two
// hypotheses for why the relay goes unheard:
//   • ff03::1 arrives, ff03::4f5a does not -> our custom group is not being
//     registered with the parent / not subscribed effectively.
//   • neither arrives -> realm-local multicast is not reaching a Child at all
//     (link-mode / MPL forwarding), independent of which group we use.
const uint8_t OZ_ALLNODES_BYTES[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
const IPAddress OZ_REALM_ALLNODES(IPv6, OZ_ALLNODES_BYTES);

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

// One multicast send, logged per destination group so the bench can tell which
// group a datagram actually went out on.
static bool sendToThreadGroup(const IPAddress &group, const String &target,
                              const String &payloadHex, const char *label) {
  // "via" tags which multicast group carried this datagram. The doorlock reads
  // only "target"/"payload" and ArduinoJson ignores unknown keys, so this is
  // inert to the relay — but the doorlock's rx diagnostic dumps the whole
  // buffer, making it obvious which group actually got through.
  JsonDocument doc;
  doc["target"] = target;
  doc["payload"] = payloadHex;
  doc["via"] = label;
  String out;
  serializeJson(doc, out);

  if (!threadUdp.beginPacket(group, OZ_THREAD_UDP_PORT)) {
    Serial.printf("[UDP] beginPacket failed (%s)\n", label);
    return false;
  }
  threadUdp.write((const uint8_t *)out.c_str(), out.length());
  if (!threadUdp.endPacket()) {
    Serial.printf("[UDP] endPacket failed (%s)\n", label);
    return false;
  }
  Serial.printf("[UDP] >> [%s] %s\n", label, out.c_str());
  return true;
}

// DIAGNOSTIC (2026-07-28, temporary): who is actually attached to THIS mesh?
// The doorlock's own LCD/MON "JOINED" comes from a latched flag that is never
// cleared, so it keeps claiming JOINED after the bridge re-forms the network
// and orphans it.
//
// ⚠ CORRECTED 2026-08-02 — this used to print "0 = the lock is NOT on this mesh"
// and that assertion is FALSE. It cost a full day. The child table only lists
// devices for which THIS node is the parent; it says nothing about a device that
// is our parent, our sibling, or a router elsewhere in the partition. Bench
// proof: bridge and lock were on ext_pan 916e387c9a1bd316 the entire time, and
// children==0 throughout — because the *lock* was Leader and the bridge attached
// to IT as a Child (19:42:03, role=Child after 102ms). We wrote a diagnostic
// that asserted the opposite of the truth and then reasoned from it.
//
// So report OUR role and partition first: on this network the interesting
// question is not "who are my children" but "whose partition am I in, and am I
// the router I was designed to be". children==0 is only meaningful once role is
// known to be Leader/Router.
static void logThreadChildren() {
  otInstance *inst = thread.getInstance();
  if (inst == nullptr) {
    Serial.println("[MESH] no OT instance");
    return;
  }
  // LOCK DISCIPLINE (2026-07-28): this runs on the MQTT callback task, NOT the
  // OpenThread task. Calling ot* APIs from a foreign task without holding
  // esp_openthread's lock can corrupt/crash the stack — exactly the class of
  // fault this file already documents for getExtendedPanId()/getNetworkKey().
  // Bounded wait so a busy OT task costs us one skipped diagnostic, never a
  // blocked relay: forwarding the command matters more than logging.
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
    Serial.println("[MESH] lock busy — skipped child dump");
    return;
  }
  otDeviceRole role = otThreadGetDeviceRole(inst);
  uint16_t rloc16 = otThreadGetRloc16(inst);
  uint32_t partition = otThreadGetPartitionId(inst);

  // If we are a Child, name our PARENT — that is the device the old message
  // would have reported as "not on this mesh".
  otRouterInfo parent;
  bool haveParent = (role == OT_DEVICE_ROLE_CHILD) &&
                    (otThreadGetParentInfo(inst, &parent) == OT_ERROR_NONE);

  int n = 0;
  otChildInfo ci;
  for (uint16_t i = 0; i < 64; i++) {
    if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE) continue;
    n++;
    Serial.printf("[MESH] child %d: rloc16=0x%04x rssi=%d timeout=%lus\n", n,
                  ci.mRloc16, (int)ci.mAverageRssi, (unsigned long)ci.mTimeout);
  }
  esp_openthread_lock_release();

  const char *roleName = (role == OT_DEVICE_ROLE_LEADER)   ? "Leader"
                         : (role == OT_DEVICE_ROLE_ROUTER) ? "Router"
                         : (role == OT_DEVICE_ROLE_CHILD)  ? "Child"
                         : (role == OT_DEVICE_ROLE_DETACHED) ? "Detached"
                                                             : "Disabled";
  Serial.printf("[MESH] self role=%s rloc16=0x%04x partition=0x%08lx children=%d\n",
                roleName, rloc16, (unsigned long)partition, n);
  if (haveParent) {
    // otRouterInfo carries NO rssi — mAverageRssi is otChildInfo's, which is why
    // the child line above compiles and this one did not (esp32c6-libs 3.3.11,
    // openthread/thread.h:140). Link quality in (0-3) is the equivalent here.
    Serial.printf("[MESH] parent rloc16=0x%04x routerid=%u lqi_in=%u — WE ARE A CHILD. "
                  "The border router should be Leader/Router; a battery lock should "
                  "not parent it.\n",
                  parent.mRloc16, (unsigned)parent.mRouterId,
                  (unsigned)parent.mLinkQualityIn);
  } else if (n == 0 && (role == OT_DEVICE_ROLE_LEADER || role == OT_DEVICE_ROLE_ROUTER)) {
    Serial.println("[MESH] children=0 and we are Leader/Router — no device has us as "
                   "its parent (it may still be elsewhere in this partition).");
  }
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
  logThreadChildren(); // DIAGNOSTIC (temporary) — is the lock even on this mesh?
  sendToThreadGroup(OZ_THREAD_GROUP, target, payloadHex, "ff03::4f5a");
  // DIAGNOSTIC (2026-07-28, temporary): same datagram to realm-local
  // all-nodes — see the OZ_REALM_ALLNODES note above.
  sendToThreadGroup(OZ_REALM_ALLNODES, target, payloadHex, "ff03::1");

  // DIAGNOSTIC (2026-07-28, temporary — REMOVE, hard-codes one bench lock).
  // Unicast control test. The doorlock's mesh-local EID, read from its own
  // boot dump. Its stack confirms it has joined BOTH ff03::4f5a and ff03::1,
  // it is an attached Child of this bridge (RLOC16 0x7401), and its socket is
  // bound with no netif filter — yet neither multicast is ever delivered.
  // This separates the last two possibilities:
  //   • unicast ARRIVES, multicast does not -> multicast delivery to a Child
  //     is broken; the relay needs a lock->address map (which ozkey-11 §3
  //     ruled unnecessary, but that assumed multicast worked).
  //   • unicast ALSO fails -> UDP receive is broken end-to-end regardless of
  //     addressing, and the next step is the OpenThread CLI (`ot ping`) to
  //     test below our code entirely.
  IPAddress benchLock;
  if (benchLock.fromString("fd30:4e72:549c:3c5b:5630:8734:5090:340b")) {
    sendToThreadGroup(benchLock, target, payloadHex, "unicast-ML-EID");
  } else {
    Serial.println("[UDP] bench unicast address failed to parse");
  }
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
  // ROOT CAUSE of "the app can't open the door" (found 2026-07-29).
  // PubSubClient defaults to MQTT_MAX_PACKET_SIZE = 256 bytes and DISCARDS any
  // larger inbound packet silently — no error, no callback, nothing in the log.
  // ozlockserv's real command envelope (msg_id/device_id/action/grant_id/
  // payload_hex/issued_at/source/target/payload) is ~280 bytes of JSON plus a
  // ~45-byte topic, so every command from the SERVER was dropped here, while
  // hand-rolled `{target,payload}` test publishes (~130 bytes total) always got
  // through. That is exactly why bench tests passed and the app never worked.
  // 1024 leaves headroom for the ozkey-06 sealed envelope, which will be
  // larger again (ver+counter+nonce+ciphertext+tag, hex-encoded).
  mqttClient.setBufferSize(1024);
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

  // RESTORE wins over both resume and self-form: the whole point is to put a
  // reset/replacement bridge back onto the household's EXISTING mesh so the
  // locks rejoin themselves.
  uint8_t rExtPan[8], rKey[16], rPan[2];
  if (haveRestoreDataset && hexToBytes(rdExtPanHex, rExtPan, 8) &&
      hexToBytes(rdNetworkKeyHex, rKey, 16) && hexToBytes(rdPanIdHex, rPan, 2)) {
    otDataset.initNew();
    otDataset.setNetworkName(rdNetworkName.c_str());
    otDataset.setExtendedPanId(rExtPan);
    otDataset.setNetworkKey(rKey);
    otDataset.setChannel(rdChannel);
    otDataset.setPanId((uint16_t)((rPan[0] << 8) | rPan[1]));
    thread.commitDataSet(otDataset);
    Serial.printf("[THREAD] RESTORED dataset from app: name=%s channel=%u panId=0x%s\n",
                  rdNetworkName.c_str(), rdChannel, rdPanIdHex.c_str());
    haveRestoreDataset = false; // one-shot; NVS holds it from here
  } else if (thread.hasActiveDataset()) {
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

  // DIAGNOSTIC (2026-08-02): print WHICH network this is. One line, and it
  // settles a question that has now cost hours twice — when a doorlock sits at
  // Leader on its own partition, the ONLY thing distinguishing "the app handed
  // it a stale dataset" from "same network, ordinary partition-merge delay" is
  // comparing ext_pan_id, and neither device printed it. The lock already logs
  // its side (`[PROV] thread fields: … ext_pan=…`); this is the missing half.
  // Companion to logThreadChildren(): that says WHO is attached, this says WHAT
  // network they would be attaching to.
  //
  // Placed AFTER the fail-closed check above on purpose — on a failed attach the
  // dataset is unattached and, per that comment, likely garbage; printing it
  // would look authoritative and mislead. Uses `const DataSet &` and DataSet's
  // own getters, which are plain struct reads: the 2026-07-26 crash was
  // thread.getExtendedPanId() (lock-dependent, can return nullptr), not these.
  {
    const DataSet &ds = thread.getCurrentDataSet();
    Serial.printf("[THREAD] network name='%s' ext_pan=%s ch=%u pan=%04x\n",
                  ds.getNetworkName(), bytesToHex(ds.getExtendedPanId(), 8).c_str(),
                  (unsigned)ds.getChannel(), (unsigned)ds.getPanId());
  }

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

// [XF-47] Bridge ownership guard — the exact table from CONTRACT-BRIDGE.md:
//   no owner, app_id present OR absent  -> claim window open?  yes: apply (claim
//                                          if app_id present) | no: BRIDGE_CLAIM_REQUIRED
//   owner == incoming app_id            -> ok, no window needed (normal re-provision)
//   owner != incoming app_id            -> BRIDGE_DENIED
//   owner exists, app_id ABSENT         -> BRIDGE_DENIED
// Refusal is atomic — caller must not apply ANY field (broker/Wi-Fi/reset) when
// this returns false, or an attacker who fails the app_id check can still
// repoint the bridge by omitting it (the exact bypass XF-47 review closed).
bool bridgeOwnershipCheck(const String &incomingAppId) {
  if (ownerAppId.length() == 0) {
    if (!claimWindowOpen()) {
      Serial.println("[CLAIM] unowned, window closed — BRIDGE_CLAIM_REQUIRED");
      notifyStatus("BRIDGE_CLAIM_REQUIRED");
      return false;
    }
    if (incomingAppId.length()) {
      ownerAppId = incomingAppId; // claimed — persisted by the caller's saveConfig()
      Serial.printf("[CLAIM] bridge claimed by app_id=%.16s…\n", incomingAppId.c_str());
    } else {
      Serial.println("[CLAIM] window open, no app_id offered — applying unowned (bench)");
    }
    return true;
  }
  if (incomingAppId.length() && incomingAppId == ownerAppId) return true; // idempotent
  Serial.printf("[CLAIM] owner mismatch (owner=%.16s… incoming=%s) — BRIDGE_DENIED\n",
                ownerAppId.c_str(), incomingAppId.length() ? incomingAppId.c_str() : "(absent)");
  notifyStatus("BRIDGE_DENIED");
  return false;
}

void applyProvision(JsonDocument &doc) {
  String incomingAppId = doc["app_id"] | "";

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
    // Same ownership guard as any other write — a remote reset is exactly as
    // destructive as repointing the broker, and was previously reachable by
    // ANY BLE client with no check at all. Found 2026-08-06 alongside the
    // main provision-hijack bug; same root cause, same fix.
    if (!bridgeOwnershipCheck(incomingAppId)) return;
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

  // Ownership guard BEFORE any field is touched (atomicity — CONTRACT-BRIDGE.md
  // "Refusal is atomic in every branch"). notifyStatus() for the refusal is
  // sent inside the check itself.
  if (!bridgeOwnershipCheck(incomingAppId)) return;

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

  // Optional Thread dataset to RESTORE (see haveRestoreDataset above). All five
  // fields must be present and well-formed or we ignore the lot and fall back
  // to resume/self-form — a half-applied dataset would strand the bridge on a
  // network nothing else is on, which is worse than forming a fresh one.
  {
    String nn = (const char *)(doc["network_name"] | "");
    String ep = (const char *)(doc["ext_pan_id"] | "");
    String nk = (const char *)(doc["network_key"] | "");
    String pi = (const char *)(doc["pan_id"] | "");
    uint16_t ch = (uint16_t)(doc["channel"] | 0);
    if (nn.length() && ep.length() == 16 && nk.length() == 32 &&
        pi.length() == 4 && ch >= 11 && ch <= 26) {
      rdNetworkName = nn;
      rdExtPanHex = ep;
      rdNetworkKeyHex = nk;
      rdPanIdHex = pi;
      rdChannel = ch;
      haveRestoreDataset = true;
      Serial.printf("[PROV] Thread dataset supplied — will RESTORE mesh %s "
                    "(ch=%u panId=0x%s) instead of forming a new one\n",
                    nn.c_str(), ch, pi.c_str());
    } else if (nn.length() || ep.length() || nk.length() || pi.length()) {
      Serial.println("[PROV] partial/invalid Thread dataset in payload — ignored");
    }
  }

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
  Serial.println("\n*** OZBRIDGE Thread border router bootstrap ***");
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

  // RX banner lapsed -> redraw once to put site/device_id back.
  if (lcdRxFlashActive && millis() >= lcdRxFlashUntil) {
    lcdRxFlashActive = false;
    if (lcdOn) drawStatus();
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
