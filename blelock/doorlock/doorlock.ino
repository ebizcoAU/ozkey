/*
 * doorlock — unified OZLOCK COMM MODULE (forked from blecomm.ino, 2026-07-26)
 * Board : Waveshare ESP32-C6 Touch-LCD 1.47" (pin map: blelock/HARDWARE.md)
 *
 * UNIFICATION (docs/ozkey-10.md, operator directive 2026-07-26): ONE
 * firmware image for both transports blecomm.ino (Wi-Fi) and threadcomm.ino
 * (Thread) used to split across. Selected per-device by the BLE provision
 * payload's shape at commissioning time — never by which sketch was
 * flashed:
 *   - `ssid` present        -> Wi-Fi transport (blecomm's original path,
 *                              unchanged below)
 *   - `network_key` present -> Thread transport (ported from threadcomm.ino:
 *                              join the dataset an already-provisioned
 *                              bridge32 hands over BLE, then receive DPID
 *                              command frames over the F4 UDP relay proven
 *                              2026-07-25/26)
 * `mode` (ozkey-cloud|ozkey-local) is orthogonal to transport — it still
 * selects WHO the lock talks to; transport only selects HOW.
 *
 * KNOWN GAP, DELIBERATE (ozkey-10.md §5/§7): the Thread path's UPLINK
 * (heartbeat/log -> bridge32 -> MQTT -> server) is NOT built — bridge32's
 * own UDP socket is send-only today, and there is no addressing scheme yet
 * for a lock to reach the bridge from a Thread mesh with no discovery
 * mechanism. This pass proves Thread-join + DOWNLINK (bridge -> lock DPID
 * frames, e.g. remote unlock) and measures flash footprint on N8. A
 * Thread-transport lock therefore has no real "enrolled/paired" concept
 * yet; it goes operational (keypad/DPID/PIN storage all work) the moment
 * Thread attaches, since there is nothing yet for it to enroll WITH.
 *
 * ROLE SPLIT (real Tuya architecture, unchanged from blecomm): this sketch
 * is PURELY the comm module (a TYWE3S equivalent). ALL lock duty — keypad,
 * RFID, fingerprint, battery, credential storage/validation, motor — lives
 * on the MCU = LockSim Mode B, connected over the Tuya 55 AA bus (Serial1
 * GPIO16/17 @9600 8N1 → CP2102).
 *
 *   server → module : command frame (via MQTT or Thread UDP, transport-
 *                     dependent) → RAW frame forwarded to MCU (never
 *                     executed locally — the MCU owns credentials)
 *   MCU → module    : 55 AA frames translated up: ACCESS_RESULT (DP 8) →
 *                     granted/denied/expired door logs; other DPs → dp_report
 *
 * Provisioning/network spine is blelock-identical for the Wi-Fi path: BLE
 * "OZLOCK" advertise → BANOI/MAOI ProvisionPayload → WiFi → MQTT → enroll
 * (cloud) or unpaired-announce + provision_assign (hotel). Same NVS
 * namespace, so a board flashed blelock↔blecomm↔doorlock keeps its
 * enrollment (transport field is additive, defaults to "wifi" for existing
 * rows).
 * Factory reset: same invisible '*' then '5' touch zones, every screen —
 * now also erases the persisted Thread dataset via otInstanceFactoryReset()
 * when the active transport is Thread (a plain NVS wipe alone would leave
 * the device rejoining the same old mesh on reboot).
 * Screen = comm dashboard (no keypad): mode/transport/broker/MCU-link +
 * counters.
 *
 * Power/wake model (ozkey-08 §0.2/§0.3): persistent power (keep-alive
 * topology), SRDY/MRDY wake handshake on GPIO7/8, module-owned proactive
 * pull timer (heartbeat_s, 1-10 min) — Wi-Fi transport only for now. Bench:
 * NVS wake_sim=true (CP2102 has no wake wires) = SRDY assumed asserted, no
 * sleep; MRDY still driven. Thread SED polling as a wake source (ozkey-10
 * §7 Q1) is an open decision, not yet wired — Thread-transport locks simply
 * never enter the keep-alive nap in this pass (see the loop() gate).
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
#include <LittleFS.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "ozcrypto.h" // member-ceremony crypto (XF-46 §7.1) + boot self-test
#include <OThread.h>     // Thread transport (ported from threadcomm.ino)
#include <OThreadUDP.h>  // F4 UDP relay (bridge32/threadcomm proven, 2026-07-25/26)
// Receive half runs on lwIP, not OpenThread's internal UDP — see the root-cause
// note above threadUdpBegin() (esp_openthread_netif_glue pushes inbound Thread
// packets into lwIP, so otUdp* sockets never see them).
#include <lwip/sockets.h>
#include <errno.h>
#include <fcntl.h>

// ── Hardware pins (HARDWARE.md, operator-verified) ──────────────────────────
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 1
#define LCD_DIN 2
#define LCD_RST 22
#define LCD_BL 23
#define I2C_SDA 18
#define I2C_SCL 19
#define TOUCH_RST 20
#define TOUCH_INT 21
#define TOUCH_ADDR 0x63

// Tuya MCU bus → LockSim Mode B (wire-verified 2026-07-19)
#define TUYA_TX_PIN 16  // -> CP2102 RXD
#define TUYA_RX_PIN 17  // <- CP2102 TXD

// §0.2 wake lines (Tuya keep-alive contract): active low, answer-before-
// transmit, 10 s serial-idle release. GPIO1-4 reserved (SPI/SD). SRDY on an
// LP pin (deep-sleep-wake capable). GPIO8 is a C6 strapping pin — MRDY
// idles HIGH so boot is unaffected; remap on real lock hw if its MCU pulls
// this line low at reset.
#define SRDY_PIN 7  // MCU → module: "module, wake" / held low = MCU awake
#define MRDY_PIN 8  // module → MCU: "MCU, wake" / held low = module awake

// ── BGR-corrected palette (panel is BGR) ────────────────────────────────────
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0x001F
#define C_BLUE 0xF800
#define C_GREEN 0x07E0
#define C_AMBER 0x051F
#define C_GREY 0x8410
#define C_DIM 0x39E7

Arduino_DataBus *bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_DIN);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, false /*BGR*/, 172, 320, 34, 0, 34, 0);

// ── GATT contract (blelock/CONTRACT.md — unchanged so BANOI/MAOI pair as-is) ─
#define BLE_NAME "OZLOCK"
#define SVC_UUID "4f5a4b31-0001-4c4f-434b-000000000001"
#define CHR_PROVISION "4f5a4b31-0002-4c4f-434b-000000000001"
#define CHR_STATUS "4f5a4b31-0003-4c4f-434b-000000000001"
#define CHR_INFO "4f5a4b31-0004-4c4f-434b-000000000001"
// ── Firmware version ────────────────────────────────────────────────────────
// BUMP THE MINOR ON EVERY FLASHED CHANGE (operator directive 2026-08-02). This
// is not bookkeeping: FW_VERSION goes out on `info.fw` and is persisted as
// `locks.fw` server-side, so it is how BANOI and ozlockserv tell which contract
// a device speaks — and how a serial capture is matched to a known build. The
// compiler's __DATE__ stamp identifies a binary but nobody can talk about it.
//
//   1.0  M1 — AES-GCM envelope in ozcrypto.h, boot self-test vs ozkey-06 §5
//   1.1  M2 — bond #0 from provision.app_id (BOND_OK / BOND_DENIED, atomic
//             refusal); LCD bottom row shows Owner instead of the txlog count;
//             factoryReset() order fix so the txlog wipe and screen flash are
//             no longer dead code behind otInstanceFactoryReset(); Thread
//             attach-timeout latch fix (was re-notifying THREAD_FAIL every
//             loop, flooding a connected app's status ladder)
//             — superseded by 1.2 before it was ever flashed; no board ran it
//   1.2  XF-52 (R) — BLE maintenance window: a short BOOT press makes a
//             COMMISSIONED lock discoverable for 60 s. Deliberately physical;
//             there is no remote verb for it and there must never be one.
//             Unblocks M3 member_enroll and XF-52 (S) `Ghép lại`, neither of
//             which could reach a working lock at all.
//   1.3  STALE-ROLE FIX — after commitDataSet() the stack briefly still reports
//             the OLD network's role, so the lock announced THREAD_OK 0 ms after
//             THREAD_JOINING, went ST_OPERATIONAL, and never re-evaluated —
//             claiming "Thread: OK" while orphaned on a dead partition. Root
//             cause of the XF-53 ladder hang, which we had wrongly attributed
//             to the app catching a fast-but-honest attach.
//             Also XF-53 (Y): bleClientConnected is now DERIVED from a link
//             count, not latched false by any teardown, so a stale link closing
//             while a live one is up can no longer silence the status ladder.
//             onConnect/onDisconnect now log — they were mute, which is why the
//             T2 capture could not settle (Y) either way.
//             Also: boot banner no longer carries a second, stale version.
//   1.4  LCD told the operator the lock was fine when it was not. "Thread:
//             JOINED" came from `threadFormed`, set once at first attach and
//             cleared nowhere, so it stayed green through detachment and through
//             the lock leading its own orphaned partition. Now reads the LIVE
//             role and NAMES it — CHILD / ROUTER / LEADER! / DOWN — because the
//             role is the thing that matters: a doorlock showing LEADER is the
//             topology inversion, not a healthy lock. The [MON] serial line was
//             given the live role on 2026-07-28; the display an installer
//             actually reads was left latched for five weeks.
//             Same pass, same fault class: an UNPROVISIONED lock showed
//             "(wifi)" and logged xport=wifi because cfgTransport defaults to
//             "wifi" (:572) — a default rendered as a decision. It now reads
//             "(mode: set by app)" / xport=unset until the app actually picks a
//             transport, which it does by sending network_key or not.
#define FW_VERSION "doorlock-1.4"
#define FW_DISPLAY_VERSION "v1.4" // shown on-screen next to the OZLOCK logo

// ── State machine ───────────────────────────────────────────────────────────
enum CommState { ST_ADVERTISING, ST_JOINING, ST_OPERATIONAL };
CommState state = ST_ADVERTISING;

Preferences prefs; // namespace "blelock" — shared with blelock deliberately

String cfgSsid, cfgPass, cfgBrokerHost, cfgServerIp, cfgSiteId, cfgName, cfgDeviceId;
uint16_t cfgBrokerPort = 1883, cfgServerPort = 4200;
uint32_t cfgHeartbeatS = 60;
bool provisioned = false, enrolled = false;
String cfgMode = "ozkey-cloud", cfgRoomNo, cfgMacToken;
bool isLocalMode() { return cfgMode == "ozkey-local"; }

