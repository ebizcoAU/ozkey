/*
 * simlock — SIMLOCK N8 test-master (bare ESP32-C6-DevKitC-1-N8)
 *
 * ozkit's compliance-test fixture: our OZKIE MCU plays the Tuya "module"
 * side of the wire and drives a manufacturer's DL-MCU through a suite of
 * DP-level test cases, converting each test definition (dp/type/payload)
 * into a real Tuya frame via tuya_frame.h and reporting PASS/FAIL locally
 * (this file) and over MQTT (see mqttPublishStatus()/mqttPublishResult()).
 *
 * DP source of truth: `../common/ozprofile_gen.h` (compiled from
 * `profiles/` by `blelock/tools/gen_profile.py`) — never a number
 * hand-typed here. See `ozkit/docs/simlock-dp-reconciliation.md` and
 * `docs/ozkey-38.md` for why that matters for a fixture that ships to a
 * manufacturer.
 *
 * 🔴 cmd-variant (general vs low-power, `ozkey-27` §2.2 Q1) is UNRESOLVED.
 * tuya_frame.h hardcodes general (0x06/0x07), matching firmware's existing
 * assumption. Do not ship this fixture to a manufacturer until that's
 * confirmed — see `docs/ozkey-38.md` §4 Q2. Safe to build and bench-test
 * against LockSim/an emulator in the meantime (§8.1 of `ozkey-27`: link
 * layer, plumbing and profile loading are not blocked on it).
 *
 * Board: BOARD=SIMLOCK in blelock/Makefile (bare N8, 8MB flash, no
 * LCD/touch — no peripheral conflicts).
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_mac.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "../common/ozprofile.h"
#include "tuya_frame.h"
#include "ozkit_crypto.h"

// ── Firmware version ────────────────────────────────────────────────────────
// BUMP ON EVERY FLASHED CHANGE (house convention, see doorlock.ino).
static const char *FW_VERSION = "simlock-0.1";

// ── DL-MCU UART link ─────────────────────────────────────────────────────────
// 🔴 CORRECTED 2026-08-22 — the earlier claim here (GPIO16/17 belong to an
// onboard CH343 USB-UART bridge chip, "physically live whenever USB-C is
// plugged in") was wrong. Checked against the wrong product page. This
// board's actual pinout (Waveshare ESP32-C6-DEV-KIT-N8, operator-supplied,
// 2026-08-22) has THREE distinct serial-capable interfaces, not two:
//   1. UART0 — U0TXD=GPIO16, U0RXD=GPIO17. A plain hardware UART. Nothing
//      else on this board uses it.
//   2. LP_UART — TXD=GPIO5, RXD=GPIO4, CTS=GPIO3, RTS=GPIO2, DTR=GPIO0,
//      DSR=GPIO1. Unused here.
//   3. Native USB-Serial/JTAG — D+=GPIO13, D-=GPIO12, built into the SoC
//      itself, routed straight to the USB-C connector. THIS is what
//      `CDCOnBoot=cdc` (this board's FQBN, blelock/Makefile) actually uses
//      for flashing and the `Serial` console — not UART0. GPIO16/17 were
//      never in conflict with the console; they were simply never verified.
// So UART0 (16/17) is a genuinely free hardware UART, matching doorlock's
// own TUYA_TX_PIN/TUYA_RX_PIN choice for the same reason on its board.
#define DUT_TX_PIN 16 // U0TXD -> DUT RX (or CP2102 RXD when testing against LockSim)
#define DUT_RX_PIN 17 // U0RXD <- DUT TX (or CP2102 TXD when testing against LockSim)
#define DUT_UART_BAUD 9600 // matches LockSim's useSerialLink.ts (9600 8N1)
// LED(8)/button(9) below and SRDY/MRDY(18/19) further down were reasoned
// about against the same wrong product page as the UART pins above, but
// unlike DUT_TX/RX_PIN they don't collide with anything in the corrected
// pinout either (GPIO8/9/18/19 don't appear in UART0/LP_UART/native-USB
// above) — carried forward unconfirmed rather than blocking on it. Flag if
// wrong on real hardware.

// SRDY (DL-MCU -> N8, input) / MRDY (N8 -> DL-MCU, output). GPIO18/19:
// clean on this board's pinout (SDIO_CMD/FSPICS2 and SDIO_CLK/FSPICS3 are
// unused alt functions here, no strap/USB/CH343 conflict) — confirmed with
// operator 2026-08-17 alongside the physical DL-MCU emulator build.
// Active-low, idle-HIGH (a de-asserted/unplugged emulator reads as idle,
// not as an assertion), level-held not pulsed (a dropped byte self-
// recovers: the line is still asserted, so we re-poll rather than needing
// our own timeout/retry on top of the UART one).
#define DUT_SRDY_PIN 18
#define DUT_MRDY_PIN 19
// Off by default — LockSim (Mode B, plain TX/RX/GND, no handshake lines at
// all) is still the active in-house target as of this session. Flip this
// on once the physical emulator is wired and ready to test against.
static bool dutHandshakeEnabled = false;

HardwareSerial DutUart(1); // UART1 — UART0 stays the USB/console per house convention

// ── MQTT ─────────────────────────────────────────────────────────────────────
// Dedicated ozkit broker, deliberately separate from the lab broker
// ozlockserv/ozlodgeserv/ozpmsserv use (operator's decision, 2026-08-17).
static const char *MQTT_HOST = "mqtt.ebizco.com.au";
static const uint16_t MQTT_PORT = 8883;
// Broker requires auth — confirmed working via mosquitto_pub/sub, 2026-08-17.
// Anonymous connect (the prior nullptr/nullptr) either failed outright or
// succeeded-but-ACL-restricted depending on broker config; either way,
// telemetry was never actually reaching a subscriber.
static const char *MQTT_USER = "simlock";
static const char *MQTT_PASS = "esp32c6n8";
WiFiClientSecure mqttNetClient;
PubSubClient mqttClient(mqttNetClient);

// ── Device identity (ozkit_crypto.h) ────────────────────────────────────────
// Delivered by ozkit/server via POST /api/provision_key, which in turn got it
// from NEXUS's POST /api/v1/dev/lock-keys (development mode) — the same
// courier pattern doorlock's app plays for the lock (ozkey-34.md §2), with
// ozkit/server standing in for "the app" since this board has no BLE/app of
// its own. See ozkit_crypto.h's header comment and the session plan for why:
// replaces a single shared plaintext MQTT credential with per-board X25519
// identity, used here to seal (not just authenticate) telemetry.
static uint8_t g_boardPriv[32];
static uint8_t g_boardPub[32];
static uint8_t g_serverPub[32];
static uint8_t g_sharedSecret[32];
static bool g_keyReady = false;
static uint64_t g_uplinkCounter = 0; // NVS-persisted, monotonic — never resets, avoids nonce reuse

// ── Provisioning (NVS via Preferences, matches bridge32.ino's convention) ───
Preferences prefs;
String cfgSsid, cfgPass;
bool wifiProvisioned = false;

WebServer webServer(80);
DNSServer dnsServer;
bool apMode = false;
String boardId; // "N8-XXXXXX" — last 3 MAC bytes, matches the doc's N8-{MAC} client-id
// Full 6-byte MAC, canonical lowercase 12-hex no separators — matches
// NEXUS's lock_registry.mac_address format exactly (nexus-02.md §N-5),
// needed for POST /api/v1/dev/lock-keys. boardId's 3-byte suffix alone
// isn't enough for that call.
String boardMac;

// ── Active profile ───────────────────────────────────────────────────────────
// Per-manufacturer separation (operator directive, 2026-08-17): this build
// targets exactly one profile. Swap OZ_PROFILE_DEFAULT_ID in ozprofile_gen.h
// (regenerated per-build from profiles/products/*.json) to retarget, rather
// than selecting at runtime — a dev kit ships to one manufacturer.

// ── Test-suite engine ────────────────────────────────────────────────────────
// One entry per DP the seed suite exercises. `enabled=false` entries are
// present for traceability (Ch.13's 50-test numbering) but never sent — see
// docs/ozkey-38.md §3.2/§4 Q1 for why reserved-DP tests are marked, not built.
struct SimTest {
  uint16_t   id;
  const char *name;
  uint8_t    dp;
  OzTuyaDpType type;
  const uint8_t *payload; // down-direction payload to issue; nullptr = read-only/observe
  size_t     payloadLen;
  bool       enabled;
  const char *blockedBy; // non-null when enabled==false
};

static const uint8_t kAutolockOn[]  = {0x01};
static const uint8_t kAutolockOff[] = {0x00};
static const uint8_t kDelay30s[]    = {0x00, 0x00, 0x00, 0x1E}; // VALUE is 4-byte BE
static const uint8_t kVolumeNormal[] = {0x02}; // enum: mute0/low1/normal2/high3
static const uint8_t kBleOn[]  = {0x01};
static const uint8_t kWifiDtm10[] = {0x01};
// DP 76 unlock_ble — VALUE, 4-byte BE, "id of the BLE credential used"
// (range 0-99999). Confirmed on BOTH supplier docs, and this is the DP
// firmware/app now actually open a real door with (XF-120 §8) — the sent
// value is a bench sentinel credential id, not a real bond/credential;
// SIMLOCK tests protocol conformance (does the DUT ack DP 76), not identity.
static const uint8_t kBleUnlockCred[] = {0x00, 0x00, 0x00, 0x01};

// Seed suite: one round-trip test per `confirmed` settings DP, plus
// observe-only placeholders for the report-only DPs (battery/bolt/events) —
// those PASS on receiving ANY well-formed report on that DP within the
// window, since we can't command a DUT to unlock with a credential we can't
// write yet. Credential/unlock tests are listed for Ch.13 traceability only.
static const SimTest kSeedSuite[] = {
  { 1, "Autolock ON",           23, OZ_TUYA_BOOL,  kAutolockOn,   1, true,  nullptr },
  { 2, "Autolock OFF",          23, OZ_TUYA_BOOL,  kAutolockOff,  1, true,  nullptr },
  { 3, "Autolock delay = 30s",  24, OZ_TUYA_VALUE, kDelay30s,     4, true,  nullptr },
  { 4, "Set volume: normal",    21, OZ_TUYA_ENUM,  kVolumeNormal, 1, true,  nullptr },
  { 5, "BLE switch ON",         42, OZ_TUYA_BOOL,  kBleOn,        1, true,  nullptr },
  { 6, "WiFi strategy: DTIM10", 156, OZ_TUYA_ENUM, kWifiDtm10,    1, true,  nullptr },
  { 7, "Observe: battery report",  45, OZ_TUYA_VALUE, nullptr, 0, true, nullptr },
  { 8, "Observe: bolt state report", 47, OZ_TUYA_BOOL, nullptr, 0, true, nullptr },
  { 9, "Observe: alarm report",   60, OZ_TUYA_ENUM,  nullptr, 0, true, nullptr },
  { 10, "Observe: duress report", 98, OZ_TUYA_BOOL,  nullptr, 0, true, nullptr },
  // DP 76 unlock_ble — CONFIRMED, real, hardware-proven (XF-120 §8, replacing
  // the DP 10 remote_unlock test: DP 10 stays RESERVED indefinitely per
  // ozkey-27 Q2, while DP 76 is the DP actually shipped for this purpose).
  { 11, "Open Door (unlock_ble, DP 76)", 76, OZ_TUYA_VALUE, kBleUnlockCred, 4, true, nullptr },
  // Credential — RESERVED DPs, payload layout not supplied (ozkey-27 Q2).
  { 12, "Add PIN credential",        16, OZ_TUYA_RAW, nullptr, 0, false, "ozkey-27 Q2 — bulk_password_add RAW layout not supplied" },
  { 13, "Delete PIN credential",     17, OZ_TUYA_RAW, nullptr, 0, false, "ozkey-27 Q2 — bulk_password_delete RAW layout not supplied" },
  { 14, "Add RFID credential",       13, OZ_TUYA_RAW, nullptr, 0, false, "ozkey-27 Q2 — bulk_unlock_method_add RAW layout not supplied" },
  { 15, "Delete RFID credential",    14, OZ_TUYA_RAW, nullptr, 0, false, "ozkey-27 Q2 — bulk_unlock_method_delete RAW layout not supplied" },
};
static const size_t kSeedSuiteCount = sizeof(kSeedSuite) / sizeof(kSeedSuite[0]);

enum TestStatus : uint8_t { TS_PASS, TS_FAIL, TS_UNSUPPORTED, TS_TIMEOUT };

struct TestResult {
  uint16_t id;
  TestStatus status;
  uint32_t durationMs;
  String detail;
};

// Defined here, not next to runFullTestSuite() where it's used — Arduino's
// auto-generated function prototypes land above every struct definition in
// the file, so a struct used only as a later function's return type doesn't
// exist yet when that prototype is emitted. Same reason TestResult itself
// sits this early.
struct TestSuiteSummary {
  uint16_t total, passed, failed, unsupported;
  uint32_t durationMs;
};

static String bytesToHex(const uint8_t *buf, size_t n) {
  static const char *hexd = "0123456789ABCDEF";
  String s;
  s.reserve(n * 3);
  for (size_t i = 0; i < n; i++) {
    if (i) s += ' ';
    s += hexd[buf[i] >> 4];
    s += hexd[buf[i] & 0xF];
  }
  return s;
}

// The actual frame this test would send, or "OBSERVE" for a read-only
// entry — this is what /config and /live show, real bytes, not the
// SIMLOCK guide mockup's illustrative ones.
static String testHexPreview(const SimTest &t) {
  if (!t.enabled) return "—"; // em dash, nothing sent
  if (t.payload == nullptr) return "OBSERVE";
  uint8_t frame[32];
  size_t n = ozkitBuildDpFrame(OZ_TUYA_CMD_DP_ISSUE_DOWN, t.dp, (uint8_t)t.type,
                                t.payload, t.payloadLen, frame, sizeof(frame));
  return n ? bytesToHex(frame, n) : "(too large)";
}

// ── UART frame I/O ───────────────────────────────────────────────────────────
static uint8_t rxBuf[256];
static size_t  rxLen = 0;

// Handshake-aware send: when dutHandshakeEnabled, assert MRDY ("I want to
// send"), wait for the DL-MCU to assert SRDY back ("ready to receive")
// before writing, then release MRDY. When disabled (LockSim, Mode B),
// this is just DutUart.write() — no lines to wait on.
static bool dutSendFrame(const uint8_t *frame, size_t n, uint32_t timeoutMs) {
  if (!dutHandshakeEnabled) {
    DutUart.write(frame, n);
    return true;
  }
  digitalWrite(DUT_MRDY_PIN, LOW);
  const uint32_t start = millis();
  while (digitalRead(DUT_SRDY_PIN) == HIGH) {
    if (millis() - start > timeoutMs) {
      digitalWrite(DUT_MRDY_PIN, HIGH);
      return false;
    }
    delay(1);
  }
  DutUart.write(frame, n);
  digitalWrite(DUT_MRDY_PIN, HIGH);
  return true;
}

// Drains DutUart into rxBuf and returns a completed frame if one arrived
// within timeoutMs, resyncing past junk the way ozkitScanForFrame() does.
// Handshake-aware: when enabled, mirrors SRDY into MRDY (level-held, not a
// one-shot ack) — the DL-MCU asserting SRDY means "I have something", we
// answer "I'm listening" by holding MRDY low for as long as SRDY stays
// low, and let go the moment it does. This also covers a DL-MCU-initiated
// send (not just a reply to something we sent), which the plain-UART path
// has no equivalent for at all.
static bool waitForFrame(uint32_t timeoutMs, uint8_t *out, size_t *outLen) {
  const uint32_t start = millis();
  rxLen = 0;
  bool mrdyAsserted = false;
  while (millis() - start < timeoutMs) {
    if (dutHandshakeEnabled) {
      bool srdyLow = digitalRead(DUT_SRDY_PIN) == LOW;
      if (srdyLow && !mrdyAsserted) {
        digitalWrite(DUT_MRDY_PIN, LOW);
        mrdyAsserted = true;
      } else if (!srdyLow && mrdyAsserted) {
        digitalWrite(DUT_MRDY_PIN, HIGH);
        mrdyAsserted = false;
      }
    }
    while (DutUart.available() && rxLen < sizeof(rxBuf)) {
      rxBuf[rxLen++] = (uint8_t)DutUart.read();
      size_t flen = 0;
      size_t off = ozkitScanForFrame(rxBuf, rxLen, &flen);
      if (flen > 0) {
        memcpy(out, rxBuf + off, flen);
        *outLen = flen;
        if (mrdyAsserted) digitalWrite(DUT_MRDY_PIN, HIGH);
        return true;
      }
      // ozkitScanForFrame drops confirmed-junk offsets internally on the
      // next call by re-scanning from 0 each time; cheap at this buffer size.
    }
    // Without this, an unconnected DUT (no LockSim/DL-MCU wired up yet —
    // exactly today's bench state) makes this a 2000ms busy-spin with zero
    // yield points. Stacked across a whole test-suite run with no delay
    // between tests, that's long enough to starve the idle task and trip
    // the watchdog — the actual cause of the board freezing/resetting
    // mid test-run, not a WiFi/MQTT bug.
    delay(1);
  }
  if (mrdyAsserted) digitalWrite(DUT_MRDY_PIN, HIGH);
  return false;
}

// Issue one DP command down to the DUT and wait for its report/ack.
static TestResult runTest(const SimTest &t) {
  TestResult r{t.id, TS_FAIL, 0, ""};
  const uint32_t start = millis();

  if (!t.enabled) {
    r.status = TS_UNSUPPORTED;
    r.detail = t.blockedBy ? t.blockedBy : "disabled";
    r.durationMs = millis() - start;
    return r;
  }

  if (t.payload != nullptr) {
    uint8_t frame[32];
    size_t n = ozkitBuildDpFrame(OZ_TUYA_CMD_DP_ISSUE_DOWN, t.dp, (uint8_t)t.type,
                                  t.payload, t.payloadLen, frame, sizeof(frame));
    if (n == 0) {
      r.detail = "payload too large for frame buffer";
      r.durationMs = millis() - start;
      return r;
    }
    if (!dutSendFrame(frame, n, 2000)) {
      r.detail = "DUT never asserted SRDY within 2000ms";
      r.durationMs = millis() - start;
      return r;
    }
  }

  uint8_t resp[256];
  size_t respLen = 0;
  if (!waitForFrame(2000, resp, &respLen)) {
    r.status = TS_TIMEOUT;
    r.detail = "no response frame within 2000ms";
    r.durationMs = millis() - start;
    return r;
  }

  uint8_t dp, type; uint16_t vlen; const uint8_t *val;
  if (!ozkitParseDpReport(resp, respLen, &dp, &type, &vlen, &val)) {
    r.detail = "malformed response frame";
    r.durationMs = millis() - start;
    return r;
  }

  if (dp != t.dp) {
    r.detail = String("response DP ") + dp + " != expected " + t.dp;
    r.durationMs = millis() - start;
    return r;
  }

  // DP 60 (alarm, 18-value enum) and DP 98 (duress, bool) are genuinely
  // observable per profiles/tuya-lock-catalogue.json — worth a real /alert
  // publish the moment one arrives, not just a PASS in the test table.
  if (dp == 60 && vlen >= 1) {
    static const char *kAlarmNames[] = {
      "wrong_finger", "wrong_password", "wrong_card", "wrong_face", "tongue_bad",
      "tongue_not_out", "unclosed_time", "unlock_attempt", "key_in", "too_hot",
      "low_battery", "wrong_finger_vein", "wrong_hand", "stay_alarm", "pry",
      "network_error", "network_recovery", "system_lock",
    };
    uint8_t code = val[0];
    const char *name = code < 18 ? kAlarmNames[code] : "unknown";
    mqttPublishAlert(name, "WARNING", String("DP 60 alarm code ") + code);
  } else if (dp == 98 && vlen >= 1 && val[0] != 0) {
    mqttPublishAlert("duress", "CRITICAL", "DP 98 hijack/duress asserted");
  }

  r.status = TS_PASS;
  r.detail = "OK";
  r.durationMs = millis() - start;
  return r;
}

// ── Device identity persistence (NVS) ────────────────────────────────────────
static bool ozkitHexDecode(const String &hex, uint8_t *out, size_t n) {
  if ((size_t)hex.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char byteStr[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    char *end = nullptr;
    long v = strtol(byteStr, &end, 16);
    if (end != byteStr + 2) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static String ozkitHexEncode(const uint8_t *b, size_t n) {
  static const char *H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 0xF]; }
  return s;
}

// Loads a previously-provisioned identity from NVS, if one exists. Silent,
// non-fatal when absent — this is the normal state for a board that hasn't
// been provisioned by ozkit/server yet (see g_keyReady checks throughout).
static void loadDeviceIdentity() {
  prefs.begin("simlock", true);
  bool has = prefs.getBool("keyReady", false);
  if (has) {
    String privHex = prefs.getString("boardPriv", "");
    String pubHex = prefs.getString("boardPub", "");
    String srvHex = prefs.getString("serverPub", "");
    g_uplinkCounter = prefs.getULong64("ctr", 0);
    if (ozkitHexDecode(privHex, g_boardPriv, 32) && ozkitHexDecode(pubHex, g_boardPub, 32) &&
        ozkitHexDecode(srvHex, g_serverPub, 32) &&
        ozkitX25519(g_boardPriv, g_serverPub, g_sharedSecret)) {
      g_keyReady = true;
    }
  }
  prefs.end();
}

static void saveDeviceIdentity() {
  prefs.begin("simlock", false);
  prefs.putBool("keyReady", true);
  prefs.putString("boardPriv", ozkitHexEncode(g_boardPriv, 32));
  prefs.putString("boardPub", ozkitHexEncode(g_boardPub, 32));
  prefs.putString("serverPub", ozkitHexEncode(g_serverPub, 32));
  prefs.putULong64("ctr", g_uplinkCounter);
  prefs.end();
}

// Persists the counter every N increments rather than every single one —
// mirrors the lock's own counter_floor discipline (ozkey-27, ozcrypto.h):
// an occasional extra replay window on power loss is a cheaper trade than
// ~150 flash writes/day for a value that changes on every heartbeat.
static void saveUplinkCounter() {
  prefs.begin("simlock", false);
  prefs.putULong64("ctr", g_uplinkCounter);
  prefs.end();
}

// ── WiFi provisioning + captive portal ──────────────────────────────────────
static void loadWifiConfig() {
  prefs.begin("simlock", true);
  wifiProvisioned = prefs.getBool("prov", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  prefs.end();
}

static void saveWifiConfig(const String &ssid, const String &pass) {
  prefs.begin("simlock", false);
  prefs.putBool("prov", true);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// ── Test-profile selection — local, NVS-persisted (XFtposDecisions-124 §11:
// SIMLOCK issues live UART commands at real hardware from a compiled-in DP
// table, the same posture doorlock uses for its own pinned profile; a
// runtime Nexus fetch would be a correctness/security regression, not an
// improvement, for a fixture that ships to third-party manufacturers'
// benches). Returns "" if nothing has been explicitly chosen yet — caller
// keeps whatever ozProfileSelect() was already given.
static String loadProfileSelection() {
  prefs.begin("simlock", true);
  String id = prefs.getString("profId", "");
  prefs.end();
  return id;
}

static void saveProfileSelection(const String &id) {
  prefs.begin("simlock", false);
  prefs.putString("profId", id);
  prefs.end();
}

// ── Shared page shell ────────────────────────────────────────────────────────
// ozkit's own design, not the SIMLOCK guide mockups (operator, 2026-08-17:
// "u can have your design.. my design just for illustration"; separately,
// "this is a commercial product" — treated as a lab diagnostic instrument's
// own panel, not a SaaS dashboard).
//
// Design plan:
//   Color   — graphite ink on a cool off-white ground (not pure white/grey —
//             both picked, not inherited); one accent, deep signal-teal, used
//             ONLY for primary actions and the wordmark rule; semantic
//             pass/fail/warn/unsupported colors are separate from the accent,
//             never reused for it.
//   Type    — system sans for structure (labels, headings, prose) with tight
//             tracking on small caps labels; system monospace with
//             tabular-nums for every piece of actual DATA (hex frames, board
//             IDs, DP numbers, latencies) — the monospace/sans split IS the
//             point: bytes look like bytes, prose looks like prose.
//             Deliberately no @font-face/webfont: this page's first and most
//             important render is inside its OWN captive-AP with zero
//             internet path, so a remote font would silently fail exactly
//             when it matters most.
//   Layout  — a slim instrument-panel header (wordmark + board identity +
//             live status pills) over a single content card; tables read as
//             data, not prose — status is a pill (shape + color), never
//             color alone.
static String pageShell(const String &eyebrow, const String &subtitle, const String &body) {
  String h;
  h += "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>";
  h += "<title>SIMLOCK &middot; " + eyebrow + "</title><style>";
  h += ":root{";
  h += "--ink:#16212c;--paper:#f4f6f8;--panel:#ffffff;--line:#dde3e9;--muted:#5c6975;";
  h += "--accent:#0f7a78;--accent-ink:#ffffff;";
  h += "--ok:#1f9d55;--fail:#d6494a;--warn:#c98a1f;--off:#8a94a0;";
  h += "}";
  h += "*{box-sizing:border-box}";
  h += "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--paper);color:var(--ink);margin:0;line-height:1.45}";
  h += "code,.mono{font-family:ui-monospace,'SF Mono','Cascadia Code',Menlo,Consolas,monospace;font-variant-numeric:tabular-nums}";
  h += ".hdr{display:flex;align-items:baseline;justify-content:space-between;gap:16px;flex-wrap:wrap;";
  h += "padding:18px 28px;border-bottom:3px solid var(--accent);background:var(--ink);color:#fff}";
  h += ".hdr .wordmark{font-size:.72em;font-weight:800;letter-spacing:.14em;text-transform:uppercase;color:var(--accent);opacity:.95}";
  h += ".hdr h1{margin:2px 0 0;font-size:1.28em;font-weight:700;letter-spacing:-.01em}";
  h += ".hdr .sub{font-size:.82em;opacity:.72;margin-top:3px}.hdr .sub code{color:#cfe;opacity:.9}";
  h += ".wrap{max-width:900px;margin:0 auto;padding:24px 20px 48px}";
  h += ".card{background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow:hidden}";
  h += ".pad{padding:22px 26px}";
  h += "label{display:block;font-weight:700;font-size:.78em;letter-spacing:.03em;text-transform:uppercase;color:var(--muted);margin:16px 0 7px}";
  h += "label:first-child{margin-top:0}";
  h += "input{width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:6px;font-size:1em;background:#fff;color:var(--ink)}";
  h += "input:focus{outline:2px solid var(--accent);outline-offset:1px;border-color:var(--accent)}";
  h += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:12px 20px;border:0;border-radius:6px;";
  h += "font-weight:700;font-size:.92em;letter-spacing:.02em;cursor:pointer}";
  h += ".btn-block{display:flex;width:100%;margin-top:20px}";
  h += ".btn-accent{background:var(--accent);color:var(--accent-ink)}";
  h += ".btn-warn{background:var(--warn);color:#fff}.btn-danger{background:var(--fail);color:#fff}";
  h += ".btn[disabled]{opacity:.45;cursor:default}";
  h += ".note{background:var(--paper);border:1px dashed var(--line);border-radius:6px;padding:11px 14px;font-size:.83em;color:var(--muted);margin-top:18px}";
  h += ".row{display:flex;gap:10px;flex-wrap:wrap}";
  h += "table{width:100%;border-collapse:collapse;font-size:.88em}";
  h += "th{text-align:left;padding:11px 10px;border-bottom:2px solid var(--ink);font-size:.72em;letter-spacing:.05em;text-transform:uppercase;color:var(--muted)}";
  h += "td{padding:10px 10px;border-bottom:1px solid var(--line);vertical-align:middle}";
  h += "tr:last-child td{border-bottom:0}";
  h += "tr.running{background:#eaf6f5}";
  h += ".pill{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:20px;font-size:.76em;font-weight:700;letter-spacing:.02em;text-transform:uppercase}";
  h += ".pill:before{content:'';width:7px;height:7px;border-radius:50%;background:currentColor}";
  h += ".pill-pass{background:#e5f6ea;color:var(--ok)}.pill-fail{background:#fbe9e9;color:var(--fail)}";
  h += ".pill-warn{background:#fbf1de;color:var(--warn)}.pill-off{background:#eef0f2;color:var(--off)}";
  h += ".pill-idle{background:#eef0f2;color:var(--muted)}";
  h += ".tablewrap{overflow-x:auto}";
  h += ".foot{padding:14px 26px;background:var(--paper);border-top:1px solid var(--line);font-size:.8em;color:var(--muted)}";
  h += "a.navlink{color:#cfe;text-decoration:none;font-size:.82em;font-weight:600}a.navlink:hover{text-decoration:underline}";
  h += "</style></head><body>";
  h += "<div class=hdr><div><div class=wordmark>SIMLOCK</div>";
  h += "<h1>" + eyebrow + "</h1><div class=sub>" + subtitle + "</div></div>";
  h += "<div class=row style='align-self:center'><a class=navlink href=/config>Config</a><a class=navlink href=/live>Live Run</a></div>";
  h += "</div>";
  h += "<div class=wrap><div class=card><div class=pad>" + body + "</div></div></div>";
  h += "</body></html>";
  return h;
}

static void handlePortalRoot() {
  Serial.printf("[HTTP] GET / from %s\n", webServer.client().remoteIP().toString().c_str());
  String body;
  body += "<form method=POST action='/save'>";
  body += "<label>Target Router Wi-Fi SSID</label><input name=ssid autocomplete=off required>";
  body += "<label>Router Wi-Fi Password</label>";
  body += "<div style='position:relative'>";
  body += "<input id=pass name=pass type=password autocomplete=off style='padding-right:64px'>";
  body += "<button type=button onclick=\"var i=document.getElementById('pass');var on=i.type==='password';i.type=on?'text':'password';this.textContent=on?'Hide':'Show';\" ";
  body += "style='position:absolute;right:6px;top:50%;transform:translateY(-50%);border:0;background:none;color:var(--accent);font-weight:700;font-size:.8em;cursor:pointer;padding:4px 6px'>Show</button>";
  body += "</div>";
  body += "<button class='btn btn-accent btn-block' type=submit>Save &amp; Connect</button>";
  body += "</form>";
  webServer.send(200, "text/html", pageShell("Setup", "profile <code>" + String(ozProfileId()) + "</code>", body));
}

static void handlePortalSave() {
  Serial.printf("[HTTP] POST /save from %s\n", webServer.client().remoteIP().toString().c_str());
  if (!webServer.hasArg("ssid")) { webServer.send(400, "text/plain", "missing ssid"); return; }
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  saveWifiConfig(ssid, pass);

  // mDNS (simlock.local) is unreliable across clients — confirmed failing
  // on Android Chrome AND Firefox this session, not just one browser quirk.
  // Real fix: don't drop the phone's connection to us before we know the
  // answer. WIFI_AP_STA keeps the SIMLOCK AP alive WHILE we join the real
  // network, so we can show the actual IP on THIS page, over THIS still-
  // connected AP session, before the phone ever has to go find us again.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("simlock");
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(200);

  String body;
  if (WiFi.status() == WL_CONNECTED) {
    const String ip = WiFi.localIP().toString();
    body += "<p style='margin-top:0'>Connected to <code>" + ssid + "</code>.</p>";
    body += "<label>This board's address on that network</label>";
    body += "<p class=mono style='font-size:1.3em;margin:4px 0 18px'><a href='http://" + ip + "/config'>http://" + ip + "/config</a></p>";
    body += "<div class=note>Write this address down &mdash; <code>simlock.local</code> doesn't resolve on every device "
            "(confirmed failing on Android). This Wi-Fi network (<code>SIMLOCK</code>) will disconnect in a few seconds; "
            "reconnect your device to <code>" + ssid + "</code>, then visit the address above.</div>";
  } else {
    body += "<p style='margin-top:0'>Could not connect to <code>" + ssid + "</code> &mdash; check the password and try again.</p>";
    body += "<button class='btn btn-accent btn-block' onclick=\"location.href='/'\">Back</button>";
  }
  webServer.send(200, "text/html", pageShell("Connected", "Board <code>" + boardId + "</code>", body));

  if (WiFi.status() == WL_CONNECTED) {
    delay(6000); // hold the AP open long enough to actually read/note the IP
    ESP.restart(); // clean boot into STA-only, same path as every other boot
  }
}

static void startCaptivePortal() {
  apMode = true;
  String apSsid = "SIMLOCK";
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", handlePortalRoot);
  webServer.on("/save", HTTP_POST, handlePortalSave);
  webServer.onNotFound(handlePortalRoot); // captive-portal catch-all
  webServer.begin();
  Serial.printf("[AP] %s up, portal at http://%s/\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

// Polling loop() can only see "connected" vs "not" every ~10s (the
// heartbeat interval) and misses transient drops entirely if it reconnects
// in between checks. Events catch every transition with a timestamp and,
// for disconnects, WHY — auth failure, beacon timeout, AP not found, etc.
// are very different bugs and look identical from the outside otherwise.
static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi event] STA_CONNECTED, t=%lums\n", millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi event] GOT_IP %s, t=%lums\n", WiFi.localIP().toString().c_str(), millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi event] STA_DISCONNECTED reason=%d, t=%lums\n",
                    info.wifi_sta_disconnected.reason, millis());
      break;
    default: break;
  }
}

static bool connectStation(uint32_t timeoutMs) {
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  // Separate from mDNS (MDNS.begin() below, post-connect) — this is the
  // DHCP hostname, which shows up in a router's own connected-devices page.
  // Needed because Android Chrome doesn't resolve simlock.local at all
  // (no mDNS support), so "find it by name in the router's client list"
  // is the fallback path, and that only works if it HAS a name.
  WiFi.setHostname("simlock");
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// ── REST API (Ch.12 inventory, confirmed-DP subset) ─────────────────────────
// Endpoints touching a `reserved` DP return 501 with the catalogue's own
// blocked_by reason (ozkey-28's don't-blind-forward rule) rather than
// faking success.
static void sendUnsupported(const char *reason) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["code"] = "UNSUPPORTED";
  doc["reason"] = reason;
  String out; serializeJson(doc, out);
  webServer.send(501, "application/json", out);
}

static void handleApiVersion() {
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["profile_id"] = ozProfileId();
  // The DUT profile's canonical id (fw-tuya-luona-ds013-t3-r2, etc.) —
  // generated once by gen_profile.py from the same source doorlock's enrol
  // payload reports (XF-123/124), so a QA run is traceable to "which
  // manufacturer map, which catalogue rev" without a second, hand-kept copy.
  doc["firmware_id"] = ozProfile()->firmware_id;
  doc["board_id"] = boardId;
  String out; serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

// GET /api/profile — every profile compiled into ozprofile_gen.h, for the
// /config picker (and any external tooling). "pairable" here is a cheap,
// honest proxy (has a Tuya PID, isn't the deprecated fiction map) — the full
// derivation (a complete DP map too) lives in gen_profile.py's models.json;
// this is a UI hint, not a claim to be dispatched against.
//
// POST /api/profile?id=<profile_id> — switch which compiled-in DP map this
// bench issues over UART. Local only: validates against OZ_PROFILES[],
// persists to NVS, no network round trip (XFtposDecisions-124 §11).
//
// One handler for both methods, branching internally — registering two
// separate handler functions at the same URI (one implicit GET, one
// explicit HTTP_POST) does NOT dispatch by method on this WebServer library;
// only the first-registered handler ever fires, for either method (found the
// hard way — /api/dut/handshake already worked around this the same way and
// that was the tell). Same pattern as handleApiDutHandshake.
static void handleApiProfile() {
  if (webServer.method() == HTTP_POST) {
    if (!webServer.hasArg("id")) {
      webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"missing id\"}");
      return;
    }
    String id = webServer.arg("id");
    if (!ozProfileSelect(id.c_str())) {
      webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"unknown profile id\"}");
      return;
    }
    saveProfileSelection(id);
    Serial.printf("[HTTP] profile switched to '%s' (operator, /config)\n", id.c_str());
    JsonDocument doc;
    doc["ok"] = true;
    doc["profile_id"] = ozProfileId();
    doc["firmware_id"] = ozProfile()->firmware_id;
    String out; serializeJson(doc, out);
    webServer.send(200, "application/json", out);
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["profiles"].to<JsonArray>();
  for (uint8_t i = 0; i < OZ_PROFILE_COUNT; i++) {
    const OzProfile &p = OZ_PROFILES[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["firmware_id"] = p.firmware_id;
    o["tuya_pid"] = p.tuya_pid ? p.tuya_pid : (const char *)nullptr;
    o["dp_count"] = p.count;
    o["pairable"] = (p.tuya_pid != nullptr) && !p.deprecated;
  }
  doc["active"] = ozProfileId();
  String out; serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

static void handleApiLockUnlock() {
  sendUnsupported("DP 10 remote_unlock — RAW payload layout not supplied (ozkey-27 Q2)");
}

static void handleApiCredentialGeneric() {
  sendUnsupported("Credential-write DPs (13-19, 86) are RESERVED — RAW payload layout not supplied (ozkey-27 Q2)");
}

// No OTA mechanism exists yet (no update server, no image-signing story) —
// stubbed rather than faked, same don't-blind-forward discipline as the
// credential endpoints above.
static void handleApiOtaGeneric() {
  sendUnsupported("OTA not implemented yet — no update server exists");
}

static void handleApiTestsList() {
  JsonDocument doc;
  JsonArray arr = doc["tests"].to<JsonArray>();
  for (size_t i = 0; i < kSeedSuiteCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = kSeedSuite[i].id;
    o["name"] = kSeedSuite[i].name;
    o["dp"] = kSeedSuite[i].dp;
    o["enabled"] = kSeedSuite[i].enabled;
    if (!kSeedSuite[i].enabled) o["blocked_by"] = kSeedSuite[i].blockedBy;
  }
  String out; serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

// Runs the full seed suite, publishing the same test/start|result*|complete
// sequence over MQTT that the {"action":"start_test"} remote command
// triggers (runFullTestSuite() is the one shared implementation — see its
// own comment). REST response is just the summary; per-test detail is on
// the wire via test/result and, for interactive use, via /api/tests/run_one
// through the /live page.
static void handleApiTestsRun() {
  TestSuiteSummary s = runFullTestSuite();
  JsonDocument doc;
  doc["summary"]["total"] = s.total;
  doc["summary"]["passed"] = s.passed;
  doc["summary"]["failed"] = s.failed;
  doc["summary"]["unsupported"] = s.unsupported;
  doc["summary"]["duration_ms"] = s.durationMs;
  String out; serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

// Runs exactly one seed-suite entry by id and returns its result — this is
// what the /live page's JS calls in sequence, so each table row updates as
// its own request completes instead of waiting for the whole suite (which
// /api/tests/run still does, for the MQTT/automation path).
static void handleApiTestsRunOne() {
  if (!webServer.hasArg("id")) { webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"missing id\"}"); return; }
  uint16_t id = (uint16_t)webServer.arg("id").toInt();
  for (size_t i = 0; i < kSeedSuiteCount; i++) {
    if (kSeedSuite[i].id != id) continue;
    TestResult r = runTest(kSeedSuite[i]);
    mqttPublishTestResult(r, kSeedSuite[i].name); // same cadence as the full-suite path
    JsonDocument doc;
    doc["id"] = r.id;
    doc["status"] = r.status == TS_PASS ? "PASS" : r.status == TS_UNSUPPORTED ? "UNSUPPORTED"
                    : r.status == TS_TIMEOUT ? "TIMEOUT" : "FAIL";
    doc["duration_ms"] = r.durationMs;
    doc["detail"] = r.detail;
    String out; serializeJson(doc, out);
    webServer.send(200, "application/json", out);
    return;
  }
  webServer.send(404, "application/json", "{\"ok\":false,\"reason\":\"unknown test id\"}");
}

// ── /config — the real test-definition table, DP-catalogue-driven ──────────
static void handleConfigPage() {
  Serial.printf("[HTTP] GET /config from %s\n", webServer.client().remoteIP().toString().c_str());
  String rows;
  for (size_t i = 0; i < kSeedSuiteCount; i++) {
    const SimTest &t = kSeedSuite[i];
    rows += "<tr><td class=mono>" + String(t.id) + "</td><td>" + t.name + "</td>";
    rows += "<td class=mono>DP " + String(t.dp) + "</td>";
    rows += "<td>" + String(t.enabled
              ? "<span class='pill pill-pass'>enabled</span>"
              : "<span class='pill pill-off'>blocked</span>") + "</td>";
    rows += "<td class=mono>" + testHexPreview(t) + "</td></tr>";
    if (!t.enabled) {
      rows += "<tr><td></td><td colspan=4 style='color:var(--muted);font-size:.85em;padding-top:0'>" + String(t.blockedBy) + "</td></tr>";
    }
  }
  String profOptions;
  bool onSeedSuiteProfile = false;
  for (uint8_t i = 0; i < OZ_PROFILE_COUNT; i++) {
    const OzProfile &p = OZ_PROFILES[i];
    bool isActive = strcmp(p.id, ozProfileId()) == 0;
    if (isActive && strcmp(p.id, "tuya-luona-ds013-t3") == 0) onSeedSuiteProfile = true;
    profOptions += "<option value='" + String(p.id) + "'" + (isActive ? " selected" : "") + ">" +
                    String(p.id) + (p.tuya_pid ? "" : " (no PID)") + "</option>";
  }
  String body;
  body += "<div class=row style='justify-content:space-between;align-items:baseline;margin-bottom:14px'>";
  body += "<div><label for=profSel style='margin:0'>Active Profile</label>";
  body += "<div class=row style='gap:8px;margin-top:4px'>";
  body += "<select id=profSel style='min-width:230px'>" + profOptions + "</select>";
  body += "<button class=btn onclick=switchProfile()>Switch</button></div></div>";
  body += "<div><label style='margin:0'>Seed Suite</label><p class=mono style='margin:2px 0 0'>" + String(kSeedSuiteCount) + " cases</p></div>";
  body += "</div>";
  if (!onSeedSuiteProfile) {
    body += "<div class=note style='margin-bottom:14px'>&#9888; The seed suite below is written against "
            "<code>tuya-luona-ds013-t3</code>'s DP semantics specifically (DP 76 = unlock_ble, etc). "
            "Switching the active profile changes what this board REPORTS itself as "
            "(<code>/api/version</code>, MQTT heartbeat) but does NOT retarget which DPs the seed suite "
            "issues &mdash; running it now would test Luona's DP numbers against a different profile's hardware. "
            "A profile-driven seed suite is future work.</div>";
  }
  body += "<div class=tablewrap><table><thead><tr><th>#</th><th>Test</th><th>DP</th><th>Status</th><th>Frame (issued)</th></tr></thead><tbody>";
  body += rows;
  body += "</tbody></table></div>";
  body += "<button class='btn btn-accent btn-block' style='margin-top:20px' onclick=\"location.href='/live'\">Run Test Suite &rarr;</button>";
  body += "<script>async function switchProfile(){"
          "const id=document.getElementById('profSel').value;"
          "const r=await fetch('/api/profile?id='+encodeURIComponent(id),{method:'POST'});"
          "if(r.ok){location.reload();}else{alert('Could not switch profile');}}</script>";
  webServer.send(200, "text/html", pageShell("Test Configuration", "Board <code>" + boardId + "</code> &middot; DPs not in this table are RESERVED &mdash; see <code>ozkey-27</code> Q2", body));
}

// ── /live — step-through execution, JS-driven via /api/tests/run_one ───────
static void handleLivePage() {
  Serial.printf("[HTTP] GET /live from %s\n", webServer.client().remoteIP().toString().c_str());
  String rows;
  for (size_t i = 0; i < kSeedSuiteCount; i++) {
    const SimTest &t = kSeedSuite[i];
    rows += "<tr id='row" + String(t.id) + "' data-id='" + String(t.id) + "'>";
    rows += "<td class=mono>" + String(t.id) + "</td><td>" + t.name + "</td>";
    rows += "<td class='st'><span class='pill pill-idle'>waiting</span></td>";
    rows += "<td class='lat mono'>&mdash;</td>";
    rows += "<td class='hex mono'>" + testHexPreview(t) + "</td></tr>";
  }
  String body;
  body += "<div class=row style='justify-content:space-between;align-items:baseline;margin-bottom:14px'>";
  body += "<div id=runStatus><label style='margin:0'>Status</label><p class=mono style='margin:2px 0 0'>idle</p></div>";
  body += "<button class='btn btn-accent' id=runBtn onclick=runSuite()>&#9654; Run Test Suite</button>";
  body += "</div>";
  body += "<div class=tablewrap><table><thead><tr><th>#</th><th>Test</th><th>Result</th><th>Latency</th><th>Frame</th></tr></thead><tbody id=tbody>";
  body += rows;
  body += "</tbody></table></div>";
  body += "<script>";
  body += "const ids=[" ;
  for (size_t i = 0; i < kSeedSuiteCount; i++) { if (i) body += ","; body += String(kSeedSuite[i].id); }
  body += "];";
  body += "function pill(cls,label){return \"<span class='pill pill-\"+cls+\"'>\"+label+\"</span>\";}";
  body += "async function runSuite(){";
  body += "document.getElementById('runBtn').disabled=true;";
  body += "document.querySelector('#runStatus p').textContent='running';";
  body += "let pass=0,fail=0,unsup=0;";
  body += "for(const id of ids){";
  body += "const row=document.getElementById('row'+id);row.classList.add('running');";
  body += "row.querySelector('.st').innerHTML=pill('warn','running');";
  body += "try{const res=await fetch('/api/tests/run_one?id='+id,{method:'POST'});const d=await res.json();";
  body += "row.classList.remove('running');";
  body += "let cls=d.status==='PASS'?'pass':(d.status==='UNSUPPORTED'?'off':'fail');";
  body += "row.querySelector('.st').innerHTML=pill(cls,d.status)+' <span style=\"color:var(--muted);font-size:.85em\">'+(d.detail||'')+'</span>';";
  body += "row.querySelector('.lat').textContent=d.duration_ms+' ms';";
  body += "if(d.status==='PASS')pass++;else if(d.status==='UNSUPPORTED')unsup++;else fail++;";
  body += "}catch(e){row.querySelector('.st').innerHTML=pill('fail','ERROR');fail++;}";
  body += "}";
  body += "document.querySelector('#runStatus p').textContent='done — '+pass+' pass / '+fail+' fail / '+unsup+' unsupported';";
  body += "document.getElementById('runBtn').disabled=false;";
  body += "}";
  body += "</script>";
  webServer.send(200, "text/html", pageShell("Live Execution", "Board <code>" + boardId + "</code> &middot; each row calls /api/tests/run_one in sequence", body));
}

// Toggle at runtime rather than reflash — GET reports current state, POST
// with ?on=1 or ?on=0 sets it. Switching between LockSim (off) and the
// physical DL-MCU emulator (on) is otherwise a same-day, repeated action.
static void handleApiDutHandshake() {
  if (webServer.method() == HTTP_POST && webServer.hasArg("on")) {
    dutHandshakeEnabled = webServer.arg("on").toInt() != 0;
    Serial.printf("[DUT] handshake %s\n", dutHandshakeEnabled ? "ENABLED" : "disabled");
  }
  JsonDocument doc;
  doc["handshake_enabled"] = dutHandshakeEnabled;
  doc["srdy_pin"] = DUT_SRDY_PIN;
  doc["mrdy_pin"] = DUT_MRDY_PIN;
  doc["srdy_level"] = digitalRead(DUT_SRDY_PIN) == LOW ? "asserted" : "idle";
  String out; serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

// Delivers this board's X25519 identity — called by ozkit/server, which got
// the keypair from NEXUS's POST /api/v1/dev/lock-keys (development mode)
// and is playing the courier role doorlock's app plays for the lock
// (ozkey-34.md §2). Body: {"pub":"<64 hex>","priv":"<64 hex>","server_pub":"<64 hex>"}.
// No auth on this endpoint today — same trust boundary as every other
// unauthenticated REST call on this board (local network only). Revisit
// if/when this fixture leaves a trusted bench network.
static void handleApiProvisionKey() {
  Serial.printf("[HTTP] POST /api/provision_key from %s\n", webServer.client().remoteIP().toString().c_str());
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"missing JSON body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, webServer.arg("plain")) != DeserializationError::Ok) {
    webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"malformed JSON\"}");
    return;
  }
  String pubHex = doc["pub"] | "";
  String privHex = doc["priv"] | "";
  String srvHex = doc["server_pub"] | "";

  uint8_t priv[32], pub[32], srv[32];
  if (!ozkitHexDecode(privHex, priv, 32) || !ozkitHexDecode(pubHex, pub, 32) ||
      !ozkitHexDecode(srvHex, srv, 32)) {
    webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"pub/priv/server_pub must each be 64 hex chars\"}");
    return;
  }

  // Confirm the delivered pub actually derives from the delivered priv —
  // catches a transcription/pairing error at provision time rather than a
  // silent sealing failure on the first heartbeat after.
  uint8_t derivedPub[32];
  if (!ozkitX25519Base(priv, derivedPub) || memcmp(derivedPub, pub, 32) != 0) {
    webServer.send(400, "application/json", "{\"ok\":false,\"reason\":\"pub does not derive from priv\"}");
    return;
  }

  memcpy(g_boardPriv, priv, 32);
  memcpy(g_boardPub, pub, 32);
  memcpy(g_serverPub, srv, 32);
  if (!ozkitX25519(g_boardPriv, g_serverPub, g_sharedSecret)) {
    webServer.send(500, "application/json", "{\"ok\":false,\"reason\":\"ECDH failed\"}");
    return;
  }
  g_uplinkCounter = 0; // fresh identity, fresh counter — this is a new key, not a rotation
  g_keyReady = true;
  saveDeviceIdentity();

  Serial.println("[PROVISION] identity installed — telemetry now sealed");
  JsonDocument out;
  out["ok"] = true;
  out["board_id"] = boardId;
  out["mac"] = boardMac;
  String s; serializeJson(out, s);
  webServer.send(200, "application/json", s);
}

static void setupRestApi() {
  webServer.on("/api/version", handleApiVersion);
  webServer.on("/api/profile", handleApiProfile);
  webServer.on("/api/profile", HTTP_POST, handleApiProfile);
  webServer.on("/api/provision_key", HTTP_POST, handleApiProvisionKey);
  webServer.on("/api/dut/handshake", handleApiDutHandshake);
  webServer.on("/api/dut/handshake", HTTP_POST, handleApiDutHandshake);
  webServer.on("/api/lock/unlock", HTTP_POST, handleApiLockUnlock);
  webServer.on("/api/credential/pin/otp", HTTP_POST, handleApiCredentialGeneric);
  webServer.on("/api/credential/pin/duration", HTTP_POST, handleApiCredentialGeneric);
  webServer.on("/api/credential/pin/permanent", HTTP_POST, handleApiCredentialGeneric);
  webServer.on("/api/credential/rfid/read", HTTP_POST, handleApiCredentialGeneric);
  webServer.on("/api/ota/check", HTTP_POST, handleApiOtaGeneric);
  webServer.on("/api/ota/status", handleApiOtaGeneric);
  webServer.on("/api/ota/update", HTTP_POST, handleApiOtaGeneric);
  webServer.on("/api/ota/rollback", HTTP_POST, handleApiOtaGeneric);
  webServer.on("/api/tests/list", handleApiTestsList);
  webServer.on("/api/tests/run", HTTP_POST, handleApiTestsRun);
  webServer.on("/api/tests/run_one", HTTP_POST, handleApiTestsRunOne);
  webServer.on("/config", handleConfigPage);
  webServer.on("/live", handleLivePage);
  webServer.begin();
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
static uint32_t mqttConnectedSince = 0;

// Remote control (Ch.10 §8.5's command topic) — the same four actions the
// SIMLOCK guide specifies, dispatched here. factory_reset reuses the exact
// NVS-clear-and-restart logic as the physical BOOT-hold path
// (checkFactoryResetButton()) — one factory-reset implementation, two
// triggers, not two.
static void onMqttCommand(char *topic, uint8_t *payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
    Serial.println("[MQTT] command payload not valid JSON");
    return;
  }
  String action = doc["action"] | "";
  Serial.printf("[MQTT] command: %s\n", action.c_str());

  if (action == "start_test") {
    runFullTestSuite();
  } else if (action == "restart") {
    mqttPublishLog("INFO", "MQTT", "restart requested");
    delay(200);
    ESP.restart();
  } else if (action == "factory_reset") {
    mqttPublishLog("INFO", "MQTT", "factory_reset requested");
    prefs.begin("simlock", false);
    prefs.clear();
    prefs.end();
    delay(200);
    ESP.restart(); // does not return
  } else if (action == "check_ota") {
    mqttPublishLog("INFO", "MQTT", "check_ota requested — no OTA mechanism implemented yet");
  } else {
    mqttPublishLog("WARN", "MQTT", "unknown command action: " + action);
  }
}

static void mqttConnect() {
  if (mqttClient.connected()) return;
  mqttNetClient.setInsecure(); // bench default — no CA bundle pinned yet, see note below
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttCommand);
  mqttClient.setBufferSize(1024); // PubSubClient's 256B default silently drops larger JSON — mqtt-256-byte-cliff
  // PubSubClient's default keepalive is 15s — matches this board's exact
  // symptom (MQTT dropping ~every 15s) because loop() shares one thread
  // between webServer.handleClient() and mqttClient.loop(), and a slow/
  // hanging HTTP request starves the keepalive ping past that window.
  // 60s gives real headroom; the actual fix (below) is not starving it.
  mqttClient.setKeepAlive(60);
  const String willTopic = "test/boards/" + boardId + "/connection";
  const char *willOffline = "{\"status\":\"offline\"}";
  if (mqttClient.connect(boardId.c_str(), MQTT_USER, MQTT_PASS, willTopic.c_str(), 1, true, willOffline)) {
    mqttConnectedSince = millis();
    Serial.printf("[MQTT] connected, t=%lums\n", mqttConnectedSince);
    const String online = "{\"status\":\"online\"}";
    mqttClient.publish(willTopic.c_str(), online.c_str(), true);
    const String cmdTopic = "test/boards/" + boardId + "/command";
    mqttClient.subscribe(cmdTopic.c_str(), 1);
    Serial.printf("[MQTT] subscribed %s\n", cmdTopic.c_str());
  } else {
    Serial.printf("[MQTT] connect failed, rc=%d, t=%lums\n", mqttClient.state(), millis());
  }
}

// Seals `doc` under the board<->server shared secret and publishes as
// binary bytes (ciphertext may contain embedded zero bytes — unsafe for a
// plain C-string publish, hence the binary publish() overload). Falls back
// to a loudly-logged plaintext publish only in the bootstrap window before
// ozkit/server has ever provisioned this board — never a silent downgrade.
static void sealAndPublish(const String &topic, JsonDocument &doc) {
  String plain;
  serializeJson(doc, plain);

  if (!g_keyReady) {
    Serial.printf("[MQTT] UNSEALED publish to %s — board not yet provisioned\n", topic.c_str());
    mqttClient.publish(topic.c_str(), plain.c_str());
    return;
  }

  uint8_t key[32];
  if (!ozkitEnvKey(g_sharedSecret, boardId, "ozkit/board->server", key)) {
    Serial.println("[MQTT] key derivation failed — dropping publish rather than sending unsealed");
    return;
  }

  g_uplinkCounter++;
  uint8_t sealed[512];
  int n = ozkitEnvSeal(key, boardId, g_uplinkCounter, (const uint8_t *)plain.c_str(),
                        plain.length(), sealed, sizeof(sealed));
  if (n < 0) {
    Serial.println("[MQTT] seal failed (payload too large for sealed[512]?) — dropping publish");
    return;
  }
  // Persisted every 20th publish, not every one — mirrors the lock's own
  // counter_floor discipline (occasional extra replay window on power loss
  // is a cheaper trade than a flash write on every single heartbeat).
  if (g_uplinkCounter % 20 == 0) saveUplinkCounter();
  mqttClient.publish(topic.c_str(), sealed, (unsigned int)n);
}

static void mqttPublishHeartbeat() {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["mac"] = boardMac;
  doc["firmware_version"] = FW_VERSION;
  doc["uptime_seconds"] = millis() / 1000;
  doc["wifi"]["rssi"] = WiFi.RSSI();
  doc["wifi"]["ip"] = WiFi.localIP().toString();
  doc["profile_id"] = ozProfileId();
  doc["firmware_id"] = ozProfile()->firmware_id;
  const String topic = "test/boards/" + boardId + "/status";
  sealAndPublish(topic, doc);
}

// ── Remaining telemetry topics (Ch.10 of the SIMLOCK guide — the MQTT shape
// itself is corroborated infra, kept; see ozkit/docs/simlock-dp-reconciliation.md) ──
static void mqttPublishLog(const char *level, const char *source, const String &message) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["level"] = level;
  doc["source"] = source;
  doc["message"] = message;
  sealAndPublish("test/boards/" + boardId + "/log", doc);
}

static void mqttPublishAlert(const char *alertType, const char *severity, const String &details) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["alert_type"] = alertType;
  doc["severity"] = severity;
  doc["details"] = details;
  sealAndPublish("test/boards/" + boardId + "/alert", doc);
}

static void mqttPublishTestStart(const String &eventId, size_t total) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["event_id"] = eventId;
  doc["total"] = total;
  sealAndPublish("test/boards/" + boardId + "/test/start", doc);
}

static void mqttPublishTestResult(const TestResult &r, const char *name) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["test_id"] = r.id;
  doc["test_name"] = name;
  doc["status"] = r.status == TS_PASS ? "PASS" : r.status == TS_UNSUPPORTED ? "UNSUPPORTED"
                  : r.status == TS_TIMEOUT ? "TIMEOUT" : "FAIL";
  doc["duration_ms"] = r.durationMs;
  doc["details"] = r.detail;
  sealAndPublish("test/boards/" + boardId + "/test/result", doc);
}

static void mqttPublishTestComplete(const String &eventId, uint16_t total, uint16_t passed,
                                     uint16_t failed, uint16_t unsupported, uint32_t durationMs) {
  if (!mqttClient.connected()) return;
  JsonDocument doc;
  doc["board_id"] = boardId;
  doc["event_id"] = eventId;
  doc["summary"]["total"] = total;
  doc["summary"]["passed"] = passed;
  doc["summary"]["failed"] = failed;
  doc["summary"]["unsupported"] = unsupported;
  doc["summary"]["pass_rate"] = total ? (100.0 * passed / total) : 0;
  doc["duration_seconds"] = durationMs / 1000.0;
  sealAndPublish("test/boards/" + boardId + "/test/complete", doc);
}

// Shared by both trigger paths (REST /api/tests/run and the MQTT
// {"action":"start_test"} command) so the telemetry sequence — test/start,
// one test/result per case, test/complete — only has one implementation to
// keep correct, matching Ch.10's cadence regardless of who asked for the run.
static TestSuiteSummary runFullTestSuite() {
  const uint32_t start = millis();
  const String eventId = boardId + "_" + String(start);
  mqttPublishTestStart(eventId, kSeedSuiteCount);

  uint16_t passed = 0, failed = 0, unsupported = 0;
  for (size_t i = 0; i < kSeedSuiteCount; i++) {
    TestResult r = runTest(kSeedSuite[i]);
    mqttPublishTestResult(r, kSeedSuite[i].name);
    if (r.status == TS_PASS) passed++;
    else if (r.status == TS_UNSUPPORTED) unsupported++;
    else failed++;
  }
  TestSuiteSummary s{(uint16_t)kSeedSuiteCount, passed, failed, unsupported, millis() - start};
  mqttPublishTestComplete(eventId, s.total, s.passed, s.failed, s.unsupported, s.durationMs);
  return s;
}

// ── Status LED ───────────────────────────────────────────────────────────────
// Onboard WS2812 RGB LED on GPIO8 (per docs.waveshare.com/ESP32-C6-DEV-KIT-N8
// pinout diagram, 2026-08-17) — a real instrument needs a status light
// independent of any host connection, not just a debugging aid. Colors:
// amber=booting, cyan(blink)=AP/portal mode waiting for provisioning,
// green=WiFi+MQTT up, red(blink)=WiFi/MQTT down.
#define STATUS_LED_PIN 8
static void ledSet(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(STATUS_LED_PIN, r, g, b); }

// ── Factory reset (hold BOOT 5s) ─────────────────────────────────────────────
// Same house pattern as bridge32.ino's checkFactoryResetButton() / the
// shared version in common/ozdoorlock_core.h — matched deliberately rather
// than reinvented, per the operator (2026-08-17). This is the one recovery
// path that doesn't depend on the network being reachable at all, which
// matters here because the portal/REST path is useless if the saved Wi-Fi
// credentials are the thing that's wrong. GPIO9 is this board's BOOT button
// (Waveshare pinout diagram) — active-low, same USER_BUTTON=9 as
// doorlock.ino/bridge32.ino.
#define USER_BUTTON 9
#define FACTORY_RESET_HOLD_MS 5000UL
static unsigned long buttonHeldSince = 0;
static bool buttonWasDown = false;

static void checkFactoryResetButton() {
  bool down = digitalRead(USER_BUTTON) == LOW;
  if (down && !buttonWasDown) {
    buttonHeldSince = millis();
  } else if (down && buttonWasDown) {
    unsigned long held = millis() - buttonHeldSince;
    if (held >= FACTORY_RESET_HOLD_MS) {
      Serial.println("[RESET] BOOT held 5s — factory reset (clearing saved Wi-Fi)");
      for (int i = 0; i < 6; i++) { ledSet(i % 2 ? 40 : 0, 0, 0); delay(150); } // fast red flash = confirmed
      prefs.begin("simlock", false);
      prefs.clear();
      prefs.end();
      delay(200);
      ESP.restart(); // does not return
    } else if (held > 800 && (held / 500) % 2 == 0) {
      Serial.printf("[RESET] holding BOOT... %lus/5s\n", held / 1000);
    }
  }
  buttonWasDown = down;
}

// ── setup / loop ─────────────────────────────────────────────────────────────
static uint32_t lastHeartbeat = 0;
static uint32_t lastMqttAttempt = 0;
static uint32_t lastBlink = 0;
static bool blinkOn = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(USER_BUTTON, INPUT_PULLUP);
  ledSet(40, 20, 0); // amber — booting

  ozProfileBegin();
  // ozProfileBegin() resolves OZ_PROFILE_DEFAULT_ID — as of the 2026-08-20
  // profile restructure (XF-122) that default is "tuya-wifi-lock-pro", a
  // Tuya CATEGORY reference map with no PID of its own, NOT a real product
  // (ozkie-legacy-v0, the old fiction-map default, was deleted in the same
  // restructure). That default is firmware's, not this fixture's — per the
  // operator's per-manufacturer-build directive (2026-08-17), this build
  // targets exactly one real profile; select it explicitly rather than
  // inheriting a default meant for a different board.
  //
  // "tuya-ds013-t3" was renamed to "tuya-luona-ds013-t3" in the same
  // restructure (XF-122 confirmed Luona Smart DS013-T3, PID
  // vr4iiuqtyh0q4nix, as SIMLOCK's one real, pairable reference target —
  // Ladin's tuya-ladin-f7-t3 remains an unusable 4-DP stub, no DP reference
  // ever supplied). Silently falling through to tuya-wifi-lock-pro would
  // have run the whole seed suite against the WRONG DP semantics (e.g. its
  // DP76 is fill_light, not unlock_ble) with only one easy-to-miss log line
  // as a symptom.
  //
  // The operator can switch which compiled-in profile this bench issues
  // over UART via /config (XFtposDecisions-124 §11 — local, NVS-persisted,
  // no reflash, no network fetch). If they have, honor that choice; the
  // default below is only the first-boot / never-configured case.
  {
    String savedProfile = loadProfileSelection();
    if (savedProfile.length() && ozProfileSelect(savedProfile.c_str())) {
      Serial.printf("[BOOT] using operator-selected profile '%s' (from /config)\n", savedProfile.c_str());
    } else if (!ozProfileSelect("tuya-luona-ds013-t3")) {
      Serial.println("[BOOT] FATAL: profile 'tuya-luona-ds013-t3' not found in ozprofile_gen.h");
    }
  }
  Serial.printf("[BOOT] %s, profile '%s'%s\n", FW_VERSION, ozProfileId(),
                ozProfile()->deprecated ? " [INVENTED MAP]" : "");

  bool frameOk = ozkitTuyaFrameSelfTest();
  Serial.printf("[SELFTEST] tuya frame codec %s\n", frameOk ? "PASS" : "FAIL");

  // WiFi.macAddress() reads via esp_wifi_get_mac(), which needs the WiFi
  // driver initialized — at this point in setup() it isn't yet (that
  // happens in connectStation()/startCaptivePortal() below), so this was
  // silently returning all-zero bytes (board_id = N8-000000). esp_read_mac()
  // reads the efuse-burned factory MAC directly, valid from cold boot.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char idBuf[8];
  snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  boardId = String("N8-") + idBuf;
  char macBuf[13];
  snprintf(macBuf, sizeof(macBuf), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  boardMac = String(macBuf);
  Serial.printf("[BOOT] board_id = %s, mac = %s\n", boardId.c_str(), boardMac.c_str());

  DutUart.begin(DUT_UART_BAUD, SERIAL_8N1, DUT_RX_PIN, DUT_TX_PIN);

  // Always configured to a safe idle state regardless of dutHandshakeEnabled
  // — cheap, and means flipping the flag later doesn't also need a pin
  // re-init. INPUT_PULLUP on SRDY: idle-HIGH even if the emulator side is
  // unplugged or its own pull-up (ozkey-27 §2.0.15a's "R4 10k") isn't there.
  pinMode(DUT_SRDY_PIN, INPUT_PULLUP);
  pinMode(DUT_MRDY_PIN, OUTPUT);
  digitalWrite(DUT_MRDY_PIN, HIGH);

  loadDeviceIdentity();
  Serial.printf("[BOOT] device identity %s\n", g_keyReady ? "loaded from NVS" : "NOT PROVISIONED — telemetry unsealed until ozkit/server provisions this board");

  loadWifiConfig();
  if (!wifiProvisioned || cfgSsid.length() == 0) {
    startCaptivePortal();
  } else if (connectStation(15000)) {
    Serial.printf("[WiFi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
    // simlock.local — so finding the board on a network never again depends
    // on reading its IP off a router page or a serial console that may not
    // be reachable. One board per network for now (fixed hostname); revisit
    // if/when a fleet shares a single LAN.
    if (MDNS.begin("simlock")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("[mDNS] simlock.local");
    }
    setupRestApi();
    mqttConnect();
  } else {
    Serial.println("[WiFi] STA connect failed — falling back to captive portal");
    startCaptivePortal();
  }
}

void loop() {
  checkFactoryResetButton(); // works in AP mode too — the point is to not need the network at all

  if (millis() - lastBlink > 500) {
    blinkOn = !blinkOn;
    if (apMode) {
      ledSet(0, blinkOn ? 40 : 0, blinkOn ? 40 : 0); // cyan blink
    } else if (mqttClient.connected()) {
      ledSet(0, 30, 0); // solid green
    } else {
      ledSet(blinkOn ? 40 : 0, 0, 0); // red blink — WiFi up, MQTT down
    }
    lastBlink = millis();
  }

  if (apMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    if (millis() - lastHeartbeat > 5000) {
      Serial.printf("[AP] alive, uptime=%lus, clients=%d, heap=%u\n",
                    millis() / 1000, WiFi.softAPgetStationNum(), (unsigned)ESP.getFreeHeap());
      lastHeartbeat = millis();
    }
    return;
  }

  webServer.handleClient();

  bool mqttUpNow = mqttClient.connected();
  if (!mqttUpNow) {
    if (mqttConnectedSince) { // was up last time we checked — this IS the drop
      Serial.printf("[MQTT] dropped, rc=%d, was up %lums, t=%lums\n",
                    mqttClient.state(), millis() - mqttConnectedSince, millis());
      mqttConnectedSince = 0;
    }
    // Backoff, not retry-every-iteration: a TLS handshake retried back-to-
    // back with zero yield room between attempts is exactly what starves
    // the idle-task watchdog on this core and crashes the whole board —
    // which would ALSO look like "MQTT drops every ~15s" from the outside,
    // because the crash-reboot cycle resets everything including the LED.
    if (millis() - lastMqttAttempt > 5000) {
      mqttConnect();
      lastMqttAttempt = millis();
    }
  }
  mqttClient.loop();

  if (millis() - lastHeartbeat > 10000) {
    mqttPublishHeartbeat();
    // Same info, to Serial too — so grabbing the IP or checking connection
    // health never again depends on catching a one-time boot-log line at
    // exactly the right moment after a reset.
    Serial.printf("[STA] alive, ip=%s, rssi=%d, mqtt=%s, uptime=%lus, heap=%u\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  mqttClient.connected() ? "up" : "down",
                  millis() / 1000, (unsigned)ESP.getFreeHeap());
    lastHeartbeat = millis();
  }
}