String deviceId, macStr;

// ── Transport (2026-07-26, ozkey-10 unification) ────────────────────────────
// "wifi" (blecomm's original path) or "thread" (ported from threadcomm.ino).
// Selected once, at provisioning, by payload shape — see applyProvision().
String cfgTransport = "wifi"; // NVS "xport"
bool isThread() { return cfgTransport == "thread"; }

OpenThread thread;
DataSet otDataset;
bool threadFormed = false;
// Set when a NEW dataset is committed: until the stack reports DETACHED at least
// once, otGetDeviceRole() may still be returning the role from the network we
// are leaving. See the STALE-ROLE FIX in loop(). Not set on boot-resume, where
// the stack starts detached and the role is trustworthy from the first read.
bool threadAwaitDetach = false;
unsigned long threadJoinStart = 0;
#define THREAD_JOIN_TIMEOUT_MS 30000UL

// F4 UDP relay, receive half (ported from threadcomm.ino, bench-proven
// 2026-07-25/26). DOWNLINK ONLY in this pass — see the header comment for
// why uplink isn't built yet. Group/port MUST match bridge32.ino exactly.
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
// XF-53 (Y). bleClientConnected used to be a plain bool that onDisconnect set
// false unconditionally — so ANY link teardown cleared it, including a stale
// link's, while a second live link was still up. A re-provision is by definition
// a second connection to a lock we have connected to before, which is exactly
// when that happens. Symptom is silent: notifyStatus() then skips notify() and
// its 150 ms settle, so the app never receives that rung of the ladder and sits
// on a spinner. Count links instead of latching a bool; the flag is now derived.
volatile int bleLinkCount = 0;
// XF-52 (R) maintenance window. Declared HERE, not beside its helpers further
// down, because drawStatus() reads it — and the IDE auto-prototypes functions
// but NOT variables, so a later declaration compiles as "not declared in this
// scope" with a confusing line number.
unsigned long bleWindowUntil = 0; // 0 = closed; millis deadline while open
bool bleWindowOpen();
String provBuf;

// Networking
WiFiClient wifiTcp;
PubSubClient mqtt(wifiTcp);
unsigned long lastHeartbeat = 0, lastMqttAttempt = 0, wifiJoinStart = 0;
unsigned long lastEnrollSent = 0;
uint8_t enrollAttempts = 0;
unsigned long lastUnpairedAnnounce = 0;
String topicCommand, topicEnroll, topicHeartbeat, topicLog, topicPairConfirm;
#define TOPIC_UNPAIRED "hotel/locks/unpaired/heartbeat"

bool screenDirty = true;
String joinLine1 = "", joinLine2 = "";
bool touchWasDown = false;

// ── §0.2/§0.3 power & wake state (persistent-power keep-alive) ──────────────
// wake_sim=true (bench default; CP2102 exposes TX/RX only): SRDY assumed
// asserted, module never sleeps; MRDY still driven genuinely (probe-able).
// wake_sim=false: honest handshake + light sleep — wake on SRDY low or the
// heartbeat_s proactive-pull timer. Toggle: MQTT {op:"wake_sim","on":bool}.
bool wakeSim = true;                  // NVS "wksim"
bool mrdyAsserted = false;
unsigned long lastWireActivityAt = 0; // any Serial1 byte, either direction
unsigned long lastActivityAt = 0;     // frames / MQTT rx / touch / connects
uint32_t sleepWakeCount = 0;
#define MRDY_IDLE_RELEASE_MS 10000UL  // Tuya: release after 10 s serial idle
#define SRDY_WAIT_TIMEOUT_MS 1500UL   // answer-before-transmit guard
#define SLEEP_IDLE_MS 30000UL         // nap after 30 s with nothing to do

bool srdyAsserted() { return wakeSim || digitalRead(SRDY_PIN) == LOW; }

void mrdySet(bool assertLow) {
  if (mrdyAsserted == assertLow) return;
  mrdyAsserted = assertLow;
  digitalWrite(MRDY_PIN, assertLow ? LOW : HIGH);
  Serial.printf("[WAKE] MRDY %s\n",
                assertLow ? "LOW (awake/has data)" : "HIGH (idle release)");
}

// §0.3: heartbeat_s doubles as the proactive-pull interval — user range is
// 1-10 min (60-600 s); clamp whatever provisioning/ack delivers.
uint32_t clampHeartbeatS(uint32_t s) {
  return s < 60 ? 60 : (s > 600 ? 600 : s);
}

// ── MCU bus health (drives the dashboard) ───────────────────────────────────
uint32_t mcuTxFrames = 0;         // frames forwarded server → MCU
uint32_t mcuRxFrames = 0;         // frames received MCU → module
unsigned long lastMcuFrameAt = 0; // millis() of last frame FROM the MCU
String lastMcuSummary = "";       // one-line description of it
// LockSim heartbeats every 60s — no frame for 90s = MCU link considered down
#define MCU_LINK_TIMEOUT_MS 90000UL
bool mcuLinkUp() { return lastMcuFrameAt && millis() - lastMcuFrameAt < MCU_LINK_TIMEOUT_MS; }

// Door status as REPORTED by the MCU traffic we relay (the MCU owns the bolt;
// this is the comm module's mirror of it): granted/remote-unlock → UNLOCKED,
// reverting after LockSim's known 5s auto-relock.
String doorStatus = "LOCKED";
unsigned long doorUnlockAt = 0;
#define DOOR_UNLOCK_MS 5000UL
void markDoorUnlocked() {
  doorStatus = "UNLOCKED";
  doorUnlockAt = millis();
  screenDirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRANSACTION LOG (LittleFS). ⚠ DP-9/tier-2 credential DB REMOVED 2026-07-19:
// the doorlock speaks STRICT Tuya DP vocabulary only (the maker won't change
// MCU firmware for us — attempted-credential values never cross the UART in
// the standard protocol). Large-directory auth belongs to a separate
// access-control device, not this comm module. See ozkey-08 §0.
// ─────────────────────────────────────────────────────────────────────────────
#define TXLOG_ROTATE_LINES 5000 // two files × 5000 = 10,000-event buffer

bool fsUp = false;
uint32_t txlogCount0 = 0, txlogCount1 = 0; // lines in /txlog.0 + /txlog.1

// 10,000-event transaction buffer: JSONL ring across /txlog.0 (live) and
// /txlog.1 (previous). Rotate at 5,000 lines each. Every event is captured
// even with the network down — the upstream MQTT publish is best-effort.
uint32_t txlogCountLines(const char *path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  uint32_t n = 0;
  while (f.available()) if (f.read() == '\n') n++;
  f.close();
  return n;
}

void txlogAppend(const char *result, const char *detail) {
  if (!fsUp) return;
  if (txlogCount0 >= TXLOG_ROTATE_LINES) {
    LittleFS.remove("/txlog.1");
    LittleFS.rename("/txlog.0", "/txlog.1");
    txlogCount1 = txlogCount0;
    txlogCount0 = 0;
  }
  File f = LittleFS.open("/txlog.0", "a");
  if (!f) return;
  JsonDocument doc;
  String ts = isoNow();
  if (ts.length()) doc["ts"] = ts; else doc["up_ms"] = millis();
  doc["result"] = result;
  doc["detail"] = detail;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
  txlogCount0++;
}

uint32_t txlogTotal() { return txlogCount0 + txlogCount1; }

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
  if (now < 1600000000) return String("");
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
    if (bleClientConnected) {
      chrStatus->notify();
      // Same fix as bridge32.ino (2026-07-26): a fast-resolving status
      // ladder can overwrite an unsent notify before the phone's BLE stack
      // transmits it, so the app misses a state entirely. Give it room.
      delay(150);
    }
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
  gfx->println("OZLOCK COMM MODULE (doorlock)");
  gfx->setCursor(15, 28);
  gfx->print("BLE: ");
  gfx->print(bleClientConnected ? "APP CONNECTED" : "ADVERTISING...");
  gfx->setTextSize(3);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(52, 70);
  gfx->println("OZLOCK");
  gfx->setTextSize(2); // bumped from 1 (2026-07-27) — too small to read
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(172, 86); // version badge beside the logo
  gfx->println(FW_DISPLAY_VERSION);
  gfx->setTextSize(1);
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
  gfx->println(joinLine1);
  gfx->setCursor(15, 88);
  gfx->println(joinLine2);
  gfx->setCursor(15, 120);
  gfx->setTextColor(C_DIM);
  gfx->print("device_id: ");
  gfx->println(deviceId);
  gfx->setCursor(15, 152);
  gfx->println("reset: tap * then 5 (left edge)");
}

// OPERATIONAL dashboard (operator spec): DOOR STATUS is the hero element,
// plus door name, IP, network status. Border colour = health summary:
// GREEN = net + MCU link up · AMBER = one leg down · RED = both down.
void drawOperational() {
  // Transport-aware (2026-07-26): Wi-Fi's "connected" is WL_CONNECTED+MQTT;
  // Thread's is just having attached (threadFormed) — this pass has no
  // uplink, so there's no MQTT-equivalent "server reachable" signal for
  // Thread yet (ozkey-10 §5). Showing plain WiFi state for a Thread board
  // would just be wrong, not merely incomplete.
  // ⚠ 2026-08-02 (operator: "LCD shows Thread: JOINED — misleading").
  // threadFormed is set true ONCE (doorlock.ino:1839) and cleared NOWHERE, so the
  // LCD kept showing green JOINED while the lock was detached or orphaned. The
  // [MON] serial line was fixed to read the live role on 2026-07-28 (see :1936)
  // and the LCD — the thing an installer actually looks at — was left latched.
  // Read the live role here too. ROUTER/LEADER on a doorlock is not "joined to
  // the bridge": it means we formed or took over a partition, which is the
  // topology inversion, so only CHILD counts as properly attached below.
  ot_device_role_t liveRole = isThread() ? OpenThread::otGetDeviceRole() : OT_ROLE_DISABLED;
  bool threadAttached = (liveRole == OT_ROLE_CHILD || liveRole == OT_ROLE_ROUTER ||
                         liveRole == OT_ROLE_LEADER);
  bool netUp = isThread() ? threadAttached
                          : ((WiFi.status() == WL_CONNECTED) && mqtt.connected());
  bool mcuUp = mcuLinkUp();
  bool open = doorStatus == "UNLOCKED";
  uint16_t border = (netUp && mcuUp) ? C_GREEN : (netUp || mcuUp) ? C_AMBER : C_RED;
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, 320, 172, border);
  gfx->drawRect(1, 1, 318, 170, border);

  // top strip: module id + mode
  gfx->setTextSize(1);
  gfx->setTextColor(border);
  gfx->setCursor(15, 8);
  gfx->print("OZLOCK ");
  // An unprovisioned lock has NO transport — cfgTransport merely defaults to
  // "wifi" (:572). Showing "(wifi)" on a virgin lock renders a default as a
  // decision, which is how a factory-reset lock looked like a misconfigured one.
  // The app decides the mode, at provision time, by sending network_key or not.
  gfx->print(!provisioned            ? "(mode: set by app)"
             : isThread()            ? "(thread)"
             : isLocalMode()         ? "(hotel)"
                                     : "(wifi)");
  if (isLocalMode() && cfgRoomNo.length()) { // room lives in the header now
    gfx->print(" P.");
    gfx->print(cfgRoomNo);
  }
  // XF-52 (R): while the maintenance window is open, say so instead of showing
  // the reset hint. Pressing a button and getting no visible response is how a
  // user concludes it didn't work and holds it longer — which is the factory
  // reset. The countdown is the whole point: it tells them how long they have.
  gfx->setCursor(220, 8);
  if (bleWindowOpen()) {
    gfx->setTextColor(C_GREEN);
    gfx->printf("BLE open %lus", (bleWindowUntil - millis()) / 1000);
  } else {
    gfx->setTextColor(C_DIM);
    gfx->print("reset: * then 5");
  }

  // DOOR STATUS — compact block (operator 2026-07-19: smaller status fonts,
  // bigger white text lines — size-1 grey was unreadable on this panel)
  gfx->fillRoundRect(15, 24, 150, 34, 8, open ? C_GREEN : C_RED);
  gfx->setTextSize(2);
  gfx->setTextColor(open ? C_BLACK : C_WHITE);
  gfx->setCursor(open ? 42 : 54, 34); // centered in the 150px block
  gfx->print(open ? "UNLOCKED" : "LOCKED");
  // MCU link tag beside the block — door state is only as fresh as the link
  gfx->setTextSize(2);
  gfx->setCursor(185, 34);
  gfx->setTextColor(C_WHITE);
  gfx->print("MCU ");
  gfx->setTextColor(mcuUp ? C_GREEN : C_RED);
  gfx->print(mcuUp ? "UP" : "DOWN");

  // NETWORK + IP + log lines — size 2, white on black
  gfx->setTextSize(2);
  int y = 72;
  gfx->setCursor(15, y);
  gfx->setTextColor(C_WHITE);
  gfx->print(isThread() ? "Thread: " : "Net: ");
  gfx->setTextColor(netUp ? C_GREEN : C_RED);
  // Name the ROLE, not a boolean. "JOINED" told the operator the lock was
  // working when it was Leader of its own orphaned partition — and even when
  // genuinely attached, it implied reachable, which a commissioned Thread lock
  // is not (no uplink yet, and BLE stops advertising once provisioned).
  gfx->print(netUp ? (isThread() ? (liveRole == OT_ROLE_CHILD    ? "CHILD"
                                    : liveRole == OT_ROLE_ROUTER ? "ROUTER"
                                                                 : "LEADER!")
                                 : "ONLINE")
                   : (isThread() ? "DOWN" : "OFFLINE"));
  gfx->setTextColor(C_WHITE);
  if (!isThread()) gfx->print(mqtt.connected() ? " MQTT OK" : " MQTT --");
  y += 24;
  gfx->setCursor(15, y);
  if (isThread()) {
    gfx->print("Uplink: not built yet"); // ozkey-10 §5 — honest, not silent
  } else {
    gfx->print("IP : ");
    gfx->print(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("---"));
  }
  y += 24;
  gfx->setCursor(15, y);
  // M2: ownership, not the txlog count. Replaced "Log: N events" 2026-08-01 —
  // the event count was never actionable at the door, whereas "who owns this
  // lock" is the single fact an installer needs and cannot otherwise see: it
  // decides whether a failed provision is a bug or a correctly-refused
  // BOND_DENIED. First 8 hex of bond #0 is enough to eyeball against the app.
  gfx->print("Owner: ");
  if (g_bond0Present) {
    gfx->setTextColor(C_GREEN);
    gfx->print(ozBond0PubHex().substring(0, 8));
  } else {
    // Amber, not red: unowned is a normal pre-commissioning state, not a fault.
    gfx->setTextColor(C_AMBER);
    gfx->print("none");
  }
  gfx->setTextColor(C_WHITE);
}

void drawFlash(const char *msg, uint16_t bg, uint16_t fg) {
  gfx->fillScreen(bg);
  gfx->setTextSize(4);
  gfx->setTextColor(fg);
  int16_t x = 160 - (int)strlen(msg) * 12;
  gfx->setCursor(x > 0 ? x : 4, 70);
  gfx->println(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// NVS (no "creds" namespace — the MCU owns credentials now)
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
  cfgHeartbeatS = clampHeartbeatS(prefs.getUInt("hb", 60));
  cfgMode = prefs.getString("mode", "ozkey-cloud");
  cfgRoomNo = prefs.getString("room", "");
  cfgMacToken = prefs.getString("mtoken", "");
  wakeSim = prefs.getBool("wksim", true);
  cfgTransport = prefs.getString("xport", "wifi"); // additive field — old
                                                    // rows with no "xport"
                                                    // key correctly default
                                                    // to "wifi" (unchanged
                                                    // behavior for existing
                                                    // blecomm-provisioned boards)
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
  prefs.putString("mode", cfgMode);
  prefs.putString("room", cfgRoomNo);
  prefs.putString("mtoken", cfgMacToken);
  prefs.putBool("wksim", wakeSim);
  prefs.putString("xport", cfgTransport);
  prefs.end();
}

void factoryReset() {
  Serial.println("[RESET] factory reset — wiping NVS + txlog");
  // M2: prefs.clear() wipes the "blelock" namespace, which holds BOTH the
  // ceremony keypair (xpriv/xpub) and bond #0 (b0pub/b0role/b0ctr). So a factory
  // reset is the ONLY way to clear ownership (CONTRACT.md), and it necessarily
  // mints a NEW identity — info.pub changes. That coupling is deliberate: an
  // owner who resets a lock must not inherit the previous owner's identity, and
  // a new owner must not be able to reuse a captured pairing secret.
  prefs.begin("blelock", false); prefs.clear(); prefs.end();

  // ORDER FIX (2026-08-02, bench): everything destructive must happen BEFORE the
  // OpenThread reset below, because otInstanceFactoryReset() performs a PLATFORM
  // RESET and never returns. The old order left the txlog wipe, the "RESET"
  // screen flash and ESP.restart() all downstream of it — dead code on any
  // Thread-transport lock, which is the primary topology.
  //
  // Measured: "[RESET] factory reset" to ESP-ROM banner was 34 ms, against a
  // coded delay(800) further down. That gap is the proof.
  //
  // It mattered because txlog is the DOOR EVENT buffer, and a factory reset is
  // the ownership transfer (XF-46 §1 sold-house semantics) — so the previous
  // owner's access history was surviving onto the next owner's lock.
  if (fsUp) {
    LittleFS.remove("/txlog.0");
    LittleFS.remove("/txlog.1");
    txlogCount0 = txlogCount1 = 0;
  }
  drawFlash("RESET", C_AMBER, C_BLACK);
  delay(800);
  // Thread's persisted dataset lives in OpenThread's own NVS storage, not
  // our "blelock" namespace above — a plain NVS wipe alone would leave a
  // Thread-transport board rejoining the same old mesh on reboot. Same fix
  // as threadcomm.ino (2026-07-26): otInstanceFactoryReset() erases it
  // properly (lock-guarded; esp_openthread_lock.h is transitively available
  // via <OThread.h>).
  if (isThread()) {
    otInstance *inst = thread.getInstance();
    if (inst != nullptr && esp_openthread_lock_acquire(portMAX_DELAY)) {
      otInstanceFactoryReset(inst); // erases OT persistent info + platform reset
      esp_openthread_lock_release(); // unreachable if the reset already fired
    }
  }
  // Reached only on a Wi-Fi-transport lock: the isThread() branch above has
  // already reset the platform and never returned. Kept so the Wi-Fi path still
  // reboots — do NOT put anything else after this line for the same reason the
  // block above was moved.
  ESP.restart();
}

// Hardware escape hatch (2026-07-26, added alongside the touch gesture
// above): touch depends on the CST816-class controller/I2C wiring working
// at all — if that's ever flaky or dead on a given unit, the '*'-then-'5'
// ceremony is unreachable. BOOT-hold is independent of touch entirely, same
// mechanism as bridge32.ino/threadcomm.ino.
#ifndef USER_BUTTON
#define USER_BUTTON BOOT_PIN
#endif
#define FACTORY_RESET_HOLD_MS 5000UL
unsigned long buttonHeldSince = 0;
bool buttonWasDown = false;

// ─────────────────────────────────────────────────────────────────────────────
// XF-52 (R): the BLE maintenance window.
//
// A commissioned lock does NOT advertise — startBle() runs only when
// unprovisioned or after a network failure. That is deliberate (it is why a
// deployed lock is invisible to a passer-by, XF-52 §1) but it made two things
// impossible: re-provisioning your own lock, and M3's member_enroll, which is a
// BLE write on a working lock.
//
// So: a SHORT BOOT press opens a bounded advertising window. Deliberately
// physical — a remotely-triggerable window would hand back at scale exactly the
// exposure the closed radio prevents (XF-52 §4), so there is no MQTT/DPID verb
// for this and there must never be one.
//
// The 5 s hold is already factory reset, so short-press is free and unambiguous:
// press and release = window, press and keep holding = wipe. The countdown
// prints either way, so a user who overshoots sees it coming.
#define BLE_WINDOW_MS 60000UL
#define BUTTON_DEBOUNCE_MS 60UL
// bleWindowUntil is declared with the other BLE globals near the top — see the
// note there about drawStatus() needing it before this point.

void startBle(); // defined with the GATT setup, further down

bool bleWindowOpen() { return bleWindowUntil && (long)(millis() - bleWindowUntil) < 0; }

void openBleWindow() {
  bleWindowUntil = millis() + BLE_WINDOW_MS;
  if (bleServer == nullptr) startBle(); // first open: build the GATT server
  else BLEDevice::startAdvertising();   // already built: just go discoverable
  Serial.printf("[BLE] window OPEN %lus (short BOOT press)\n", BLE_WINDOW_MS / 1000);
  screenDirty = true;
}

void closeBleWindow(const char *why) {
  bleWindowUntil = 0;
  BLEDevice::stopAdvertising();
  Serial.printf("[BLE] window closed (%s)\n", why);
  screenDirty = true;
}

void checkFactoryResetButton() {
  bool down = digitalRead(USER_BUTTON) == LOW;
  if (down && !buttonWasDown) {
    buttonHeldSince = millis();
  } else if (down && buttonWasDown) {
    unsigned long held = millis() - buttonHeldSince;
    if (held >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[RESET] BOOT held 5s — factory reset");
      factoryReset(); // does not return
    } else if (held > 800 && (held / 500) % 2 == 0) {
      Serial.printf("[RESET] holding BOOT... %lus/5s\n", held / 1000);
    }
  } else if (!down && buttonWasDown) {
    // Released. A short press opens the window; a long one already wiped the
    // device and never got here.
    unsigned long held = millis() - buttonHeldSince;
    if (held >= BUTTON_DEBOUNCE_MS && held < FACTORY_RESET_HOLD_MS) openBleWindow();
  }
  buttonWasDown = down;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tuya MCU wire (Serial1 → LockSim Mode B). ⚠ RAW BYTES, never spaced-hex —
// LockSim's extractFrames() scans for the contiguous 0x55 0xAA header.
// ─────────────────────────────────────────────────────────────────────────────
void tuyaWireSend(const uint8_t *f, size_t n) {
  // §0.2 module-initiated send: raise MRDY, wait for the MCU's answering
  // SRDY (wake_sim: assumed answered), then transmit — no bytes ever hit a
  // sleeping UART.
  mrdySet(true);
  if (!srdyAsserted()) {
    unsigned long t0 = millis();
    while (!srdyAsserted() && millis() - t0 < SRDY_WAIT_TIMEOUT_MS) delay(5);
    if (!srdyAsserted())
      Serial.println("[WAKE] SRDY no answer in 1.5s — transmitting anyway");
  }
  Serial1.write(f, n);
  lastWireActivityAt = millis();
  mcuTxFrames++;
  String hex; hex.reserve(n * 3);
  for (size_t i = 0; i < n; i++) {
    char b[4]; snprintf(b, sizeof(b), "%02X ", f[i]); hex += b;
  }
  Serial.printf("[TUYA->] %s\n", hex.c_str());
  screenDirty = true; // bus counters live on the dashboard
}

// Short human line for the console + dashboard ("what did the MCU say?")
String describeDpid(const uint8_t *f, size_t n) {
  if (n >= 4 && f[3] == 0x00) return String("MCU heartbeat");
  if (n < 11 || f[3] != 0x06) return String("cmd 0x") + String(f[3], HEX);
  uint8_t dpid = f[6], type = f[7];
  uint16_t vlen = ((uint16_t)f[8] << 8) | f[9];
  const uint8_t *v = f + 10;
  if (dpid == 8 && type == 0x04 && vlen >= 1) {
    const char *r = v[0] == 0 ? "SUCCESS" : v[0] == 1 ? "DENIED" : v[0] == 2 ? "EXPIRED" : "?";
    return String("ACCESS_RESULT ") + r;
  }
  if (dpid == 1) return String("unlock channel report");
  if (dpid == 5) return String("battery alarm");
  return String("DP ") + dpid + " type " + type + " len " + vlen;
}

// MCU → server translation: the module's actual job. ACCESS_RESULT becomes
// the door log the servers already understand; heartbeats prove the link;
// anything else goes up raw as dp_report so nothing is silently dropped.
void handleMcuFrame(const uint8_t *f, size_t n) {
  // checksum gate (same rule both directions)
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += f[i];
  if (sum != f[n - 1]) { Serial.println("[TUYA<-] bad checksum — dropped"); return; }

  mcuRxFrames++;
  lastMcuFrameAt = millis();
  lastActivityAt = millis();
  lastMcuSummary = describeDpid(f, n);
  Serial.printf("[TUYA<-] %s (%u bytes)\n", lastMcuSummary.c_str(), (unsigned)n);
  screenDirty = true;

  if (n >= 4 && f[3] == 0x00) return; // MCU heartbeat = link-alive only

  if (n >= 11 && f[3] == 0x06) {
    uint8_t dpid = f[6], type = f[7];
    uint16_t vlen = ((uint16_t)f[8] << 8) | f[9];
    const uint8_t *v = f + 10;
    if (dpid == 8 && type == 0x04 && vlen >= 1) { // ACCESS_RESULT → door log
      const char *result = v[0] == 0 ? "granted" : v[0] == 1 ? "denied" : "expired";
      if (v[0] == 0) markDoorUnlocked(); // mirror the bolt for the dashboard
      publishLog(result, "MCU report");
      return;
    }
    if (dpid == 5) { publishLog("battery_alarm", "MCU report"); return; }
  }
  // unrecognised — forward raw hex upstream rather than dropping
  String hex; hex.reserve(n * 3);
  for (size_t i = 0; i < n; i++) {
    char b[4]; snprintf(b, sizeof(b), "%02X ", f[i]); hex += b;
  }
  hex.trim();
  publishLog("dp_report", hex.c_str());
}

// RX reassembly off the wire (LockSim frames arrive as raw bytes)
void tuyaWirePump() {
  static uint8_t buf[128];
  static size_t bn = 0;
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    lastWireActivityAt = millis();
    mrdySet(true); // MCU is talking → answer its SRDY per the §0.2 handshake
    if (bn == 0 && b != 0x55) continue;
    if (bn == 1 && b != 0xAA) { bn = 0; if (b == 0x55) bn = 1; continue; }
    if (bn < sizeof(buf)) buf[bn++] = b; else { bn = 0; continue; }
    if (bn >= 7) {
      uint16_t plen = ((uint16_t)buf[4] << 8) | buf[5];
      size_t total = 6 + plen + 1;
      if (total > sizeof(buf)) { bn = 0; continue; }
      if (bn == total) { handleMcuFrame(buf, bn); bn = 0; }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT wire (blelock-identical topics; ozlockserv/ozkeyserv untouched)
// ─────────────────────────────────────────────────────────────────────────────
void publishLog(const char *result, const char *detail) {
  txlogAppend(result, detail); // transaction buffer first — works offline
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

void publishUnpairedAnnounce() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  String out; serializeJson(doc, out);
  mqtt.publish(TOPIC_UNPAIRED, out.c_str());
  lastUnpairedAnnounce = millis();
  Serial.println("[PAIR->] unpaired announce (waiting for room assign)");
}

// Shared by both transports (2026-07-26 unification): a command frame is a
// command frame regardless of whether it arrived via MQTT payload_hex
// (Wi-Fi) or the F4 Thread UDP relay's "payload" field — parse the hex,
// forward to the MCU, mirror a remote-unlock locally. Spaced or bare hex
// both accepted (OZKEYSERV publishes spaced; bridge32's F4 envelope is bare).
void forwardHexToMcu(const String &hex) {
  static uint8_t frame[256];
  size_t fn = 0;
  int hi = -1;
  for (size_t i = 0; i < hex.length() && fn < sizeof(frame); i++) {
    char c = hex[i];
    if (c == ' ' || c == ':') continue;
    int v = hexNibble(c);
    if (v < 0) { Serial.println("[TUYA] bad hex in command frame"); return; }
    if (hi < 0) {
      hi = v;
    } else {
      frame[fn++] = (hi << 4) | v;
      hi = -1;
    }
  }
  if (fn < 4) return;
  Serial.printf("[FWD] server cmd -> MCU: %s\n", describeDpid(frame, fn).c_str());
  tuyaWireSend(frame, fn);
  // remote unlock (DP 1 BOOL 01): LockSim unlocks on receipt — mirror it
  if (fn >= 11 && frame[3] == 0x06 && frame[6] == 1 && frame[7] == 0x01 &&
      frame[10] == 0x01)
    markDoorUnlocked();
}

// ─────────────────────────────────────────────────────────────────────────────
// F4 UDP relay, receive half (ported from threadcomm.ino, bench-proven
// 2026-07-25/26). Every lock on the mesh gets every datagram from bridge32
// and filters by its own device_id in "target" — same v0 addressing
// simplification threadcomm.ino used (no discovery mechanism exists yet,
// ozkey-10 §7 Q3).
// ─────────────────────────────────────────────────────────────────────────────
// ── RAW OpenThread UDP receive (2026-07-28) ─────────────────────────────────
// Replaces OThreadUDP for the receive half. OThreadUDP::begin() binds with
//     otUdpBind(inst, &sock, &sa, OT_NETIF_THREAD_INTERNAL)
// and on arduino-esp32 3.3.11 no inbound multicast ever matched that socket.
// Bench-proven 2026-07-28: bridge32 transmitted the SAME datagram to both
// ff03::4f5a and ff03::1 (realm-local all-nodes, which every Thread node joins
// automatically) while this board was a confirmed attached Child — live role
// from otGetStringDeviceRole(), not the latched threadFormed flag — with the
// socket reporting open and subscribed. parsePacket() never returned a single
// packet. Attach, subscribe and transmit were all verified working; only
// receive was dead, which points squarely at the netif filter.
// OT_NETIF_UNSPECIFIED (= 0) disables netif filtering altogether, which is what
// a Thread-only node actually wants. Note this OpenThread has FOUR netif values
// (UNSPECIFIED / THREAD_HOST / THREAD_INTERNAL / BACKBONE) where older versions
// had three, so the identifier numbering shifted under the wrapper.
// ROOT CAUSE (2026-07-28, found by reading the installed core): OThread.cpp
// :255/:271 creates an esp_netif for OpenThread and attaches
// esp_openthread_netif_glue. With CONFIG_ESP_NETIF_TCPIP_LWIP=y, inbound
// Thread packets are pushed UP into lwIP by that glue and are NEVER delivered
// to OpenThread's own otUdp* socket list. So OThreadUDP's whole receive half —
// and our raw otUdpOpen port of it — can never fire on ESP32, whichever netif
// identifier is bound. Sending is unaffected, because otUdpSend goes DOWN
// through OpenThread to the radio; that is why bridge32 worked throughout.
// Bench-proven 2026-07-28: node a confirmed attached Child, both ff03::4f5a
// and ff03::1 joined at stack level, socket bound with netif filtering off —
// and NOTHING arrived, not even a unicast datagram to this node's own
// mesh-local EID. Unicast failing is what ruled out every addressing and
// link-mode theory and pointed at the stack boundary.
// Fix: listen where the packets actually land — a normal lwIP AF_INET6 socket.
static int ozRxFd = -1;

void threadUdpBegin() {
  if (threadUdpReady) return;
  threadUdpLastAttempt = millis();

  otInstance *inst = thread.getInstance();
  if (inst == nullptr) {
    Serial.println("[UDP] no OpenThread instance yet — will retry");
    return;
  }
  // Same lock discipline as factoryReset() above; bounded wait so a busy OT
  // task costs us a retry rather than blocking loop() forever.
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
    Serial.println("[UDP] OpenThread lock busy — will retry");
    return;
  }
  bool ok = false;
  do {
    otError e = OT_ERROR_NONE;
    otIp6Address grp;
    memcpy(grp.mFields.m8, OZ_THREAD_GROUP_BYTES, 16);
    e = otIp6SubscribeMulticastAddress(inst, &grp);
    if (e != OT_ERROR_NONE && e != OT_ERROR_ALREADY) {
      // Non-fatal: ff03::1 still reaches us, so the relay can be re-pointed
      // at all-nodes if our custom group turns out to be the problem.
      Serial.printf("[UDP] subscribe ff03::4f5a failed: %d (continuing)\n", (int)e);
    }

    // GROUND TRUTH (2026-07-28): is this node actually rx-on-when-idle?
    // A SLEEPY child attaches, joins multicast groups and binds a socket
    // exactly like an rx-on one — but it only ever polls its parent for
    // UNICAST and never hears the link-layer broadcast that carries
    // realm-local multicast. That fits every symptom we have, and it is the
    // one property never read back (rx-on was ASSUMED from
    // CONFIG_OPENTHREAD_FTD=y, never verified). Raw C calls only in here —
    // the OThread wrapper methods take this same lock and would deadlock.
    otLinkModeConfig lm = otThreadGetLinkMode(inst);
    Serial.printf("[THREAD] linkmode rx_on=%d ftd=%d netdata=%d\n",
                  (int)lm.mRxOnWhenIdle, (int)lm.mDeviceType, (int)lm.mNetworkData);
    if (!lm.mRxOnWhenIdle) {
      lm.mRxOnWhenIdle = true;
      otError le = otThreadSetLinkMode(inst, lm);
      Serial.printf("[THREAD] was SLEEPY — forced rx-on-when-idle -> %d\n", (int)le);
    }
    ok = true;
  } while (false);
  esp_openthread_lock_release();

  // ── the actual listener: lwIP AF_INET6 datagram socket ────────────────────
  if (ok) {
    ozRxFd = lwip_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (ozRxFd < 0) {
      Serial.printf("[UDP] lwip socket() failed errno=%d\n", errno);
      ok = false;
    }
  }
  if (ok) {
    int on = 1;
    lwip_setsockopt(ozRxFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in6 sa6;
    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(OZ_THREAD_UDP_PORT);
    // in6addr_any: accept unicast to any of our addresses AND any joined group
    if (lwip_bind(ozRxFd, (struct sockaddr *)&sa6, sizeof(sa6)) != 0) {
      Serial.printf("[UDP] lwip bind() failed errno=%d\n", errno);
      lwip_close(ozRxFd);
      ozRxFd = -1;
      ok = false;
    }
  }
  if (ok) {
    // Non-blocking so pollThreadUdp() never stalls loop().
    int fl = lwip_fcntl(ozRxFd, F_GETFL, 0);
    lwip_fcntl(ozRxFd, F_SETFL, fl | O_NONBLOCK);

    // Join the group at the lwIP layer too. OpenThread's own subscription
    // (above) makes the stack accept the frame off the radio; this makes lwIP
    // deliver it to THIS socket. Interface 0 = default/any, which is correct
    // here since Thread is the only IPv6 netif on a Thread-transport lock.
    struct ipv6_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.ipv6mr_multiaddr, OZ_THREAD_GROUP_BYTES, 16);
    mreq.ipv6mr_interface = 0;
    if (lwip_setsockopt(ozRxFd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
      // Non-fatal: unicast still works, and that alone proves the stack theory.
      Serial.printf("[UDP] lwip IPV6_JOIN_GROUP failed errno=%d (unicast still live)\n", errno);
    }
  }

  threadUdpReady = ok;
  Serial.printf("[UDP] lwip socket %s fd=%d on port %u (group ff03::4f5a)\n",
                ok ? "open" : "FAILED", ozRxFd, OZ_THREAD_UDP_PORT);

  // GROUND TRUTH (2026-07-28): subscribe returning OT_ERROR_NONE is NOT proof
  // the group is live on the interface, and every hypothesis so far died on an
  // assumption we never actually read back. Dump what the IPv6 stack really
  // holds. Expect ff03::4f5a here, plus the auto-joined ff02::1 / ff03::1 /
  // ff03::fc — if ff03::4f5a is absent the subscription silently failed; if
  // ff03::1 is absent too, this node is not receiving realm-local multicast at
  // all and the fault is below UDP entirely.
  for (const auto &a : thread.getAllMulticastAddresses()) {
    Serial.printf("[UDP] mcast joined: %s\n", a.toString().c_str());
  }
  for (const auto &a : thread.getAllUnicastAddresses()) {
    Serial.printf("[UDP] unicast addr : %s\n", a.toString().c_str());
  }
}

void pollThreadUdp() {
  if (!threadUdpReady || ozRxFd < 0) return;

  char buf[OZ_UDP_RX_BUF];
  struct sockaddr_in6 src;
  socklen_t srcLen = sizeof(src);
  int n = lwip_recvfrom(ozRxFd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &srcLen);
  if (n <= 0) return; // EWOULDBLOCK on an idle socket
  buf[n] = '\0';

  // DIAGNOSTIC (2026-07-28 bench): log EVERY datagram before any filtering.
  // The target filter below used to return silently, which made "no packet
  // ever arrived" and "packet arrived but wasn't addressed to us" look
  // identical on the serial console — the single blind spot that stalled the
  // first end-to-end relay test (ozkey-11 §4.1).
  Serial.printf("[UDP] rx %d bytes: %s\n", n, buf);

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    Serial.println("[UDP] payload not valid JSON, dropped");
    return;
  }
  String target = (const char *)(doc["target"] | "");
  String payloadHex = (const char *)(doc["payload"] | "");
  if (target != deviceId) { // not for us
    Serial.printf("[UDP] not for us (target='%s' me='%s')\n", target.c_str(), deviceId.c_str());
    return;
  }
  Serial.printf("[UDP] << target=%s payload=%s\n", target.c_str(), payloadHex.c_str());
  lastActivityAt = millis();
  forwardHexToMcu(payloadHex);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String body; body.reserve(length);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  Serial.printf("[MQTT<-] %s %s\n", topic, body.c_str());
  lastActivityAt = millis();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  const char *op = doc["op"] | (const char *)nullptr;
  if (op && (strcmp(op, "factory_reset") == 0 || strcmp(op, "unpair") == 0)) {
    Serial.println("[MQTT<-] factory_reset (unpaired by app/server)");
    factoryReset();
    return;
  }
  if (op && strcmp(op, "wake_sim") == 0) { // §0.2 bench toggle, NVS-persisted
    wakeSim = doc["on"] | true;
    prefs.begin("blelock", false);
    prefs.putBool("wksim", wakeSim);
    prefs.end();
    Serial.printf("[WAKE] wake_sim %s (server toggle)\n", wakeSim ? "ON" : "OFF");
    screenDirty = true;
    return;
  }
  if (op && strcmp(op, "provision_assign") == 0) {
    String amac = doc["mac"] | "";
    amac.replace(":", ""); amac.toLowerCase();
    if (amac.length() && ("ozk-" + amac) != deviceId) return; // not ours
    cfgRoomNo = String((const char *)(doc["room_no"] | ""));
    cfgSiteId = (const char *)(doc["site_id"] | "hotel");
    cfgMacToken = (const char *)(doc["mac_token"] | "");
    if (cfgRoomNo.length()) cfgName = "P." + cfgRoomNo;
    enrolled = true;
    saveConfig();
    buildTopics();
    mqtt.subscribe(topicCommand.c_str(), 1);
    Serial.printf("[PAIR] assigned room %s (site %s)\n", cfgRoomNo.c_str(),
                  cfgSiteId.c_str());
    notifyStatus("ENROLLED");
    state = ST_OPERATIONAL;
    screenDirty = true;
    publishHeartbeat();
    return;
  }
  if (op && strcmp(op, "enrollment_ack") == 0) {
    enrolled = true;
    const char *label = doc["label"] | "";
    if (!cfgName.length() && strlen(label)) cfgName = label;
    if (doc["heartbeat_s"].is<uint32_t>())
      cfgHeartbeatS = clampHeartbeatS(doc["heartbeat_s"].as<uint32_t>());
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
  // Command envelope {action, grant_id, payload_hex}: PURE FORWARD to the
  // MCU — the comm module never executes credentials.
  const char *hex = doc["payload_hex"] | (const char *)nullptr;
  if (hex) forwardHexToMcu(String(hex));
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
    lastActivityAt = millis();
    mqtt.subscribe(topicCommand.c_str(), 1);
    if (isLocalMode() && !enrolled) mqtt.subscribe(topicPairConfirm.c_str(), 1);
    Serial.println("[MQTT] connected + subscribed command topic");
    if (state == ST_JOINING) {
      notifyStatus("BROKER_OK");
      if (isLocalMode()) {
        joinLine2 = "Cho MAOI gan phong...";
        screenDirty = true;
        publishUnpairedAnnounce();
      } else {
        joinLine2 = "Server: OK - enrolling...";
        screenDirty = true;
        enrollAttempts = 0;
        publishEnroll();
      }
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
// Thread dataset hex parsing (ported verbatim from threadcomm.ino)
// ─────────────────────────────────────────────────────────────────────────────
bool hexToBytes(const String &hex, uint8_t *out, size_t expectLen) {
  if ((size_t)hex.length() != expectLen * 2) return false;
  for (size_t i = 0; i < expectLen; i++) {
    int h = hexNibble(hex[i * 2]), l = hexNibble(hex[i * 2 + 1]);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Provisioning (BLE write → JOINING). Transport-agnostic prefix (mode,
// device_id, and the common broker/site/name/heartbeat fields) shared by
// both paths; branches on payload shape at the bottom (ozkey-10 §4).
// ─────────────────────────────────────────────────────────────────────────────
// M2: emit the bond #0 outcome at the accept point. CONTRACT.md places BOND_OK
// INSIDE the commissioning ladder — after provision-accepted, before
// WIFI_JOINING / THREAD_JOINING — so the app sees ownership settle before it
// starts waiting on a network join that can take a minute.
//
// BOND_OK is optional by contract: its absence means pre-bond firmware and the
// app falls back to `v1-bench`. That is why OZ_BOND_ABSENT emits nothing at all
// rather than an error — a legacy payload with no app_id is still a valid
// provision.
static void bond0Accept(OzBondVerdict v, const uint8_t provPub[32]) {
  if (v == OZ_BOND_CREATE) {
    ozBond0Commit(provPub);
    Serial.printf("[BOND] bond #0 CREATED role=admin floor=0 pub=%s\n",
                  ozBond0PubHex().c_str());
    screenDirty = true; // the Owner row just changed from "none" to a pubkey
    notifyStatus("BOND_OK");
  } else if (v == OZ_BOND_SAME) {
    // Idempotent: same owner re-provisioning to move broker/Wi-Fi. Bond and
    // counter_floor are left exactly as they were — re-minting would reset the
    // floor to 0 and re-open every captured frame.
    Serial.println("[BOND] bond #0 unchanged (same app_id) — idempotent re-provision");
    notifyStatus("BOND_OK");
  }
}

void applyProvision(JsonDocument &doc) {
  String pid = doc["device_id"] | "";
  if (pid.length() && pid != deviceId) {
    Serial.printf("[PROV] device_id mismatch (%s != %s)\n", pid.c_str(), deviceId.c_str());
    notifyStatus("ENROLL_FAIL");
    return;
  }

  // ── M2: bond #0 verdict — decided HERE, before ANY state is touched ───────
  //
  // `app_id` is read ONCE, ABOVE the transport branch. XF-47 §11.5: it used to
  // exist only on the Wi-Fi ProvisionPayload, so parsing it inside the Wi-Fi arm
  // would have given Thread locks no bond #0 at all — silently removing the
  // whole XF-46 member model from the platform's PRIMARY topology, and it would
  // not have surfaced until M3 bring-up. Same key, same meaning, both arms.
  uint8_t provPub[32];
  const char *appIdHex = (const char *)(doc["app_id"] | "");
  const OzBondVerdict bondVerdict = ozBond0Evaluate(appIdHex, provPub);

  if (bondVerdict == OZ_BOND_MALFORMED) {
    Serial.println("[BOND] app_id present but not 64 hex chars — payload rejected");
    notifyStatus("PAYLOAD_REJECTED");
    return;
  }
  if (bondVerdict == OZ_BOND_DENIED) {
    // ATOMIC REFUSAL — and note WHERE this return sits: above every cfg*
    // assignment, saveConfig(), WiFi.disconnect(), and the Thread
    // commitDataSet(). Nothing has been mutated, in RAM or NVS.
    //
    // CONTRACT.md: "the refusal is atomic — a rejected re-provision must not
    // change WiFi/broker either, else an attacker who cannot steal the lock can
    // still repoint it." Ownership theft was the original vector: info.pub
    // survives re-provision, so anyone inside the ~60 s touch window could
    // re-provision a commissioned lock with their own app_id and become bond #0.
    // Only a factory reset clears bond #0.
    Serial.printf("[BOND] DENIED — re-provision with a different app_id (owner=%s)\n",
                  ozBond0PubHex().c_str());
    notifyStatus("BOND_DENIED");
    return;
  }

  // BUG FIX (2026-07-26, live bench): `mode` used to be required as the
  // very first check, unconditionally. But the app's Thread-dataset
  // payload (ThreadDataset.encodeLockProvision(), unchanged since it was
  // built for the old threadcomm.ino-only binary) never included `mode` at
  // all — threadcomm.ino's own applyProvision() never checked for it. So
  // every real Thread provisioning attempt was rejected with ENROLL_FAIL
  // before this function even looked at whether it was a Thread payload.
  // `mode` is only actually CONSUMED by the Wi-Fi path today (no
  // broker/uplink exists for Thread yet — ozkey-10 §5), so it's now
  // validated strictly there and soft-defaulted here.
  String mode = doc["mode"] | "";

  cfgBrokerHost = (const char *)(doc["broker_host"] | "");
  cfgBrokerPort = doc["broker_tcp_port"] | 1883;
  cfgServerIp = (const char *)(doc["server_ip"] | "");
  cfgServerPort = doc["server_port"] | 4200;
  cfgSiteId = (const char *)(doc["site_id"] | "lab");
  cfgName = (const char *)(doc["name"] | "");
  cfgHeartbeatS = clampHeartbeatS(doc["heartbeat_s"] | 60);

  // Transport discriminator (ozkey-10 §4): same rule threadcomm.ino already
  // used as a separate binary — network_key present -> Thread dataset;
  // ssid present -> Wi-Fi credentials. Checked as strings (not just
  // truthiness) so an empty/missing field doesn't accidentally match.
  bool hasNetworkKey = doc["network_key"].is<const char *>() &&
                       String((const char *)doc["network_key"]).length();
  bool hasSsid = doc["ssid"].is<const char *>() &&
                 String((const char *)doc["ssid"]).length();

  if (hasNetworkKey) {
    // ── Thread transport (ported from threadcomm.ino) ─────────────────────
    // mode isn't sent by today's Thread-dataset payload and isn't consumed
    // by anything in this transport yet — default rather than reject.
    if (mode != "ozkey-cloud" && mode != "ozkey-local") mode = "ozkey-cloud";
    String tName = (const char *)(doc["network_name"] | "");
    String extPanHex = (const char *)(doc["ext_pan_id"] | "");
    String keyHex = (const char *)(doc["network_key"] | "");
    String panHex = (const char *)(doc["pan_id"] | "");
    int channel = doc["channel"] | 0;

    uint8_t extPanId[8], networkKey[16];
    Serial.printf("[PROV] thread fields: name='%s'(%u) ext_pan='%s'(%u) key_len=%u ch=%d pan='%s'(%u)\n",
                  tName.c_str(), (unsigned)tName.length(), extPanHex.c_str(),
                  (unsigned)extPanHex.length(), (unsigned)keyHex.length(), channel,
                  panHex.c_str(), (unsigned)panHex.length());
    if (!tName.length() || channel < 11 || channel > 26 ||
        !hexToBytes(extPanHex, extPanId, 8) || !hexToBytes(keyHex, networkKey, 16) ||
        panHex.length() != 4) {
      Serial.println("[PROV] malformed Thread dataset payload");
      notifyStatus("ENROLL_FAIL");
      return;
    }
    uint16_t panId = (uint16_t)strtoul(panHex.c_str(), nullptr, 16);

    otDataset.initNew();
    otDataset.setNetworkName(tName.c_str());
    otDataset.setExtendedPanId(extPanId);
    otDataset.setNetworkKey(networkKey);
    otDataset.setChannel((uint8_t)channel);
    otDataset.setPanId(panId);
    thread.commitDataSet(otDataset);
    // A new dataset means the role we currently report belongs to the network we
    // are about to leave — do not believe it until the stack has detached.
    threadAwaitDetach = true;

    cfgMode = mode;
    cfgTransport = "thread";
    cfgRoomNo = "";
    cfgMacToken = "";
    provisioned = true;
    enrolled = false; // no enrollment concept over Thread yet — see header comment
    saveConfig();
    buildTopics();
    Serial.printf("[PROV] mode=%s site=%s transport=thread name=%s ch=%d\n",
                  cfgMode.c_str(), cfgSiteId.c_str(), tName.c_str(), channel);

    bond0Accept(bondVerdict, provPub); // BOND_OK before THREAD_JOINING

    threadJoinStart = millis();
    state = ST_JOINING;
    joinLine1 = "Thread: joining " + tName + "...";
    joinLine2 = "(no uplink yet — ozkey-10 gap)";
    screenDirty = true;
    notifyStatus("THREAD_JOINING");
    OpenThread::begin(false);
    // ORDER FIX (2026-07-27, live bench bridge32 finding): otThreadSetEnabled()
    // (thread.start()) requires otIp6SetEnabled() (networkInterfaceUp()) to
    // already be up — OT_ERROR_INVALID_STATE otherwise (openthread/thread.h).
    // Calling start() first guarantees attach never happens.
    thread.networkInterfaceUp();
    thread.start();
    return;
  }

  if (!hasSsid) { notifyStatus("ENROLL_FAIL"); return; } // neither shape present

  // ── Wi-Fi transport (blecomm's original path, unchanged) ─────────────────
  // mode IS strictly required here — this is the only path that actually
  // uses it (isLocalMode() gates MQTT enrollment vs hotel-announce below).
  if (mode != "ozkey-cloud" && mode != "ozkey-local") { notifyStatus("ENROLL_FAIL"); return; }
  cfgSsid = (const char *)(doc["ssid"] | "");
  cfgPass = (const char *)(doc["password"] | "");
  if (!cfgSsid.length() || !cfgBrokerHost.length()) { notifyStatus("ENROLL_FAIL"); return; }

  cfgMode = mode;
  cfgTransport = "wifi";
  cfgRoomNo = "";
  cfgMacToken = "";
  provisioned = true;
  enrolled = false;
  saveConfig();
  buildTopics();
  Serial.printf("[PROV] mode=%s site=%s broker=%s:%u -> %s\n", cfgMode.c_str(),
                cfgSiteId.c_str(), cfgBrokerHost.c_str(), cfgBrokerPort,
                isLocalMode() ? "HOTEL (announce+await room)"
                              : "OZLOCK (enroll)");

  // RACE FIX round 2 (2026-07-26, bridge32 bench finding): wifiJoinStart
  // must be fresh BEFORE state flips to ST_JOINING — loop() (main task) can
  // be scheduled between any two statements here (BLE callback task) and
  // would otherwise see the new state with the OLD wifiJoinStart (from a
  // join minutes ago), making millis()-wifiJoinStart instantly exceed the
  // timeout and fire a false "join timeout" before WiFi.begin() even ran.
  bond0Accept(bondVerdict, provPub); // BOND_OK before WIFI_JOINING

  wifiJoinStart = millis();
  state = ST_JOINING;
  joinLine1 = "WiFi: joining " + cfgSsid + "...";
  joinLine2 = "Server: " + cfgBrokerHost + ":" + String(cfgBrokerPort);
  screenDirty = true;
  notifyStatus("WIFI_JOINING");
  // Tell the broker we're leaving BEFORE yanking Wi-Fi, so a re-provision
  // doesn't leave a zombie session for the broker to reap on its own
  // keepalive timeout (live-bench finding, 2026-07-26, bridge32).
  if (mqtt.connected()) mqtt.disconnect();
  WiFi.disconnect(true); // clean slate — a re-provision may land mid a prior
                          // attempt; also forces a real status edge so the
                          // ws!=lastWifi detector below actually re-fires
                          // instead of silently staying WL_CONNECTED
  Serial.printf("[WiFi] begin ssid='%s' passlen=%u\n", cfgSsid.c_str(),
                cfgPass.length());
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

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleLinkCount++;
    bleClientConnected = (bleLinkCount > 0);
    Serial.printf("[BLE] connect  — links=%d\n", bleLinkCount);
    screenDirty = true;
    notifyStatus("BLE_OK");
  }
  void onDisconnect(BLEServer *) override {
    if (bleLinkCount > 0) bleLinkCount--;
    bleClientConnected = (bleLinkCount > 0);
    // XF-53 (Y): this event was completely silent until 2026-08-02, which is why
    // the T2 capture could not answer whether the link dropped — the evidence was
    // never emitted. If links>0 here, a stale teardown arrived while a live link
    // was up and the old code would have wrongly declared the app gone.
    Serial.printf("[BLE] disconnect — links=%d%s\n", bleLinkCount,
                  bleLinkCount > 0 ? "  (stale link closed, live link retained)" : "");
    screenDirty = true;
    delay(300);
    // Restart advertising only where we are SUPPOSED to be discoverable:
    //   - unprovisioned: the commissioning state, always advertise (this is the
    //     2026-07-26 bridge32 fix — gating on state==ST_ADVERTISING meant any
    //     client connecting after provisioning killed advertising for good);
    //   - inside an open XF-52 (R) window: a dropped link must not end the
    //     window early, or one flaky connection costs the user their 60 s.
    // Otherwise stay silent. Restarting unconditionally — which is what this did
    // until 2026-08-02 — would make one connection turn a bounded window into a
    // permanent one, quietly undoing the whole point of (R).
    if (!provisioned || bleWindowOpen()) {
      BLEDevice::startAdvertising();
    } else if (bleWindowUntil) {
      closeBleWindow("expired during session");
    }
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
  doc["pub"] = ozLockPubHex(); // X25519 ceremony pubkey (XF-46 §7.1)
  // BUG FIX (2026-07-26, caught during app-impact review): blecomm.ino never
  // reported "transport" since it was always Wi-Fi; threadcomm.ino hard-
  // coded "thread" since it was always Thread. The unified binary can be
  // EITHER depending on cfgTransport, and the app's existing auto-detect
  // (info.transport=='thread' -> Thread courier flow) depends on this field
  // existing — omitting it would silently make every unified lock look
  // like a Wi-Fi lock to the app, always. NOTE: for a never-provisioned
  // board this still reads "wifi" (the NVS default) — see the app-side
  // discussion this same field surfaced (fresh-commissioning transport
  // choice can't be auto-detected the way it could when two separate
  // firmwares existed).
  doc["transport"] = cfgTransport;
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
  topicPairConfirm = "hotel/locks/" + deviceId.substring(4) + "/pair/confirm";
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch — kept ONLY for the factory-reset ceremony ('*' zone then '5' zone,
// same invisible grid as blelock so the operator muscle-memory transfers).
// ─────────────────────────────────────────────────────────────────────────────
void touchInit() {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(100);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
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

int lastTapX = 0, lastTapY = 0;
uint8_t tapSamples = 0;

bool touchRead(int &tx, int &ty) {
  uint8_t buf[7];
  if (!touchReadRegs(buf)) return false;
  uint8_t count = buf[2];
  bool down = (count > 0 && count <= 5);
  if (down) {
    lastActivityAt = millis();
    if (touchWasDown) {
      int rawX = ((buf[3] & 0x0F) << 8) | buf[4];
      int rawY = ((buf[5] & 0x0F) << 8) | buf[6];
      lastTapX = 320 - rawY;
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
  touchWasDown = false;
  uint8_t n = tapSamples;
  tapSamples = 0;
  if (n == 0) return false;
  tx = lastTapX;
  ty = lastTapY;
  return true;
}

// blelock's keypad grid, hit-test only (nothing drawn): row 2 col 0 = '*',
// row 2 col 1 = '9' … we only care about '*' (bottom-left) and '5' (mid).
const int KP_Y = 12;
const int KP_ROW_H = 53;
const char KP_KEYS[3][4] = {
  {'1','2','3','4'},
  {'5','6','7','8'},
  {'*','9','0','#'},
};

char keyAt(int tx, int ty) {
  int r = ty <= KP_Y ? 0 : (ty - KP_Y) / KP_ROW_H;
  if (r > 2) r = 2;
  if (r < 0) r = 0;
  int c = tx * 4 / 320;
  if (c > 3) c = 3;
  if (c < 0) c = 0;
  return KP_KEYS[r][c];
}

bool resetArm = false;

// ─────────────────────────────────────────────────────────────────────────────
// §0.2/§0.3 keep-alive nap (wake_sim=false only). Persistent power — this is
// light sleep, not rail-off: association state is in RAM, the module owns
// its cadence. Wake sources: SRDY low (MCU wants us) · heartbeat_s timer
// (the §0.3 proactive pull — THE credential-delivery guarantee) · screen
// touch (operator door-knock; also wakes a board before flashing).
// ─────────────────────────────────────────────────────────────────────────────
void enterKeepAliveSleep() {
  Serial.printf("[PWR] idle %lus — light sleep (wake: SRDY / %us timer / touch)\n",
                SLEEP_IDLE_MS / 1000, cfgHeartbeatS);
  Serial.flush(); // USB serial goes quiet during the nap — expected
  mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(LCD_BL, LOW); // dark panel = the visible "napping" cue

  gpio_wakeup_enable((gpio_num_t)SRDY_PIN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)cfgHeartbeatS * 1000000ULL);
  esp_light_sleep_start();

  sleepWakeCount++;
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  digitalWrite(LCD_BL, HIGH);
  lastActivityAt = millis();
  screenDirty = true;
  Serial.printf("[PWR] wake #%u by %s — rejoin + heartbeat pull\n",
                (unsigned)sleepWakeCount,
                timerWake ? "timer (proactive pull)" : "GPIO (SRDY/touch)");
  if (!timerWake) mrdySet(true); // answer the MCU's SRDY immediately
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  lastMqttAttempt = 0; // dial the broker on the next loop pass
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** OZLOCK COMM MODULE — unified doorlock (MCU = LockSim on UART) ***");
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

  // Tuya MCU bus → LockSim Mode B (raw 55 AA frames, wire-tested 2026-07-19)
  Serial1.begin(9600, SERIAL_8N1, TUYA_RX_PIN, TUYA_TX_PIN);
  Serial.println("[TUYA] Serial1 up @ 9600 8N1 GPIO16(TX)/GPIO17(RX)");

  // §0.2 wake lines — MRDY idles HIGH (also satisfies the GPIO8 strap)
  pinMode(SRDY_PIN, INPUT_PULLUP);
  pinMode(MRDY_PIN, OUTPUT);
  digitalWrite(MRDY_PIN, HIGH);

  // Transaction buffer (LittleFS, format on first mount)
  fsUp = LittleFS.begin(true);
  Serial.printf("[FS] LittleFS %s\n", fsUp ? "mounted" : "FAILED — txlog disabled");
  txlogCount0 = txlogCountLines("/txlog.0");
  txlogCount1 = txlogCountLines("/txlog.1");
  Serial.printf("[FS] txlog %u event(s) buffered\n", (unsigned)txlogTotal());

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  gfx->begin();
  gfx->setRotation(5);
  gfx->fillScreen(C_BLACK);

  touchInit();
  pinMode(USER_BUTTON, INPUT_PULLUP); // hold 5s -> factory reset, touch-independent

  // ROOT-CAUSE FIX (2026-07-28) — the ozkey-10 §1 fix, which had only ever
  // been applied to bridge32.ino (its line ~789), never here.
  // esp_event_loop_create_default() inside OpenThread::begin()'s worker task
  // FAILS once WiFi already owns the default event loop: arduino-esp32
  // tolerates ESP_ERR_INVALID_STATE, the bundled OThread.cpp does not. Both
  // later begin() calls (applyProvision + the Thread-resume branch below) run
  // AFTER WiFi.mode(WIFI_STA), so OpenThread came up in a half-initialised
  // state — which is why the resume path logged "OpenThread platform not
  // initialized", why hasActiveDataset() returned a FALSE NEGATIVE against a
  // dataset still present in NVS, and why attach retried in a tight loop.
  // begin() is idempotent (early-returns once started), so calling it here
  // is safe for the Wi-Fi-transport case too.
  OpenThread::begin(false);
  Serial.printf("[THREAD] early begin() otStarted=%d\n", (int)(bool)thread);

  WiFi.mode(WIFI_STA);
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

  // Ceremony identity (RF is up → TRNG seeded) + boot known-answer self-test.
  ozLockKeyInit();
  Serial.printf("[CRYPTO] info.pub=%s\n", ozLockPubHex().c_str());
  // M2: ownership state, printed every boot. This line is the Ask 6 factory-reset
  // evidence — after a reset both info.pub AND this must change (pub re-minted,
  // owner back to "none"), since prefs.clear() wipes the whole "blelock"
  // namespace that holds the keypair and the bond together.
  ozBond0Load();
  Serial.printf("[BOND] bond #0: %s\n",
                g_bond0Present ? ozBond0PubHex().c_str() : "none (unowned)");
  ozCryptoSelfTest();

  if (provisioned && isThread()) {
    // Thread resume (ported from threadcomm.ino): relies entirely on
    // OpenThread's own NVS-backed dataset (never stored in our "blelock"
    // prefs), same as threadcomm.ino always did.
    state = ST_JOINING;
    joinLine1 = "Thread: joining...";
    joinLine2 = "(no uplink yet — ozkey-10 gap)";
    OpenThread::begin(false);
    if (thread.hasActiveDataset()) {
      Serial.println("[THREAD] resuming persisted dataset");
    } else {
      Serial.println("[THREAD] no persisted dataset on resume — re-provision needed");
    }
    // ORDER FIX (2026-07-27): see the applyProvision() Thread branch —
    // networkInterfaceUp() must run before start(), not after.
    thread.networkInterfaceUp();
    thread.start();
    threadJoinStart = millis();
  } else if (provisioned) {
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
  Serial.printf("[WAKE] wake_sim=%s hb=%us (SRDY=GPIO%d MRDY=GPIO%d)\n",
                wakeSim ? "ON (bench: SRDY assumed, no sleep)" : "OFF (honest)",
                cfgHeartbeatS, SRDY_PIN, MRDY_PIN);
  lastActivityAt = millis();
  screenDirty = true;
}

void loop() {
  checkFactoryResetButton();

  // XF-52 (R): close the window on expiry — but never mid-session. A client
  // connected at the deadline keeps its link; onDisconnect() closes it then.
  // Cutting a live commissioning off at 60 s would produce exactly the
  // dropped-ladder "unknown" state XF-48 §21.3 exists to handle, self-inflicted.
  if (bleWindowUntil && !bleWindowOpen() && !bleClientConnected) {
    closeBleWindow("60s elapsed");
  }
  tuyaWirePump(); // MCU (LockSim) → module frames off the wire

  // ── WiFi progress (Wi-Fi transport only) ─────────────────────────────────
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  wl_status_t ws = WiFi.status();
  if (!isThread() && ws != lastWifi) {
    lastWifi = ws;
    Serial.printf("[WiFi] status=%d\n", (int)ws);
    if (ws == WL_CONNECTED) {
      configTime(0, 0, "pool.ntp.org");
      // RACE GUARD: only treat this as OUR join succeeding if we're actually
      // associated to the SSID we just asked for — a re-provision landing
      // while a prior association was still settling could otherwise report
      // success against stale connectivity (bridge32 bench bug, 2026-07-26).
      if (state == ST_JOINING && WiFi.SSID() == cfgSsid) {
        notifyStatus("WIFI_OK");
        joinLine1 = "WiFi: OK - IP " + WiFi.localIP().toString();
      }
      screenDirty = true; // dashboard NET indicator
      Serial.printf("[WiFi] up, IP %s\n", WiFi.localIP().toString().c_str());
    }
  }
  if (!isThread() && state == ST_JOINING && ws != WL_CONNECTED && provisioned &&
      wifiJoinStart && millis() - wifiJoinStart > 25000) {
    wifiJoinStart = 0;
    WiFi.disconnect(true); // stop the driver's own retry loop — a bad
                            // SSID/password must not keep hammering the
                            // radio in the background indefinitely
    notifyStatus("WIFI_FAIL");
    joinLine1 = "WiFi FAILED (wrong password?)";
    screenDirty = true;
    if (bleServer == nullptr) startBle();
  }

  // ── Thread progress (Thread transport only, ported from threadcomm.ino) ──
  if (isThread() && state == ST_JOINING) {
    ot_device_role_t role = OpenThread::otGetDeviceRole();

    // STALE-ROLE FIX (2026-08-02, live bench). After commitDataSet() the stack
    // does NOT drop to DETACHED immediately — for a few ms otGetDeviceRole()
    // still reports the role held on the network we are LEAVING. This block
    // then saw LEADER, declared THREAD_OK, and set state = ST_OPERATIONAL,
    // which stops this block running ever again. Observed: THREAD_JOINING,
    // "attached as Leader" and THREAD_OK all in the SAME millisecond, followed
    // 7 s later by thread=Detached — i.e. success was announced before the lock
    // had even left the old network, and the real outcome was never evaluated.
    //
    // Two consequences, both nasty because the device looks fine:
    //   1. The lock reports "Thread: OK" while orphaned on a dead partition —
    //      exactly the latched-JOINED symptom bridge32's logThreadChildren()
    //      comment describes from the other side.
    //   2. THREAD_OK lands 0 ms after THREAD_JOINING, which is what hangs
    //      BANOI's commissioning ladder (XF-53). We had attributed that to a
    //      fast-but-honest attach; it was neither.
    //
    // So after committing a dataset, require the stack to actually let go —
    // one observed DETACHED/DISABLED — before any attached role is believed.
    // Kept OUT of the else-if chain below on purpose: if this were a branch of
    // it, the attach-timeout arm could never fire while we were waiting for a
    // detach that (on a wedged stack) might never come — trading a false success
    // for a silent hang, which is not an improvement.
    if (threadAwaitDetach && (role == OT_ROLE_DETACHED || role == OT_ROLE_DISABLED)) {
      threadAwaitDetach = false;
      Serial.println("[THREAD] left previous network — now joining");
    }

    if (!threadAwaitDetach &&
        (role == OT_ROLE_CHILD || role == OT_ROLE_ROUTER || role == OT_ROLE_LEADER)) {
      Serial.printf("[THREAD] attached as %s\n", OpenThread::otGetStringDeviceRole());
      threadFormed = true;
      notifyStatus("THREAD_OK");
      joinLine1 = "Thread: OK";
      // No enrollment concept over Thread yet (uplink not built — ozkey-10
      // §5); go operational so keypad/DPID/PIN-storage work regardless.
      enrolled = true;
      saveConfig();
      state = ST_OPERATIONAL;
      screenDirty = true;
      threadUdpBegin();
    } else if (threadJoinStart && millis() - threadJoinStart > THREAD_JOIN_TIMEOUT_MS) {
      // LATCH FIX (2026-08-02, bench): threadJoinStart must be cleared or this
      // branch re-fires on EVERY loop iteration until the state changes —
      // observed as ~20 repeats of "[THREAD] attach timeout" in 5 s. The Wi-Fi
      // equivalent already clears wifiJoinStart (see the WIFI_FAIL branch); this
      // arm was simply missing it.
      //
      // The real cost is not log noise: notifyStatus() pushes a BLE notification,
      // so a connected BANOI was being flooded with THREAD_FAIL — the app's
      // status ladder sees one failure repeated indefinitely rather than once.
      threadJoinStart = 0;
      Serial.println("[THREAD] attach timeout");
      notifyStatus("THREAD_FAIL");
      joinLine1 = "Thread FAILED";
      screenDirty = true;
      if (bleServer == nullptr) startBle();
    }
  }
  if (isThread() && state == ST_OPERATIONAL && !threadUdpReady &&
      millis() - threadUdpLastAttempt > THREAD_UDP_RETRY_MS) {
    threadUdpBegin();
  }
  if (isThread()) pollThreadUdp();

  // ── MQTT + enroll retry / unpaired announce (Wi-Fi transport only) ───────
  if (!isThread()) {
    if (provisioned) ensureMqtt();
    if (!isLocalMode() && state == ST_JOINING && mqtt.connected() && !enrolled &&
        lastEnrollSent && millis() - lastEnrollSent > 8000 && enrollAttempts < 5) {
      publishEnroll();
    }
    if (isLocalMode() && mqtt.connected() && !enrolled &&
        millis() - lastUnpairedAnnounce > 20000) {
      publishUnpairedAnnounce();
    }

    // ── heartbeat (Wi-Fi/MQTT only — Thread has no uplink yet, ozkey-10 §5) ─
    if (mqtt.connected() && millis() - lastHeartbeat > cfgHeartbeatS * 1000UL) {
      lastHeartbeat = millis();
      publishHeartbeat();
    }
  }

  // ── door-status mirror auto-relock (matches LockSim's 5s) ────────────────
  if (doorStatus == "UNLOCKED" && millis() - doorUnlockAt >= DOOR_UNLOCK_MS) {
    doorStatus = "LOCKED";
    screenDirty = true;
  }

  // ── §0.2 MRDY release after 10s serial idle ──────────────────────────────
  if (mrdyAsserted && millis() - lastWireActivityAt > MRDY_IDLE_RELEASE_MS)
    mrdySet(false);

  // ── touch: factory reset only ('*' zone arms, '5' zone fires) ────────────
  {
    int tx, ty;
    if (touchRead(tx, ty)) {
      char k = keyAt(tx, ty);
      Serial.printf("[TOUCH] %d,%d -> key '%c'\n", tx, ty, k ? k : '-');
      if (resetArm) {
        resetArm = false;
        if (k == '5') factoryReset();
        Serial.println("[RESET] disarmed");
      } else if (k == '*') {
        resetArm = true;
        Serial.println("[RESET] armed — tap 5 to wipe");
      }
    }
  }

  // ── periodic monitor + dashboard refresh (MCU-link age ticks) ────────────
  static unsigned long lastMon = 0;
  if (millis() - lastMon > 10000) {
    lastMon = millis();
    const char *st = state == ST_OPERATIONAL ? "OPERATIONAL"
                     : state == ST_JOINING   ? "JOINING"
                                             : "ADVERTISING";
    String modeInfo = cfgMode;
    if (isLocalMode())
      modeInfo += cfgRoomNo.length() ? (" room=" + cfgRoomNo) : " (no room)";
    Serial.printf("[MON] %s xport=%s mode=%s wifi=%s ip=%s mqtt=%s thread=%s "
                  "udp=%s mcu=%s tx=%u rx=%u wake=%s mrdy=%s srdy=%s hb=%us "
                  "naps=%u heap=%u\n",
                  st, provisioned ? cfgTransport.c_str() : "unset", modeInfo.c_str(),
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.localIP().toString().c_str(),
                  mqtt.connected() ? "up" : "down",
                  // LIVE role, not the latched threadFormed flag (2026-07-28).
                  // threadFormed is set once on first attach and never cleared,
                  // so it reported "up" even while actually detached — which
                  // masked a bridge reboot during the relay bench test and made
                  // a void run look like a genuine receive failure.
                  isThread() ? OpenThread::otGetStringDeviceRole()
                             : (threadFormed ? "up" : "down"),
                  threadUdpReady ? "up" : "down",
                  mcuLinkUp() ? "up" : "DOWN",
                  (unsigned)mcuTxFrames, (unsigned)mcuRxFrames,
                  wakeSim ? "sim" : "real",
                  mrdyAsserted ? "LOW" : "high",
                  digitalRead(SRDY_PIN) == LOW ? "LOW" : "high",
                  cfgHeartbeatS, (unsigned)sleepWakeCount,
                  (unsigned)ESP.getFreeHeap());
    if (state == ST_OPERATIONAL) screenDirty = true; // age/link refresh
  }

  // ── screen ────────────────────────────────────────────────────────────────
  if (screenDirty) {
    screenDirty = false;
    if (state == ST_ADVERTISING) drawAdvertising();
    else if (state == ST_JOINING) drawJoining();
    else drawOperational();
  }

  // ── §0.2/§0.3 keep-alive nap (Wi-Fi transport, honest mode only; bench
  // wake_sim skips). Thread SED polling as a wake source is still an open
  // decision (ozkey-10 §7 Q1) — Thread-transport locks never nap in this
  // pass rather than guess at an interaction that hasn't been decided.
  if (!isThread() && !wakeSim && state == ST_OPERATIONAL && enrolled &&
      bleServer == nullptr && !bleClientConnected && !resetArm &&
      doorStatus == "LOCKED" && !touchWasDown && !mrdyAsserted &&
      millis() - lastActivityAt > SLEEP_IDLE_MS) {
    enterKeepAliveSleep();
  }

  delay(15);
}
