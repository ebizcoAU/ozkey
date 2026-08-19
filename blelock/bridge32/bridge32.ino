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
#include <openthread/thread.h>     // ozkey-20 R2: otSetStateChangedCallback, otIp6Address
#include <openthread/dataset.h>    // ozkey-20 §15.3 security-policy diagnostic
#include <openthread/thread_ftd.h> // otChildInfo / otThreadGetChildInfoByIndex
                                   // (FTD-only API) — see logThreadChildren()
#include "../common/oztime.h" // ozkey-21 T3 — OZ_TIME_FLOOR, shared with the locks
// XF-115 §7.3 — the ONE definition of a lock's presence payload. The bridge
// publishes presence ON BEHALF OF Thread locks, so it must emit byte-identical
// shape to a Wi-Fi lock publishing for itself. Forwarding the lock's internal
// datagram verbatim is what crashed BANOI1.
#include "../common/ozpresence.h"
#include "esp_coexist.h"

// How long "no clock yet" is NORMAL rather than a fault. At boot the bridge has
// no RTC, so a blank clock is expected until Wi-Fi associates and the broker
// connects. Showing that in red from the first frame is what made users read a
// healthy bridge as broken — amber until this elapses, red after.
#define OZ_TIME_GRACE_MS 60000UL
#include <OThreadUDP.h>
// ozkey-17 U2: the RECEIVE half runs on lwIP, not OThreadUDP. Not a preference —
// esp_openthread_netif_glue pushes inbound Thread packets up into lwIP, so an
// otUdp* socket never sees them at all. This was root-caused the hard way on
// 2026-07-28 (see ozdoorlock_core.h's threadUdpBegin()) after attach, subscribe
// and transmit all verified working while parsePacket() returned nothing, ever.
// This is a port of that proven receive path, not a second attempt at it.
#include <lwip/sockets.h>
#include <errno.h>
#include <fcntl.h>
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
// ozkey-20 §15.3 — CHILD is amber: on the mesh and working, but not the
// parent, so we can see no child table and the mesh is upside down.
#define LCD_C_AMBER 0xFD20

// LCD idle blank (2026-07-27): bench aid, not a status signal — the screen
// itself goes dark after LCD_IDLE_OFF_MS with nothing to check on it. Only a
// BOOT button press wakes it back up (status changes while it's off do NOT
// wake it — screen state is intentionally independent of the status ladder
// once it's gone dark, matching "only turn on if someone touch the boot
// button").
#define LCD_IDLE_OFF_MS 60000UL

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
// 1.7 (2026-08-08): 1.5's LCD_IDLE_OFF_MS disable reverted, back to 60000UL
// (60s). The bridge-ownership investigation it was disabled for closed
// 2026-08-06 (ozkey-12 §9.8) — no reason left to keep the bench diagnostic
// override in place.
// 1.8 (2026-08-08): ozkey-13 §8 BR1 — mqttMessageReceived()/forwardOverThread()/
// sendToThreadGroup() generalized to relay `envelope_hex` (sealed) alongside
// legacy `payload`/`payload_hex`, whichever arrives, under its own field
// name. Pure pass-through — this board never decodes or opens the envelope,
// same as it never understood `payload` either. Closes the gap where a
// sealed grant/delete could never reach any Thread lock behind this bridge.
// 1.9    ozkey-17 U2/U3 — the bridge can now LISTEN. Until this version
//        `threadUdp` was send-only ("sender: plain unicast bind"): no
//        parsePacket(), no read loop, nothing. It had only ever pushed commands
//        at locks and never once heard one, which is the transport half of why
//        an app cannot ask a lock anything. U2: lwIP raw-socket receive on the
//        new uplink port 5053 (a port of ozdoorlock_core.h's proven path —
//        OThreadUDP's receive half can never fire on ESP32, root-caused
//        2026-07-28). U3: republish under the LOCK's own topic
//        ozkey/<site>/locks/<from>/uplink, never the bridge's — that is what
//        makes a bridged lock indistinguishable from a WiFi one to ozlockserv,
//        so the server needs no change and stays a mailman. The bridge reads
//        only `from`; `envelope_hex` is sealed to the app and opaque here.
//   1.11 (2026-08-11) ozkey-20 R1/R2 + topology:
//        R1 — MQTT Last Will, retained, on ozkie/<site>/bridges/<id>/presence.
//             Sub-second bridge-death detection for the server, no polling.
//        R2 — Thread liveness table on .../liveness, 30 s sweep AND immediate
//             push on CHILD_ADDED/CHILD_REMOVED. Carries `role` +
//             `authoritative` so the server never reads an empty child table
//             from a non-parent bridge as "every lock is unreachable".
//        Router promotion — the bridge now asks to BE a Router (eligibility +
//             BecomeRouter, retried from loop()). Found because it was sitting
//             as a CHILD, which meant no child table at all.
//        LCD — "THREAD: OK" replaced by the actual ROLE (LEADER/ROUTER black,
//             CHILD amber, else red). "OK" hid the single most important fact
//             about this device: a doorlock (LockB) had taken Leader and the
//             border router was hanging off it.
#define FW_VERSION "bridge32-1.44"
// ── FW_DISPLAY_VERSION is DERIVED, never hand-maintained (2026-08-12) ──────
//
// It read "v1.17" while FW_VERSION said bridge32-1.31 — stale by fourteen
// bumps, so the panel had been lying about which firmware it was running for
// most of two days. The operator caught it on the LCD.
//
// This is the SECOND time: the changelog note above records it stuck at "v1.0"
// through the 1.1 bump, and the fix then was to hand-sync the two constants —
// which is what re-armed the trap. The operator's ruling was "FW_VERSION is
// the single source", so make that structurally true instead of a convention:
// derive the badge from FW_VERSION and there is nothing left to forget.
//
// "bridge32-1.33" -> "v1.31". Falls back to the whole string if the dash is
// ever missing, which is wrong but visibly wrong rather than silently stale.
static const char *fwDisplayVersion() {
  static char buf[16];
  const char *dash = strchr(FW_VERSION, '-');
  snprintf(buf, sizeof(buf), "v%s", dash ? dash + 1 : FW_VERSION);
  return buf;
}
#define FW_DISPLAY_VERSION fwDisplayVersion()

// Thread network defaults — this bridge always FORMS (never joins an
// existing mesh) in v0; it is the only network former in the home.
#define OT_CHANNEL 15

// ── State machine ───────────────────────────────────────────────────────────
enum BridgeState { ST_ADVERTISING, ST_WIFI_JOINING, ST_THREAD_FORMING, ST_OPERATIONAL };
BridgeState state = ST_ADVERTISING;

Preferences prefs; // namespace "bridge32"

// 1.38 clock persistence — declared here because the MQTT handler uses them
// well before their definitions further down. See the block above
// ozBridgeClockPersist() for why a bridge that forgets its clock strands every
// Thread lock behind it.
static bool g_utcFromServer = false; // has ozlockserv pushed utc THIS boot?
static void ozBridgeClockPersist(bool force);
static void ozBridgeClockRestore();
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
// ozkey-21 T3 — timezone offset in MINUTES east of UTC, from the app at
// pairing (XF-90 ask 1). Minutes, not hours: India is +330 and Nepal +345, and
// an hours-only field silently cannot express them.
int16_t cfgTzMin = 0;

// Set from the OpenThread state-changed callback, serviced in loop() — the
// callback runs under the OT lock and is not a safe place to transmit.
static volatile bool g_timeBeaconDue = false;

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
  // ozkey-21 — timezone, minutes east of UTC (Vietnam = 420). Sent by the app
  // at pairing. Survives reboot so a bridge that comes back with no app present
  // still shows and distributes correct local time.
  cfgTzMin = prefs.getShort("tzmin", 0);
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
  prefs.putShort("tzmin", cfgTzMin);
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
// 1.35 — WHICH clock source won, and a guard so the loser cannot undo it
//
// The bug this fixes was reported as "the datetime on the LCD is a red dash
// line", and the panel could not explain itself: it showed WIFI and THREAD —
// the two transports that are NOT the time source — and said nothing about the
// broker, which IS. So a bridge sitting healthily on Wi-Fi with a blocked NTP
// and no server push looked identical to a broken one.
//
// Underneath that was a real defect. TWO writers shared one clock with no
// arbitration:
//   • `configTime()` starts the SNTP daemon, which keeps correcting FOREVER.
//   • the server's `utc` over MQTT called settimeofday() with no guard beyond
//     the floor.
// So NTP could step the clock BACKWARDS over a value the server had already
// set, even though ozkey-21 §3.4 makes the server the source of record. The
// doorlock has exactly this protection (oztime.h ozTimeAccept: refuses
// backwards, below-floor and absurd jumps); the bridge — which FEEDS the
// doorlock its time — had none of it.
//
// The rule now:
//   server (MQTT) is authoritative. NTP fills the gap until the server speaks,
//   and once it has, SNTP is STOPPED so it can never contradict it.
//
// `OzTimeSource` itself is declared up with the includes: Arduino generates
// function prototypes and inserts them ahead of everything else in the sketch,
// so a type used in a signature must exist before that insertion point.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// THE clock row. Singular, deliberately.
//
// 🔴 1.36 — there used to be TWO renderers for this one row at y=88, and they
// disagreed about the timezone:
//   • drawStatus()          -> ozFormatStampNarrow(..., utc, cfgTzMin)  LOCAL
//   • the 1 s ticker in loop() -> ozFormatStampNarrow(..., utc)         UTC
// The ticker simply omitted the argument and picked up the default of 0. So the
// panel alternated between local time and UTC roughly once a second, reported
// from the bench as "the bridge is jumping between 2 timing systems". The
// timezone the app assigns during the ceremony was never the problem.
//
// Patching the second call site would have left the duplication that caused it,
// so both callers now come here instead. If a third thing ever needs to draw
// the clock, it calls this too.
//
// `force` is for the full-screen redraw, which has just fillScreen()'d the row
// away — the cached-string skip below would otherwise leave it blank.
// ─────────────────────────────────────────────────────────────────────────────
#define OZ_CLOCK_ROW_CHARS 16 // 4px margin + 16 * 12px = 196px, clear of the tag at x=202
static char g_lastClockRow[OZ_CLOCK_ROW_CHARS + 8] = {0};

static void drawClockRow(bool force) {
  char row[OZ_CLOCK_ROW_CHARS + 8];
  uint16_t col;
  const uint32_t utcNow = ozBridgeUtc();

  if (utcNow) {
    ozFormatStampNarrow(row, sizeof(row), utcNow, cfgTzMin);
    col = LCD_C_BLACK;
  } else if (millis() < OZ_TIME_GRACE_MS) {
    snprintf(row, sizeof(row), "CLOCK: SYNCING");
    col = LCD_C_AMBER;
  } else {
    // Name the cause rather than printing dashes. Kept within
    // OZ_CLOCK_ROW_CHARS so it cannot run into the source tag.
    snprintf(row, sizeof(row), mqttClient.connected() ? "NO SERVER TIME" : "NO BROKER");
    col = LCD_C_RED;
  }

  if (!force && strcmp(row, g_lastClockRow) == 0) return;
  strncpy(g_lastClockRow, row, sizeof(g_lastClockRow) - 1);

  // Opaque text (fg, bg) — self-erasing, so no fillRect and nothing to flicker.
  // Padded to a fixed width so a shorter string never leaves a tail behind.
  gfx->setTextSize(2);
  gfx->setCursor(4, 88);
  gfx->setTextColor(col, LCD_C_WHITE);
  gfx->print(row);
  for (size_t i = strlen(row); i < OZ_CLOCK_ROW_CHARS; i++) gfx->print(' ');

  gfx->setTextColor(LCD_C_BLACK);
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
    // ── WIFI: show the ADDRESS, not "OK" (operator layout, 2026-08-11) ────
    // "OK" is not actionable. The IP is the thing you actually need when
    // pointing a bench tool or a broker at this bridge, and its absence is a
    // more honest failure indication than a green word.
    gfx->setCursor(4, 44);
    if (WiFi.status() == WL_CONNECTED) {
      gfx->print("WIFI: ");
      gfx->println(WiFi.localIP().toString());
    } else {
      gfx->setTextColor(LCD_C_RED);
      gfx->println("WIFI: NOT CONNECTED");
      gfx->setTextColor(LCD_C_BLACK);
    }
    // ── THREAD ROLE, not "OK" (operator, 2026-08-11) ────────────────────
    //
    // "OK" hid the single most important fact about this device. A bridge
    // that is a CHILD has no child table, so ozkey-20 R2 reports zero locks
    // while every lock is alive — and the panel cheerfully said OK throughout.
    //
    // Worse, it means a battery doorlock is parenting the border router. That
    // is exactly the inversion logThreadChildren() has warned about in text
    // for weeks; nobody saw it because the screen never showed the role.
    // Confirmed on the bench today: LockB had taken Leader.
    //
    // LEADER/ROUTER are correct for a mains-powered border router. CHILD is
    // amber — working, but the mesh is upside down. Anything else is red.
    {
      const char *roleTxt = "NOT OK";
      uint16_t roleCol = LCD_C_RED;
      otInstance *inst = esp_openthread_get_instance();
      if (inst) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        const otDeviceRole r = otThreadGetDeviceRole(inst);
        esp_openthread_lock_release();
        switch (r) {
          case OT_DEVICE_ROLE_LEADER: roleTxt = "LEADER"; roleCol = LCD_C_BLACK; break;
          case OT_DEVICE_ROLE_ROUTER: roleTxt = "ROUTER"; roleCol = LCD_C_BLACK; break;
          // Amber, not black: we are ON the mesh and functional, but we are
          // hanging off someone else and cannot see the child table.
          case OT_DEVICE_ROLE_CHILD:  roleTxt = "CHILD";  roleCol = LCD_C_AMBER; break;
          case OT_DEVICE_ROLE_DETACHED: roleTxt = "DETACHED"; break;
          default: roleTxt = "NOT OK"; break;
        }
      }
      if (!threadFormed) { roleTxt = "NOT CONNECTED"; roleCol = LCD_C_RED; }
      // ── 1.35: THREAD and BROKER share this row ────────────────────────────
      //
      // The panel used to show WIFI and THREAD and nothing else — the two
      // transports that are NOT the time source — while the clock it feeds
      // every lock comes over MQTT. A bridge with Wi-Fi up, NTP blocked and no
      // broker looked exactly like a healthy one with a broken clock, which is
      // precisely the "no network / clock is wrong" confusion this fixes.
      //
      // `broker_connected` already existed at the BLE INFO endpoint; it simply
      // never reached the screen. Abbreviated to TH:/MQ: because the full words
      // do not fit beside each other at size2 on 240 px, and losing a row to
      // gain this fact would cost the clock its place.
      gfx->setCursor(4, 66);
      gfx->setTextColor(roleCol);
      gfx->print("TH:");
      gfx->print(threadFormed ? roleTxt : "DOWN");
      const bool mqUp = mqttClient.connected();
      gfx->setTextColor(LCD_C_BLACK);
      gfx->print(" MQ:");
      gfx->setTextColor(mqUp ? LCD_C_BLACK : LCD_C_RED);
      gfx->println(mqUp ? "UP" : "DOWN");
      gfx->setTextColor(LCD_C_BLACK);
    }

    // ── The clock (operator layout, 2026-08-11) ───────────────────────────
    //
    // 24 chars at size2 is 288px on a 240px panel, so this line is size1. It
    // earns its place regardless: this is the bridge that FEEDS every Thread
    // lock its time, so "does the border router actually know what time it is"
    // is now answerable by looking at it. Renders as dashes, never as 1970,
    // when NTP/MQTT have not supplied a time yet — see ozFormatStamp().
    {
      // size 2, not 1 (operator 2026-08-11: "unreadable... too small"). The
      // compact form drops the weekday and AM/PM to 19 chars, which is exactly
      // what buys the bigger font: 19 x 12 = 228 px of the 240 px panel.
      // ONE renderer for this row — see drawClockRow(). force=true because
      // fillScreen() above has just wiped it, so the cached-string skip
      // inside would otherwise leave the row blank.
      drawClockRow(true);
    }
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
      // device_id on the bottom line. The site line is gone: the clock took
      // that row, and "lab" was the least useful thing on the screen (it is
      // static, short, and never the thing you are debugging).
      gfx->setTextSize(2);
      gfx->setCursor(4, 112);
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
  // 1.37 — the bridge's own clock state, readable without the LCD.
  //
  // `tz` was previously write-only: the app set it during the ceremony and
  // nothing ever reported it back, so "what timezone does this bridge think it
  // is in" had no answer short of reading the panel. That is the same
  // read-back gap XF-95 §5.1 raises about `user_name`, and it made a wrong
  // offset undiagnosable remotely.
  //
  // `utc` is 0 when no clock has been served yet — the condition the panel
  // shows as NO SERVER TIME, exposed here so the server and app can see it too.
  doc["tz"] = cfgTzMin;
  doc["utc"] = ozBridgeUtc();

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
String mqttCommandTopic, mqttCommandTopicLegacy;
unsigned long mqttLastAttempt = 0;
#define MQTT_RETRY_MS 5000UL

// XF-115 / Q4 — the `msg_id` of the command currently being forwarded, stamped
// onto the datagram so the lock can echo it in its outcome. Empty for anything
// the bridge originates itself (time beacons).
static String g_fwdMsgId;

void mqttMessageReceived(char *topic, byte *payload, unsigned int len) {
  // Incoming WiFi/MQTT traffic — no ladder status step of its own, so wake
  // the screen explicitly here (2026-07-27).
  lcdWake();
  String body;
  body.reserve(len);
  for (unsigned int i = 0; i < len; i++) body += (char)payload[i];
  Serial.printf("[MQTT] << %s : %s\n", topic, body.c_str());

  // F4: distill to {target, payload}/{target, envelope_hex} for the Thread
  // hop. Accepts either the lean v0 shape (a bench `mosquitto_pub` test) or
  // the richer ozlockserv queue envelope (device_id/payload_hex) — S4 pins
  // the exact server-side shape; both are handled defensively until then.
  // ozkey-13 §8 BR1: `envelope_hex` (sealed) is checked first and relayed
  // under its own field name — the bridge never decodes it, same pure-
  // forward treatment `payload`/`payload_hex` always got.
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    Serial.println("[MQTT] payload not valid JSON, dropped");
    return;
  }
  // ── ozkey-21 T3 — accept UTC from our own server over MQTT ──────────────
  //
  // NTP IS NOT A DEPENDABLE TIME SOURCE ON A CUSTOMER SITE. Measured on the lab
  // network 2026-08-11: DNS resolves pool.ntp.org fine, UDP 123 times out. A
  // hotel or office that blocks outbound NTP would silently leave every lock
  // with no clock, and therefore every temporary PIN unenforceable — the exact
  // defect ozkey-21 exists to fix, reintroduced by the network.
  //
  // This connection, by contrast, provably works: if the bridge cannot reach
  // our server there is no product anyway. So the server is the time source of
  // record and NTP is the optimisation. `set_time` is also what lets the bench
  // drive the whole chain without waiting on either.
  //
  // Trust: same as the beacon it feeds — the lock's monotonic-forward rule and
  // the 400-day cap bound what any bad value can do. Broker ACLs are the real
  // gate here and are still a pre-production blocker (ozkey-13 S8/S9).
  const uint32_t utcIn = doc["utc"] | 0UL;
  if (utcIn >= OZ_TIME_FLOOR) {
    struct timeval tv = { .tv_sec = (time_t)utcIn, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    const bool firstReal = !g_utcFromServer;
    g_utcFromServer = true; // a REAL source spoke — see the provenance note
    Serial.printf("[TIME] utc=%lu accepted from server/bench over MQTT%s\n",
                  (unsigned long)utcIn,
                  firstReal ? " (first real sync this boot)" : "");
    // Force the write on the first genuine sync rather than waiting out the
    // hourly throttle: a reboot in the next hour would otherwise fall back to
    // whatever stale value we booted with.
    ozBridgeClockPersist(firstReal);
    // Push it straight out rather than waiting up to 24 h for the next beacon.
    sendTimeBeacon();
  }

  // ── 1.37 — accept `tz` on this topic too (ozkey-27 §9.3(2)) ──────────────
  //
  // Until now the timezone reached this bridge through exactly one door: the
  // BLE provisioning ceremony (validModePayload -> cfgTzMin -> NVS). There was
  // no way to correct it afterwards — not for DST, not for a site that moves,
  // not for a value the app got wrong — short of re-running the whole ceremony
  // on a mounted, working bridge. Server team has the publish side ready and
  // was waiting on this half.
  //
  // PANEL-ONLY, and that is not an implementation detail. We serve UTC to the
  // MCU and apply the offset only for display (ozkey-21 §8.4): DP 21/23
  // from/to are true UTC epochs, so serving genuine local time to the MCU would
  // shift every temporary credential by the offset — 7 h in Vietnam, 10 h here
  // — with nothing on our side able to detect it. The command 0x1C is named
  // GET_LOCAL_TIME, which is a trap, not a guide. Do not "fix" this.
  //
  // Accepts `tz_offset_min` as an alias, matching validModePayload() so the
  // same key works on both doors in.
  {
    JsonVariantConst tzv = doc["tz"];
    if (tzv.isNull()) tzv = doc["tz_offset_min"];
    // isNull() rather than `| 0`: 0 is a LEGITIMATE offset (UTC), so a default
    // cannot distinguish "absent" from "explicitly UTC". Getting that wrong
    // would silently reset the panel to UTC on every utc-only refresh.
    if (!tzv.isNull() && tzv.is<int>()) {
      const int tzIn = tzv.as<int>();
      // Real-world offsets span UTC-12:00 to UTC+14:00. Anything outside that
      // is a unit error — most likely HOURS sent where MINUTES were meant,
      // which is the failure mode worth catching by name.
      if (tzIn < -720 || tzIn > 840) {
        Serial.printf("[TIME] tz=%d REJECTED — outside -720..+840 min "
                      "(hours sent as minutes?)\n", tzIn);
      } else if ((int16_t)tzIn != cfgTzMin) {
        cfgTzMin = (int16_t)tzIn;
        // Only on CHANGE. The server refreshes utc every ~10 min and tz rides
        // along, so an unconditional putShort() would be ~150 NVS writes a day
        // for a value that changes twice a year at most.
        saveConfig();
        Serial.printf("[TIME] tz=%+d min (%+.2f h) accepted over MQTT, saved\n",
                      cfgTzMin, cfgTzMin / 60.0);
        // Locks render their own panels from this, so push it now rather than
        // letting them sit up to 24 h on the old offset.
        sendTimeBeacon();
      }
    }
  }

  // ── XF-91/92 — REMOTE FACTORY RESET OF THE BRIDGE ITSELF ────────────────
  //
  // This did not exist. Not a routing bug like the lock's (XF-91) — the bridge
  // had NO remote reset at all, over any transport: `grep factory_reset`
  // returned nothing in this file. So "remove the bridge" in the app could
  // never have wiped it, and the operator was left with a bridge that still
  // owned a mesh nobody could see in the app.
  //
  // AUTHENTICATION — the ownership rule we already agreed, not a new one.
  // CONTRACT-BRIDGE.md (XF-47 round 6): "Touching an unowned bridge requires
  // physical presence. Touching an owned bridge requires a matching app_id."
  // An owned bridge therefore demands a matching `app_id`; an unowned one has
  // nothing to protect and no owner to check against.
  //
  // ⚠ WEAKER THAN THE LOCK'S SEALED PATH, deliberately and with eyes open.
  // The lock takes a sealed envelope because it holds per-bond keys. The
  // bridge holds none — it relays `envelope_hex` without ever decoding it, by
  // design (relay, not authority). Sealing this would mean giving the bridge
  // crypto authority over itself, which is a real design change, not a flag.
  // `app_id` is an identifier, not a secret, so on a broker with no ACLs this
  // is a speed bump. That is acceptable ONLY because the same broker already
  // lets anyone unlock every door (ozkey-13 S8/S9 — a fabricated username
  // still publishes); broker ACLs are the actual fix and remain a
  // pre-production blocker. Do not read this as "authenticated".
  {
    const char *op = doc["op"] | (const char *)nullptr;
    if (op && (strcmp(op, "factory_reset") == 0 || strcmp(op, "unpair") == 0)) {
      // Reuse the AUDITED guard rather than the ad-hoc check I first wrote.
      // My version only compared app_id when an owner existed, which silently
      // allowed ANY remote client to wipe an UNOWNED bridge — and XF-47 round 6
      // closed exactly that hole for provisioning: an unowned-but-deployed
      // bridge is still relaying a live mesh, so it needs the physical claim
      // window, not a free pass. bridgeOwnershipCheck() already encodes the
      // whole table (claim window, BRIDGE_CLAIM_REQUIRED, idempotent match,
      // BRIDGE_DENIED) and is the same gate the BLE reset uses.
      const String reqApp = (const char *)(doc["app_id"] | "");
      if (!bridgeOwnershipCheck(reqApp)) {
        Serial.printf("[RESET] REFUSED over MQTT (app_id '%s')\n",
                      reqApp.length() ? reqApp.c_str() : "(absent)");
        // ── SAY NO OUT LOUD (ozkey-25 §3, 2026-08-12) ────────────────────
        //
        // bridgeOwnershipCheck() already called notifyStatus("BRIDGE_DENIED")
        // — but notifyStatus() is a BLE characteristic notify, and during an
        // MQTT reset there is no BLE client connected. The refusal was being
        // computed and then announced down a transport nobody was listening
        // on, so a real denial and a message the bridge never received were
        // wire-identical: both silence. The server could only ever call that
        // `unknown`.
        //
        // ⚠ NOT RETAINED, deliberately — server team's ozkey-25 §3 proposed
        // reusing this topic retained. This topic's retained value is the
        // bridge's LIVENESS STATE OF RECORD (see the LWT + clear-on-connect
        // dance at :1625-1638, whose own comment warns that a stale retained
        // value "would call a live bridge dead"). A refusal is an EVENT, not a
        // state: retaining it would overwrite liveness, and every later reader
        // would see a denied reset as the bridge's current condition. The
        // server's waiter is connected and live for its 5 s window, so a
        // non-retained publish reaches it and leaves no residue.
        //
        // `state:"online"` because we are still running with the mesh intact —
        // only a SUCCESSFUL reset goes offline. id/role included so it parses
        // through the same handleBridgePresence() path as every other message
        // on this topic.
        if (mqttClient.connected()) {
          const String t = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/presence";
          const String denied = String("{\"state\":\"online\",\"id\":\"") + deviceId +
                                "\",\"role\":\"bridge\",\"reason\":\"factory_reset_denied\"}";
          mqttClient.publish(t.c_str(), denied.c_str(), false /*NOT retained*/);
        }
        return;
      }
      Serial.println("[RESET] remote factory_reset accepted over MQTT");
      // Say so BEFORE wiping: factoryReset() ends in a platform reset and
      // never returns, so anything published after it is never published.
      // Same ordering rule as the lock's MCU reset ack (ozkey-22 R1).
      if (mqttClient.connected()) {
        const String t = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/presence";
        mqttClient.publish(t.c_str(), "{\"state\":\"offline\",\"reason\":\"factory_reset\"}", true);
      }
      factoryReset(); // never returns
      return;
    }
  }

  String target = (const char *)(doc["target"] | "");
  if (!target.length()) target = (const char *)(doc["device_id"] | "");

  // A pure time message carries no target and is not a relay command — stop
  // here rather than logging a bogus "command missing target/payload" drop.
  if (utcIn && !target.length()) return;

  String fieldName = "envelope_hex";
  String valueHex = (const char *)(doc["envelope_hex"] | "");
  if (!valueHex.length()) {
    fieldName = "payload";
    valueHex = (const char *)(doc["payload"] | "");
    if (!valueHex.length()) valueHex = (const char *)(doc["payload_hex"] | "");
  }

  // Show it on the panel for LCD_RX_FLASH_MS, then the footer restores itself
  // (loop() redraws once when lcdRxFlashUntil lapses).
  lcdRxMsg = target.length() ? target : String("(no target)");
  lcdRxFlashUntil = millis() + LCD_RX_FLASH_MS;
  lcdRxFlashActive = true;
  lcdWake(); // redraws immediately with the banner

  // XF-115 / Q4 — carry the request id down to the lock. Until 1.41 the bridge
  // held `msg_id` (it arrives on every ozlockserv command) and DROPPED it here,
  // so a Thread lock could not name the request its reset outcome answered and
  // server's pendingLockResets waiter could never settle for one.
  g_fwdMsgId = (const char *)(doc["msg_id"] | "");
  forwardOverThread(target, fieldName, valueHex);
  g_fwdMsgId = "";
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
// ozkey-17 U1/U2: uplink (lock -> bridge) has its own port. Downlink stays on
// 5052. Two directions on one port would make every receiver decide "command
// for me, or my own echo off the multicast group?" per datagram — structural
// separation is cheaper and cannot be got subtly wrong.
const uint16_t OZ_THREAD_UPLINK_PORT = 5053;

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

// ─────────────────────────────────────────────────────────────────────────────
// U2/U3 (ozkey-17 §6): the uplink receive path — the capability this bridge has
// never had.
//
// Until now `threadUdp` was send-only ("sender: plain unicast bind"): no
// parsePacket(), no read loop, nothing. The bridge has only ever pushed
// commands AT locks and never once listened to one. That is the transport half
// of why an app can't ask a lock anything, and why the 2026-08-09 session spent
// seven XF docs compensating for state nobody could observe.
//
// U3's routing rule is the load-bearing part: received uplink is republished
// under the LOCK's own topic (ozkey/<site>/locks/<from>/uplink), never the
// bridge's. ozlockserv already subscribes ozkey/<site>/locks/+/... wildcards, so
// it cannot tell a bridged lock's message from a WiFi lock's — which is exactly
// what lets the server need zero changes and stay a mailman. The bridge reads
// only the routing fields; `envelope_hex` is sealed to the app and opaque to
// every hop in between, this one included.
// ─────────────────────────────────────────────────────────────────────────────
#define OZ_UPLINK_RX_BUF 1024 // matches the lock's OZ_UDP_RX_BUF after F8's bump

int ozUplinkRxFd = -1;
bool uplinkRxReady = false;
unsigned long uplinkRxLastAttempt = 0;

void uplinkRxBegin() {
  if (uplinkRxReady) return;
  uplinkRxLastAttempt = millis();

  otInstance *inst = thread.getInstance();
  if (inst == nullptr) return; // Thread not up yet — loop() re-polls

  // Subscribe at the OpenThread layer so the stack accepts the frame off the
  // radio at all. Bounded lock wait: a busy OT task costs a retry, not a stall.
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) return;
  otIp6Address grp;
  memcpy(grp.mFields.m8, OZ_THREAD_GROUP_BYTES, 16);
  const otError e = otIp6SubscribeMulticastAddress(inst, &grp);
  esp_openthread_lock_release();
  if (e != OT_ERROR_NONE && e != OT_ERROR_ALREADY)
    Serial.printf("[UPLINK] subscribe ff03::4f5a failed: %d (continuing)\n", (int)e);

  ozUplinkRxFd = lwip_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (ozUplinkRxFd < 0) {
    Serial.printf("[UPLINK] socket() failed errno=%d\n", errno);
    return;
  }
  int on = 1;
  lwip_setsockopt(ozUplinkRxFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in6 sa6;
  memset(&sa6, 0, sizeof(sa6));
  sa6.sin6_family = AF_INET6;
  sa6.sin6_port = htons(OZ_THREAD_UPLINK_PORT);
  if (lwip_bind(ozUplinkRxFd, (struct sockaddr *)&sa6, sizeof(sa6)) != 0) {
    Serial.printf("[UPLINK] bind() failed errno=%d\n", errno);
    lwip_close(ozUplinkRxFd);
    ozUplinkRxFd = -1;
    return;
  }
  // Non-blocking, so pollUplinkUdp() never stalls loop().
  const int fl = lwip_fcntl(ozUplinkRxFd, F_GETFL, 0);
  lwip_fcntl(ozUplinkRxFd, F_SETFL, fl | O_NONBLOCK);

  // Join at the lwIP layer too: OpenThread's subscription makes the stack
  // accept the frame, this makes lwIP deliver it to THIS socket. Interface 0 =
  // default. Non-fatal on failure — unicast still works.
  struct ipv6_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  memcpy(&mreq.ipv6mr_multiaddr, OZ_THREAD_GROUP_BYTES, 16);
  mreq.ipv6mr_interface = 0;
  if (lwip_setsockopt(ozUplinkRxFd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq,
                      sizeof(mreq)) != 0)
    Serial.printf("[UPLINK] IPV6_JOIN_GROUP failed errno=%d (unicast still live)\n",
                  errno);

  uplinkRxReady = true;
  Serial.printf("[UPLINK] listening on port %u (group ff03::4f5a) fd=%d\n",
                OZ_THREAD_UPLINK_PORT, ozUplinkRxFd);
}

// 1.10 — DRAIN the socket, do not sample it. This was the real cause of the
// uplink losses, and it was here rather than on the lock.
//
// Until now this read exactly ONE datagram per call, and loop() ends with
// delay(50) — so the bridge consumed at most one datagram per ~50 ms. Once
// doorlock-1.28 added retry bursts, a single roster change sends NINE
// datagrams (3 bursts x unicast + ff03::1 + ff03::4f5a) inside ~190 ms.
// lwIP's UDP receive queue is only a few datagrams deep, so the remainder
// overflowed and were dropped silently — nothing logs a full mbox.
//
// Measured 2026-08-10: 9 sent, 2 relayed, and relay latency stretched from
// ~150 ms to ~470 ms as the survivors queued behind the poll. The retry
// hardening I added to FIX a loss was making it worse, because more
// datagrams against a fixed 20/s drain rate is strictly more overflow.
//
// Bounded, so a flood cannot starve the rest of loop(): drain up to N per
// pass, then yield and continue on the next. At 20 passes/second that is
// still 320 datagrams/s, far above anything the mesh can deliver.
#define OZ_UPLINK_DRAIN_MAX 16

static bool pollUplinkOne(); // fwd — defined below
void pollUplinkUdp() {
  if (!uplinkRxReady || ozUplinkRxFd < 0) return;
  for (int drained = 0; drained < OZ_UPLINK_DRAIN_MAX; drained++) {
    if (!pollUplinkOne()) return; // EWOULDBLOCK — socket empty, normal case
  }
  // Hit the cap with more waiting: say so. Silence here would look identical
  // to an idle socket, which is exactly the blind spot that hid this bug.
  Serial.printf("[UPLINK] drained %d in one pass, more may be queued\n",
                OZ_UPLINK_DRAIN_MAX);
}

// ─────────────────────────────────────────────────────────────────────────────
// ozkey-20 R2 — Thread liveness table
//
// The bridge is the ONLY component that can see whether a Thread lock is
// reachable. The locks cannot report their own absence, and the server cannot
// see the mesh at all. Today none of it is published, which is why
// `lock_unreachable` has no data source and every Thread lock is invisible.
//
// Cost: ZERO mesh traffic. `mAge` is maintained by MLE link management that
// happens whether we ask or not; we are reading a table, not probing nodes.
// One Wi-Fi publish covers every lock behind this bridge — the alternative
// (each lock heartbeating fast enough to detect) does not fit the airtime
// budget at all (ozkey-20 §4.1).
// ─────────────────────────────────────────────────────────────────────────────
// 30 s: fast enough that a stale child is visible well inside the ~90 s
// `lock_unreachable` target (ozkey-20 §10 Q1), slow enough to be invisible on
// Wi-Fi. Not a mesh cost at any value — this reads a local table.
#define OZ_LIVENESS_INTERVAL_MS 30000UL
static unsigned long g_lastLivenessAt = 0;
static volatile bool g_livenessPushDue = false;

// Same helper the lock has (ozdoorlock_core.h) and for the same reason: a log
// that names a destination's LABEL rather than its address cannot be used to
// diagnose a delivery failure.
static String ozIp6Str(const uint8_t a[16]) {
  char b[48];
  snprintf(b, sizeof(b),
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
           a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
  return String(b);
}

#define OZ_LOCKMAP_MAX 32
struct OzLockAddr {
  char     deviceId[24];
  uint8_t  ext[8];   // Thread extended address — the join key. See ozNoteLockExt.
  bool     used;
  // ── ozkey-34/35 D2 — the unicast downlink address ─────────────────────────
  //
  // Learned from the SOURCE ADDRESS of this lock's own uplinks. Deliberately
  // RAM-only, not added to the persisted 32-byte record: an address is the one
  // thing in here that legitimately changes when a lock re-attaches, and a
  // stale address restored from NVS after a bridge reboot would send every
  // command into a hole. Locks beacon every 60 s, so the map repopulates on
  // its own within one interval of a restart — `ext` is persisted because
  // Thread identity is stable, `ip6` is not because addresses are not.
  uint8_t  ip6[16];
  bool     haveIp6;
};
static OzLockAddr g_lockMap[OZ_LOCKMAP_MAX];

// Remember where a lock's uplinks come from, so downlinks can go back the same
// way. Called on EVERY uplink (sealed or beacon) — cheap, and an address that
// changed is exactly the case we must not miss.
static void ozNoteLockIp6(const char *deviceId, const struct sockaddr_in6 *src) {
  if (!deviceId || !src) return;
  for (int i = 0; i < OZ_LOCKMAP_MAX; i++) {
    if (!g_lockMap[i].used) continue;
    if (strncmp(g_lockMap[i].deviceId, deviceId, sizeof(g_lockMap[i].deviceId)) != 0)
      continue;
    if (g_lockMap[i].haveIp6 &&
        memcmp(g_lockMap[i].ip6, src->sin6_addr.s6_addr, 16) == 0)
      return; // unchanged, the overwhelmingly common case — say nothing
    memcpy(g_lockMap[i].ip6, src->sin6_addr.s6_addr, 16);
    g_lockMap[i].haveIp6 = true;
    Serial.printf("[UNICAST] %s reachable at %s\n", deviceId,
                  ozIp6Str(g_lockMap[i].ip6).c_str());
    return;
  }
}

// Returns true and fills [out] if we know where this lock is.
static bool ozLockIp6(const char *deviceId, IPAddress &out) {
  for (int i = 0; i < OZ_LOCKMAP_MAX; i++) {
    if (!g_lockMap[i].used || !g_lockMap[i].haveIp6) continue;
    if (strncmp(g_lockMap[i].deviceId, deviceId, sizeof(g_lockMap[i].deviceId)) != 0)
      continue;
    out = IPAddress(IPv6, g_lockMap[i].ip6);
    return true;
  }
  return false;
}

// Record device_id ↔ source address. Newest wins for a given device_id: a lock
// that re-attaches gets a new address and the stale one must not linger, or we
// would join liveness to an identity that moved.
// ── NVS persistence for the lock map ────────────────────────────────────────
//
// 🔴 ADDED 2026-08-11. The map was RAM-only, so EVERY bridge reboot erased it
// — and locks only uplink on a roster change, which is rare. So after any
// restart the join stayed empty until someone happened to revoke something,
// and every liveness report went out with no `id`.
//
// This is the SAME mistake already made and already fixed on the lock side:
// ozkey-19 R2 persisted the uplink peer address for exactly this reason. I
// repeated it here and did not notice, because I was debugging the matching
// logic instead of asking why the map was empty.
//
// Keyed by the interface identifier (8 bytes, stable across prefix changes),
// value is the device_id. Small, bounded, and written only on change.
static void ozSaveLockMap() {
  prefs.begin("bridge32", false);
  uint8_t blob[OZ_LOCKMAP_MAX * 32];
  size_t n = 0;
  for (int i = 0; i < OZ_LOCKMAP_MAX && n + 32 <= sizeof(blob); i++) {
    if (!g_lockMap[i].used) continue;
    memcpy(blob + n, g_lockMap[i].ext, 8);           // extended address
    memset(blob + n + 8, 0, 24);
    snprintf((char *)(blob + n + 8), 24, "%s", g_lockMap[i].deviceId);
    n += 32;
  }
  prefs.putBytes("lockmap", blob, n);
  prefs.end();
}

static void ozLoadLockMap() {
  prefs.begin("bridge32", true);
  const size_t len = prefs.getBytesLength("lockmap");
  uint8_t blob[OZ_LOCKMAP_MAX * 32];
  size_t got = 0;
  if (len && len <= sizeof(blob)) got = prefs.getBytes("lockmap", blob, sizeof(blob));
  prefs.end();
  int n = 0;
  for (size_t off = 0; off + 32 <= got && n < OZ_LOCKMAP_MAX; off += 32, n++) {
    g_lockMap[n].used = true;
    memcpy(g_lockMap[n].ext, blob + off, 8);
    snprintf(g_lockMap[n].deviceId, sizeof(g_lockMap[n].deviceId), "%s",
             (const char *)(blob + off + 8));
  }
  if (n) Serial.printf("[LIVENESS] restored %d lock identity(ies) from NVS\n", n);
}

// Learn device_id <-> Thread extended address, stated by the lock itself in
// its uplink (`ext`). Replaces two failed approaches: matching the uplink's
// source address against the child's registered IPv6 addresses (children
// register NONE — measured, zero returned for every child), and deriving it
// from the MAC (the extended address is random, not MAC-derived).
//
// This one cannot fail on addressing: the bridge already holds every child's
// mExtAddress, so the join is a direct 8-byte compare.
static void ozNoteLockExt(const char *deviceId, const uint8_t ext[8]) {
  int free = -1;
  for (int i = 0; i < OZ_LOCKMAP_MAX; i++) {
    if (!g_lockMap[i].used) { if (free < 0) free = i; continue; }
    if (strncmp(g_lockMap[i].deviceId, deviceId, sizeof(g_lockMap[i].deviceId)) == 0) {
      // Only write NVS when it actually changes — an uplink arrives far more
      // often than a device's Thread identity does.
      if (memcmp(g_lockMap[i].ext, ext, 8) != 0) {
        memcpy(g_lockMap[i].ext, ext, 8);
        ozSaveLockMap();
      }
      return;
    }
  }
  if (free < 0) return; // table full — 32 locks per bridge is well past design
  g_lockMap[free].used = true;
  snprintf(g_lockMap[free].deviceId, sizeof(g_lockMap[free].deviceId), "%s", deviceId);
  memcpy(g_lockMap[free].ext, ext, 8);
  {
    char h[17];
    for (int k = 0; k < 8; k++) snprintf(h + k * 2, 3, "%02x", ext[k]);
    Serial.printf("[LIVENESS] learned %s = ext %s (persisted)\n", deviceId, h);
  }
  ozSaveLockMap();
}

// Match on the INTERFACE IDENTIFIER (last 8 bytes), not the whole address.
//
// 🔴 FIXED 2026-08-11 after a live false-alarm. The full-address compare looked
// obviously right and was wrong: the mesh-local PREFIX changes whenever the
// partition re-forms, while the interface ID is stable per device. Observed in
// one capture — the same bridge IID `ad0f:fec6:645e:7b4e` appeared under three
// different prefixes (fd51:…, fde0:…, fd7e:…) within minutes.
//
// So the bridge learned a lock's address, the prefix moved, the compare stopped
// matching, and every liveness report went out with NO `id`. Downstream the
// server could not match any reported child to a known lock and logged
// "2 reported, 0 updated, 3 inferred lost" — inventing three dead locks from a
// perfectly healthy mesh.
//
// The IID survives re-parenting and partition changes, which is exactly the
// property a join key needs.
static const char *ozLockIdForExt(const uint8_t ext[8]) {
  for (int i = 0; i < OZ_LOCKMAP_MAX; i++)
    if (g_lockMap[i].used && memcmp(g_lockMap[i].ext, ext, 8) == 0)
      return g_lockMap[i].deviceId;
  return nullptr;
}

static void ozHexExt(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static void publishThreadLiveness();

// OpenThread state-change callback. Fires for many flags; we care about
// exactly two. Deliberately does NOT publish inline — this runs on
// OpenThread's own task with its lock held, and publishThreadLiveness()
// re-acquires that lock. Setting a flag for loop() to service avoids a
// self-deadlock that would only show up when a child actually attached.
static void ozThreadStateChanged(otChangedFlags flags, void *) {
  if (flags & (OT_CHANGED_THREAD_CHILD_ADDED | OT_CHANGED_THREAD_CHILD_REMOVED)) {
    g_livenessPushDue = true;
    // ozkey-21 — a child that just attached may be a lock that just REBOOTED,
    // and a rebooted lock has no clock. Without this it waits up to 24 h for
    // the next scheduled beacon while every temporary credential on it is
    // unenforceable. The event already exists for ozkey-20 R2; reusing it costs
    // one datagram and removes the whole window.
    if (flags & OT_CHANGED_THREAD_CHILD_ADDED) g_timeBeaconDue = true;
    Serial.printf("[LIVENESS] child %s — pushing\n",
                  (flags & OT_CHANGED_THREAD_CHILD_ADDED) ? "ADDED" : "REMOVED");
  }
}

// Which locks we have already published a retained `online` for. Declared here
// rather than beside its helpers further down because publishThreadLiveness()
// below also reads it, to retract claims for children that have gone (XF-116
// §3(2)) — and the Arduino preprocessor auto-prototypes functions, not data.
// See the fuller note on ozClaimLockOnline() for why the table exists at all.
#define OZ_BR_MAX_CHILDREN 16
static String g_onlineLocks[OZ_BR_MAX_CHILDREN];

// 1.44 — how many datagrams the de-dup guard in pollUplinkOne() has swallowed,
// published on the liveness topic. Declared here for the same reason as the
// table above: publishThreadLiveness() reads it and Arduino auto-prototypes
// functions, not data.
//
// WHY THIS COUNTER EXISTS: 1.43 shipped with its only witness being a
// Serial.printf on the bridge, and this bridge's USB CDC output drops the start
// of most lines. So when the duplicates stopped, there was no way to tell
// whether the guard had caught them or whether none had arrived — the fix
// looked identical to the bug not occurring. A suppression that cannot be
// observed is not a verifiable fix, and MQTT is the transport we can actually
// read. If this stays at 0 while duplicates persist, the guard is not firing.
static uint32_t g_dupSuppressed = 0;

// Walk the child table and publish one report for the whole mesh.
static void publishThreadLiveness() {
  if (!mqttClient.connected()) return;
  otInstance *inst = esp_openthread_get_instance();
  if (!inst) return;

  String locks;
  int n = 0;
  // XF-116 §3(2) — who we can still see this pass, so we can retract stale
  // `online` claims below. Bounded by the same table that bounds the claims.
  String seen[OZ_BR_MAX_CHILDREN];
  uint8_t seenN = 0;
  esp_openthread_lock_acquire(portMAX_DELAY);
  // OUR OWN ROLE — decisive for how the server must read this report.
  // otThreadGetChildInfoByIndex() is LOCAL TO A PARENT. A bridge that is
  // itself a Child has no child table, so it reports 0 children while every
  // lock is perfectly alive. Reporting the count without the role invites
  // exactly the wrong conclusion.
  const otDeviceRole role = otThreadGetDeviceRole(inst);
  otChildInfo ci;
  for (uint16_t i = 0; i < 64; i++) {
    if (otThreadGetChildInfoByIndex(inst, i, &ci) != OT_ERROR_NONE) continue;

    // Resolve Thread identity -> device_id by matching any of this child's
    // registered IPv6 addresses against what we learned from its uplinks.
    const char *id = ozLockIdForExt(ci.mExtAddress.m8);
    otChildIp6AddressIterator it = OT_CHILD_IP6_ADDRESS_ITERATOR_INIT;
    otIp6Address a;
    // DIAGNOSTIC (2026-08-11): the join keeps failing even though the learn
    // succeeds, so print what we are actually comparing. The child registers a
    // SUBSET of its addresses with its parent, and the address a lock happens
    // to send an uplink FROM may not be in that subset — which is a guess, and
    // this log is how we stop guessing.
    (void)it; (void)a; // address iteration abandoned — children register none

    char ext[17];
    ozHexExt(ci.mExtAddress.m8, 8, ext);

    if (id && seenN < OZ_BR_MAX_CHILDREN) seen[seenN++] = id;

    if (n++) locks += ",";
    locks += "{";
    if (id) { locks += "\"id\":\""; locks += id; locks += "\","; }
    // Always carry the extended address. An unidentified child is still a
    // REAL node and must not be silently dropped from the report — reporting
    // it unnamed is honest; omitting it would understate the mesh.
    locks += "\"ext\":\""; locks += ext; locks += "\",";
    locks += "\"age_s\":" + String((unsigned long)ci.mAge) + ",";
    locks += "\"rssi\":" + String((int)ci.mAverageRssi) + ",";
    locks += "\"lqi\":" + String((unsigned)ci.mLinkQualityIn) + ",";
    locks += "\"rx_on\":"; locks += ci.mRxOnWhenIdle ? "true" : "false";
    locks += ",\"state\":\"child\"}";
  }
  esp_openthread_lock_release();

  // NOTE ON WHAT THIS CANNOT SEE: children only. A lock that has aged out of
  // the table entirely is simply absent here — the SERVER decides that absence
  // means `lost`, because only it knows which locks are supposed to exist.
  // The bridge reporting "lost" for something it has never heard of would be
  // guessing.
  const char *roleName = (role == OT_DEVICE_ROLE_LEADER)     ? "leader"
                         : (role == OT_DEVICE_ROLE_ROUTER)   ? "router"
                         : (role == OT_DEVICE_ROLE_CHILD)    ? "child"
                         : (role == OT_DEVICE_ROLE_DETACHED) ? "detached"
                                                             : "disabled";
  // `authoritative` is the field the server must gate on: only a Router or
  // Leader owns a child table worth believing. Anything else means "I cannot
  // see the mesh from here", which is NOT the same as "no locks are alive".
  const bool authoritative =
      (role == OT_DEVICE_ROLE_LEADER || role == OT_DEVICE_ROLE_ROUTER);

  String payload = "{\"kind\":\"thread_liveness\",\"bridge_id\":\"" + deviceId +
                   "\",\"role\":\"" + roleName + "\",\"authoritative\":" +
                   (authoritative ? "true" : "false") +
                   ",\"children\":" + String(n) +
                   // 1.44 — see g_dupSuppressed. Rides the report that already
                   // exists rather than adding a topic; it is bridge health,
                   // which is what this topic is for.
                   ",\"dup_suppressed\":" + String(g_dupSuppressed) +
                   ",\"locks\":[" + locks + "]}";
  const String topic = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/liveness";
  const bool ok = mqttClient.publish(topic.c_str(), payload.c_str());

  // ── XF-116 §3(2) — retract our OWN stale `online` claims ─────────────────
  //
  // We publish a RETAINED `online` when a lock beacons. Nothing ever cleared
  // it, so a Thread lock that dies quietly leaves "online" on its topic
  // forever: not a signal, a permanent false statement. If you publish a
  // retained X you owe whatever publishes not-X.
  //
  // 🔴 SCOPE, deliberately narrow. The note above ("a lock that has aged out
  // is simply absent here — the SERVER decides that absence means lost")
  // still stands and this does not violate it. We are not declaring locks
  // lost, and we say nothing about locks we have never heard of. We retract
  // exactly the claims THIS bridge made, which is the one thing no other
  // party can do for us.
  //
  // Gated on `authoritative`: a bridge that is itself a Child has no child
  // table, so every lock would look absent and we would retract the entire
  // site on one bad pass. Same trap the `authoritative` field exists to warn
  // the server about — we must not fall into it ourselves.
  if (authoritative) {
    for (uint8_t i = 0; i < OZ_BR_MAX_CHILDREN; i++) {
      if (g_onlineLocks[i].length() == 0) continue;
      bool stillHere = false;
      for (uint8_t j = 0; j < seenN; j++)
        if (seen[j] == g_onlineLocks[i]) { stillHere = true; break; }
      if (stillHere) continue;

      const String gone = g_onlineLocks[i];
      const String out = ozBuildLockPresence(gone, false, OZ_PRESENCE_LWT,
                                             String(""));
      const String ptopic =
          "ozkie/" + cfgSiteId + "/locks/" + gone + "/presence";
      mqttClient.publish(ptopic.c_str(), out.c_str(),
                         ozPresenceShouldRetain(false));
      g_onlineLocks[i] = ""; // forget, so a return beacon re-announces
      Serial.printf("[PRESENCE] %s -> offline (left the child table)\n",
                    gone.c_str());
    }
  }
  // ozkey-21 T3 diagnostic: utc=0 means NTP has not answered, so no beacon has
  // gone out and every Thread lock is still clock=UNKNOWN. The [TIME] lines are
  // one-shot at boot, which makes them useless for answering "is it working
  // now?" — this is periodic on purpose.
  Serial.printf("[LIVENESS] role=%s authoritative=%s %d child(ren) utc=%lu -> %s%s\n",
                roleName, authoritative ? "yes" : "NO", n,
                (unsigned long)ozBridgeUtc(), topic.c_str(),
                ok ? "" : " PUBLISH FAILED");
}

// ozkey-20 §15.3 — keep asking to be a Router until we are one.
//
// otThreadBecomeRouter() fails with OT_ERROR_INVALID_STATE while detached and
// OT_ERROR_NONE only starts the process; promotion completes asynchronously.
// So this retries on a slow tick and stops the moment the role is right.
// Cheap, and it self-heals after a partition merge demotes us.
static unsigned long g_lastRouterTryAt = 0;
static uint8_t g_routerTries = 0;
#define OZ_ROUTER_RETRY_MS 20000UL
// 3 x 20 s = ~1 min of asking nicely before taking the partition. Long enough
// that a normal promotion has every chance; short enough that a bench or a
// customer site is not left with a doorlock leading the mesh.
#define OZ_ROUTER_TRIES_BEFORE_TAKEOVER 3

static void ozRouterPromotionTick() {
  if (millis() - g_lastRouterTryAt < OZ_ROUTER_RETRY_MS) return;
  g_lastRouterTryAt = millis();
  otInstance *inst = esp_openthread_get_instance();
  if (!inst) return;

  esp_openthread_lock_acquire(portMAX_DELAY);
  const otDeviceRole role = otThreadGetDeviceRole(inst);
  otError err = OT_ERROR_NONE;
  bool asked = false;
  bool escalated = false;
  if (role == OT_DEVICE_ROLE_CHILD) {
    err = otThreadBecomeRouter(inst);
    asked = true;
    g_routerTries++;

    // ── ESCALATION — take the partition if asking politely does not work ──
    //
    // BecomeRouter() returns OT_ERROR_NONE and then quietly does nothing when
    // the Leader declines to allocate a Router ID. Observed here: requested
    // every 20 s, accepted every time, still a Child. So after a grace period
    // we stop asking and take over.
    //
    // This works now ONLY because we raised our leader weight above — the API
    // returns OT_ERROR_NOT_CAPABLE if our weight is <= the incumbent's, which
    // is precisely why it would have failed before.
    //
    // HONEST NOTE ON THE SPEC: OpenThread documents leader takeover as
    // "only allowed when triggered by an explicit user action", or the
    // application is non-compliant. We are doing it automatically. The
    // justification is that this device IS the border router — being Leader is
    // its job, not an opportunistic grab — and we are not pursuing Thread
    // certification (Matter is out on principle, ozkey-16). If certification
    // is ever wanted, THIS is the line that has to be revisited.
    if (g_routerTries >= OZ_ROUTER_TRIES_BEFORE_TAKEOVER) {
      err = otThreadBecomeLeader(inst);
      escalated = true;
      g_routerTries = 0; // re-arm; a failed takeover should retry, not spin
    }
  } else {
    g_routerTries = 0; // we are Router/Leader — nothing to escalate
  }
  esp_openthread_lock_release();

  if (escalated)
    Serial.printf("[THREAD] asked %d times and stayed a Child — TOOK OVER as "
                  "Leader (err=%d; NOT_CAPABLE=our weight too low)\n",
                  OZ_ROUTER_TRIES_BEFORE_TAKEOVER, (int)err);

  if (!asked) return;

  // DIAGNOSTIC: BecomeRouter() returning OT_ERROR_NONE only means the request
  // was accepted. Promotion still needs the LEADER to grant a Router ID, and
  // the active Security Policy to permit routers at all (thread_ftd.h:159).
  // If we keep asking and stay a Child, one of those is refusing — print both
  // so we stop guessing which.
  otRouterInfo parent;
  otOperationalDataset ds;
  esp_openthread_lock_acquire(portMAX_DELAY);
  const bool haveParent = (otThreadGetParentInfo(inst, &parent) == OT_ERROR_NONE);
  const bool haveDs = (otDatasetGetActive(inst, &ds) == OT_ERROR_NONE);
  const uint8_t leaderId = otThreadGetLeaderRouterId(inst);
  esp_openthread_lock_release();

  Serial.printf("[THREAD] still CHILD — promotion requested (err=%d)\n", (int)err);
  if (haveParent)
    Serial.printf("[THREAD]   parent rloc16=0x%04x routerid=%u lqi_in=%u — "
                  "THIS is who we hang off\n",
                  parent.mRloc16, (unsigned)parent.mRouterId,
                  (unsigned)parent.mLinkQualityIn);
  Serial.printf("[THREAD]   leader routerid=%u\n", (unsigned)leaderId);
  if (haveDs && ds.mComponents.mIsSecurityPolicyPresent)
    Serial.printf("[THREAD]   secpolicy routers_enabled=%s rotation=%uh\n",
                  ds.mSecurityPolicy.mRoutersEnabled ? "YES"
                                                     : "NO <-- THIS BLOCKS PROMOTION",
                  (unsigned)ds.mSecurityPolicy.mRotationTime);
  else
    Serial.println("[THREAD]   secpolicy NOT PRESENT in active dataset");
}

// Returns true if a datagram was read (caller should try again), false when
// the socket is empty or the read failed.
// XF-116 §7.6 — recent-datagram memory for the de-dup below.
//
// 12 slots: a site can have several locks talking at once and each sends every
// datagram twice, so the window must hold both copies of each in flight without
// a busy neighbour evicting them. 15 s matches the window 1.41 used for reset
// outcomes — long enough to cover the gap between the unicast and multicast
// copies (milliseconds in practice), short enough that a lock legitimately
// repeating itself later is never swallowed.
#define OZ_BR_DUP_SLOTS 12
#define OZ_BR_DUP_WINDOW_MS 15000UL


static bool pollUplinkOne() {
  char buf[OZ_UPLINK_RX_BUF];
  struct sockaddr_in6 src;
  socklen_t srcLen = sizeof(src);
  const int n = lwip_recvfrom(ozUplinkRxFd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &srcLen);
  if (n <= 0) return false; // EWOULDBLOCK on an idle socket — the normal case
  buf[n] = 0;

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    Serial.println("[UPLINK] rx payload not valid JSON, dropped");
    return true; // consumed one; keep draining
  }
  const char *from = doc["from"] | (const char *)nullptr;
  const char *envHex = doc["envelope_hex"] | (const char *)nullptr;
  if (!from) {
    Serial.println("[UPLINK] rx missing `from`, dropped");
    return true; // consumed one; keep draining
  }

  // ozkey-20 R2 — learn which Thread node this device_id lives on.
  //
  // THIS IS THE JOIN, and it is the whole difficulty of R2. The child table
  // gives us liveness (`mAge`) keyed by Thread identity — extended address and
  // RLOC16. The application layer knows `device_id` ("ozk-…"). Nothing in
  // OpenThread connects the two: the extended address is randomly generated,
  // NOT derived from the MAC the device_id is built from (verified on this
  // bench — DoorA's link-local is fe80::d879:a06:ac36:7e6f, unrelated to
  // ac:eb:e6:39:f8:c4).
  //
  // So we learn it from traffic: a datagram arriving from a source address
  // tells us that address belongs to this device_id. Later we walk the child
  // table and match each child's registered IPv6 addresses against this map.
  {
    const char *extHex = doc["ext"] | (const char *)nullptr;
    uint8_t ext[8];
    if (extHex && strlen(extHex) == 16) {
      for (int k = 0; k < 8; k++) {
        char b[3] = {extHex[k * 2], extHex[k * 2 + 1], 0};
        ext[k] = (uint8_t)strtol(b, nullptr, 16);
      }
      ozNoteLockExt(from, ext);
    }
  }

  // D2 — and where to send its downlinks. Must run AFTER ozNoteLockExt, which
  // is what creates the map entry this writes into.
  ozNoteLockIp6(from, &src);

  // ── XF-116 §7.6 — ONE de-dup for EVERY relayed datagram ──────────────────
  //
  // The lock sends every datagram TWICE, on purpose: unicast (to get the
  // MAC-layer ACK) and ff03::1 (which needs no address). Three sites do it —
  // ozdoorlock_core.h:3504/3506 reset, :3604/3607 uplink, :7558/7560 beacon.
  // Both copies arrive here.
  //
  // 1.41 de-duplicated ONLY reset outcomes, because a reset outcome was what
  // crashed BANOI (XF-115). That was a fix scoped to the symptom rather than
  // to the duplication, and it left the other two paths doubled: measured
  // 2026-08-19, every sealed uplink reached the app twice and every Thread
  // heartbeat became two MQTT publishes — doubled broker traffic and doubled
  // server work, fleet-wide, permanently.
  //
  // Keyed on a hash of the whole datagram rather than on msg_id: heartbeats
  // carry no msg_id and no counter, so nothing else is common to all three
  // kinds. The two copies are the same String sent twice, so they are
  // byte-identical and hash equal — and anything genuinely new differs
  // somewhere (sealed traffic carries a monotonic counter, beacons carry
  // uptime/roster), so a real message is never mistaken for a repeat.
  //
  // Placed AFTER the ext/IP learning above: that is idempotent and worth
  // doing from whichever copy lands first. What we suppress is the OUTBOUND
  // effect, not the knowledge.
  {
    uint32_t h = 2166136261u; // FNV-1a
    for (int k = 0; k < n; k++) { h ^= (uint8_t)buf[k]; h *= 16777619u; }

    static String dupFrom[OZ_BR_DUP_SLOTS];
    static uint32_t dupHash[OZ_BR_DUP_SLOTS];
    static unsigned long dupAt[OZ_BR_DUP_SLOTS];
    static uint8_t dupNext = 0;

    for (uint8_t i = 0; i < OZ_BR_DUP_SLOTS; i++) {
      if (dupAt[i] && millis() - dupAt[i] < OZ_BR_DUP_WINDOW_MS &&
          dupHash[i] == h && dupFrom[i] == from) {
        // Name the kind: a suppression we cannot attribute is a suppression
        // we cannot audit, and silently dropping lock traffic is exactly the
        // class of bug this file keeps finding.
        g_dupSuppressed++;
        Serial.printf("[UPLINK] %s: duplicate %s suppressed (hash=%08x, total=%lu)\n",
                      from, (const char *)(doc["kind"] | "datagram"),
                      (unsigned)h, (unsigned long)g_dupSuppressed);
        return true; // consumed one; keep draining
      }
    }
    dupFrom[dupNext] = from;
    dupHash[dupNext] = h;
    dupAt[dupNext] = millis() ? millis() : 1; // 0 means "slot unused"
    dupNext = (uint8_t)((dupNext + 1) % OZ_BR_DUP_SLOTS);
  }

  // ── ozkey-20 R3 — presence beacon, not a sealed uplink ──────────────────
  //
  // A beacon has no `envelope_hex`: it is unsealed liveness (device_id, Thread
  // identity, firmware, roster epoch, MCU link) with nothing private in it.
  // It exists because a Thread lock cannot reach MQTT itself, so without this
  // the server can never mark it reachable and the app falls back to BLE
  // forever after a bridge restart.
  //
  // Handled BEFORE the sealed path so it is never mistaken for a lost uplink.
  if (!envHex) {
    const char *kind = doc["kind"] | "";

    // ── XF-114 — RESET OUTCOME RELAY (2026-08-19) ─────────────────────────
    //
    // A Thread lock has no MQTT session, so when it factory-resets it cannot
    // tell anyone — the app sat waiting for an acknowledgement that was never
    // transmitted on any wire, which is the whole of XF-114. doorlock-1.95
    // fixed that for Wi-Fi locks by publishing to their own presence topic;
    // this is the Thread half, and the bridge is the only thing that can carry
    // it.
    //
    // 🔴 THIS IS THE LOCK'S LAST BREATH. It is sent immediately before a
    // platform reset that never returns, so there is no retry, no second
    // chance, and no way for the lock to learn whether it arrived. Publish it
    // first and reason about it afterwards.
    //
    // Republished to `locks/<id>/presence` — NOT `heartbeat` — so it lands on
    // the topic the server already subscribes to for presence and can resolve
    // a pending DELETE against.
    if (strcmp(kind, "reset_outcome") == 0) {
      if (!mqttClient.connected()) {
        // The one message we most need not to lose, and the broker is down.
        // Say so loudly: this is the lock's only chance and it is gone.
        Serial.printf("[RESET] %s: broker down, RESET OUTCOME LOST\n", from);
        return true;
      }
      const char *reason = doc["reason"] | "";
      const String msgId = String((const char *)(doc["msg_id"] | ""));
      const bool online = (strcmp((const char *)(doc["state"] | "offline"), "online") == 0);

      // ── 🔴 DE-DUPLICATE — XF-115 §6.1, and this one caused a P1 ──────────
      //
      // The lock DUAL-SENDS its outcome: unicast (for the MAC ACK) and
      // ff03::1 (which needs no peer address). That redundancy is deliberate
      // and stays — it is the lock's LAST BREATH before a platform reset, it
      // cannot be retried, and halving the attempts to avoid a duplicate would
      // trade a lost reset for a tidy log.
      //
      // So the duplication is suppressed HERE, where it costs nothing: both
      // datagrams still arrive, exactly one MQTT message leaves. Consumers see
      // one outcome per reset; the radio keeps both chances.
      //
      // WHY IT MATTERS (measured, not theoretical): the second copy reached
      // BANOI as a second `acked ok` a few hundred ms after the first, and its
      // removal screen popped itself twice — `You have popped the last page off
      // of the stack`. Firmware had classified this duplicate as "harmless".
      // 1.43 — the msg_id/reason-keyed guard that used to sit here is GONE,
      // replaced by the datagram-hash de-dup at the top of this function
      // (XF-116 §7.6), which covers reset outcomes, sealed uplinks and beacons
      // alike. Deliberately not kept as belt-and-braces: it could never fire
      // again, and a guard that cannot fire is indistinguishable from a guard
      // that is broken — this file has been bitten by exactly that before.
      // The duplicate it caught is the same two-copy dual send, so the general
      // guard sees the identical bytes and stops it strictly earlier.

      // ── REBUILT, not forwarded ───────────────────────────────────────────
      //
      // 1.40 forwarded the lock's datagram VERBATIM, so its internal field
      // names (`from`, `kind`) became the published schema on the Thread path
      // while Wi-Fi locks published {state,id,role,reason,msg_id}. Two shapes,
      // one topic, one of them retained and therefore redelivered forever.
      //
      // The shared builder now decides what leaves, for BOTH transports.
      const String out = ozBuildLockPresence(String(from), online, reason, msgId);
      const String topic = "ozkie/" + cfgSiteId + "/locks/" + String(from) + "/presence";
      const bool retain = ozPresenceShouldRetain(online);
      const bool ok = mqttClient.publish(topic.c_str(), out.c_str(), retain);
      // This lock has gone away; forget that we ever marked it online, so that
      // when it comes back its first beacon re-publishes `online` and clears
      // the retained value below.
      if (!online) ozForgetLockOnline(from);
      Serial.printf("[RESET] %s -> %s reason='%s' msg_id='%s'%s%s\n", from,
                    topic.c_str(), reason, msgId.c_str(),
                    retain ? " (retained)" : " (not retained)",
                    ok ? "" : " PUBLISH FAILED");
      return true;
    }

    if (strcmp(kind, "presence") != 0) {
      Serial.printf("[UPLINK] %s: no envelope and kind='%s' — dropped\n", from, kind);
      return true;
    }
    // ozkey-21 — the lock is telling us it has no clock. Answer immediately
    // rather than making it wait for the daily beacon. Cheap and idempotent:
    // a lock that already has time simply stops setting the flag.
    if (doc["need_time"] | false) {
      Serial.printf("[TIME] %s has no clock — beaconing on request\n", from);
      sendTimeBeacon();
    }

    if (mqttClient.connected()) {
      const String topic = "ozkie/" + cfgSiteId + "/locks/" + String(from) + "/heartbeat";
      const bool ok = mqttClient.publish(topic.c_str(), buf);
      Serial.printf("[BEACON] %s -> %s%s\n", from, topic.c_str(),
                    ok ? "" : " PUBLISH FAILED");
      // Q3 — this lock is demonstrably alive, so assert it on the presence
      // topic and clear whatever retained value is sitting there (typically a
      // `factory_reset` from before it was re-paired). Once per online
      // transition, not once per beacon — see ozClaimLockOnline().
      if (ozClaimLockOnline(from)) {
        const String pres = ozBuildLockPresence(String(from), true,
                                                OZ_PRESENCE_ONLINE, String(""),
                                                (const char *)(doc["fw"] | ""));
        const String ptopic = "ozkie/" + cfgSiteId + "/locks/" + String(from) + "/presence";
        mqttClient.publish(ptopic.c_str(), pres.c_str(), true /*retain*/);
        Serial.printf("[PRESENCE] %s -> online (retained, clears any stale reset)\n", from);
      }
    } else {
      Serial.printf("[BEACON] %s: broker down, presence dropped\n", from);
    }
    return true;
  }

  if (!mqttClient.connected()) {
    // Honest failure. The lock has already burned a send counter on this
    // message, so it is gone — say so rather than let it vanish silently, which
    // is precisely the class of invisible loss this whole feature exists to end.
    Serial.printf("[UPLINK] %s: broker down, uplink DROPPED (%d B)\n", from, n);
    return true; // consumed one; keep draining
  }

  // U3: the lock's own topic, not ours. See the header note — this is what
  // keeps ozlockserv a mailman and spares it any change at all.
  const String topic = "ozkie/" + cfgSiteId + "/locks/" + String(from) + "/uplink"; // S16
  const bool ok = mqttClient.publish(topic.c_str(), buf);
  Serial.printf("[UPLINK] %s -> %s (%d B)%s\n", from, topic.c_str(), n,
                ok ? "" : " PUBLISH FAILED");
  return true;
}

// One multicast send, logged per destination group so the bench can tell which
// group a datagram actually went out on. `fieldName` is "payload" (legacy,
// pure-forward) or "envelope_hex" (sealed, ozkey-13 §8 BR1) — the bridge
// never inspects `valueHex` either way, just relays it under whichever name
// arrived. Kept generic rather than two near-duplicate functions.
// ─────────────────────────────────────────────────────────────────────────────
// ozkey-21 T3 — the bridge is the mesh's time source
//
// Neither the doorlock nor this board has an RTC, and a Thread lock has no
// Wi-Fi, so it can never reach NTP itself. The bridge can: it is mains-powered
// and already on Wi-Fi. Without this, every Thread lock answers its own MCU's
// 0x1C with "I do not know", and DP 21/23 temporary PIN and RFID windows stay
// unenforceable — see ozkey-21 §2.3, confirmed on DoorA 2026-08-10.
//
// TRUST MODEL — deliberate, and weaker than ozkey-21 §3.4 rule 4 asks for.
// Rule 4 wants time to ride the sealed-envelope path. This rides plain mesh
// traffic instead, authenticated only by the Thread network key. The reasoning:
// only a commissioned device can put a datagram on this mesh, and the lock's
// monotonic-forward rule means a hostile time can only push a clock FORWARD.
// Forward-only prematurely EXPIRES credentials — a denial of access — and can
// never resurrect an expired one, which is the attack rule 1 exists to stop.
// So the residual risk is DoS by an already-commissioned device, not access
// extension. Sealing the beacon is the follow-up; it is not what gates T5.
// ─────────────────────────────────────────────────────────────────────────────
static unsigned long lastTimeBeaconAt = 0;

// ─────────────────────────────────────────────────────────────────────────────
// 🔴 1.36 — NTP REMOVED. ONE time source, not two.
//
// The bridge used to run TWO clock writers side by side: the SNTP daemon
// started by configTime(), and the server's `utc` over MQTT. They never
// arbitrated, so NTP could silently step over a value the server had set.
//
// The first fix attempted here was to arbitrate between them — a source enum, a
// priority rule, esp_sntp_stop(). The system architect's call, and he is right:
// that adds machinery to manage a second source we do not need. NTP was never
// dependable here anyway — UDP 123 is blocked on this network (measured
// 2026-08-11) and on any hotel or office that filters outbound, which is the
// exact deployment this product targets. It contributed nothing but a second
// writer and the need to referee it.
//
// So the server is the only clock, which it already was in practice:
//   • ozlockserv pushes `utc` on the bridge's command topic (server.js:1150)
//   • the bridge stamps it onto every forwarded command, and beacons it daily
//   • the locks apply their own monotonic guard to whatever arrives
// If the bridge cannot reach our server there is no product anyway, so a
// fallback clock buys nothing and costs a class of bug.
//
// Deleted with it: ozNtpBegin(), ntpStarted, the OzTimeSource enum, the
// arbitration function, the SNTP-stop, and the panel's source tag — none of
// which had a purpose once there was one source.
// ─────────────────────────────────────────────────────────────────────────────

// DAILY, per ozkey-21 §3.3 and the operator's call 2026-08-11. Airtime is the
// cost that matters on a shared mesh (ozkey-20 §4.1), and locks also get a free
// UTC stamp on every forwarded command, so the beacon only carries the locks
// nobody talks to. See the drift caveat in ozdoorlock_core.h's MCU_TIME_PUSH_MS.
#define OZ_TIME_BEACON_MS 86400000UL

/* Our current UTC, or 0 if nothing has set it yet. 0 means "do not stamp". */
static uint32_t ozBridgeUtc() {
  time_t now = time(nullptr);
  return ((uint32_t)now >= OZ_TIME_FLOOR) ? (uint32_t)now : 0;
}

// ── 1.38 — THE BRIDGE KEEPS ITS CLOCK ACROSS A REBOOT ───────────────────────
//
// Until now the bridge stored nothing: ozBridgeUtc() returned 0 until the
// server pushed `utc`, and 0 means "do not stamp" and "do not beacon". So a
// bridge that rebooted while ozlockserv was unreachable left EVERY THREAD LOCK
// BEHIND IT WITH NO TIME SOURCE AT ALL — the locks' only supply is this
// bridge's beacon.
//
// The standing rationale was "if the bridge cannot reach our server there is no
// product anyway". That is true of remote unlock and grants. It is NOT true of
// the case that matters here: temporary PINs already stored on a lock keep
// working through a server outage — that is the point of storing them — and
// local expiry enforcement is exactly what needs a clock. A server outage is
// therefore the moment a lock is MOST dependent on its own clock, and it was
// also the one moment we guaranteed it could not have one.
//
// Same shape as the lock's proven pair (ozdoorlock_core.h ozClockPersist/
// ozClockRestore): write at most hourly to spare flash endurance, restore
// through the same floor check on boot.
#define OZ_BRIDGE_CLOCK_PERSIST_MS 3600000UL

// 🔴 PROVENANCE, and it is NOT optional decoration.
//
// doorlock-1.74 distinguishes a clock a real source gave it (`clock=live`) from
// one restored out of NVS (`clock=NVS-only`), and keeps ASKING while it only
// has the latter. That distinction is the entire fix for the battery-change
// case. If this bridge restored a stale clock and then beaconed it as though it
// were a fresh server sync, every lock downstream would mark itself `live` on
// the strength of our guess and stop asking — silently undoing that work and
// making the fleet's clocks look trustworthy precisely when they are not.
//
// So the beacon says where its time came from, and a restored value is labelled
// as such. Locks apply it (better than nothing) but keep asking for a real one.
static void ozBridgeClockPersist(bool force) {
  static unsigned long lastWrite = 0;
  const uint32_t now = ozBridgeUtc();
  if (!now) return;
  if (!force && lastWrite && millis() - lastWrite < OZ_BRIDGE_CLOCK_PERSIST_MS)
    return;
  lastWrite = millis();
  prefs.begin("bridge32", false);
  prefs.putUInt("utclast", now);
  prefs.end();
}

static void ozBridgeClockRestore() {
  prefs.begin("bridge32", true);
  const uint32_t saved = prefs.getUInt("utclast", 0);
  prefs.end();
  if (saved < OZ_TIME_FLOOR) return;
  struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[TIME] clock restored from NVS: %lu — a GUESS (stale by up to "
                "1 h plus however long we were off). Locks are told utc_src=nvs "
                "so they keep asking for a real one.\n",
                (unsigned long)saved);
}


// ── XF-115 / Q3 — the bridge owns presence for its Thread children ──────────
//
// A Wi-Fi lock clears its own retained presence when it reconnects to MQTT.
// A THREAD LOCK NEVER CAN: clearing happens on MQTT connect and it has no MQTT
// session, ever. So its retained {"state":"offline","reason":"factory_reset"}
// would survive the lock being wiped, re-paired and brought back into service —
// permanently describing a working lock as factory-reset. That is the same
// standing-false-statement bug 1.96 fixed for Wi-Fi locks, and the Thread half
// was left open.
//
// The bridge is the only thing that can speak for these locks, so it publishes
// `online` on their behalf. Driven off the presence BEACON rather than a
// commissioning event: a beacon means the lock is demonstrably alive and
// talking RIGHT NOW, which is the property we want to assert, and it
// self-heals — a lock that was reset, re-paired, power-cycled or simply missed
// by an event still gets corrected on its next beacon.
//
// Published ONCE per lock per online-transition, not per beacon: the table
// (declared above publishThreadLiveness(), which also reads it — XF-116)
// remembers who we have already marked online, so a 60 s beacon does not
// become a 60 s retained write per lock across a whole site.

static void ozForgetLockOnline(const char *id) {
  for (uint8_t i = 0; i < OZ_BR_MAX_CHILDREN; i++)
    if (g_onlineLocks[i] == id) { g_onlineLocks[i] = ""; return; }
}

/** True if `id` was not already marked online (and claims a slot for it). */
static bool ozClaimLockOnline(const char *id) {
  int8_t free_i = -1;
  for (uint8_t i = 0; i < OZ_BR_MAX_CHILDREN; i++) {
    if (g_onlineLocks[i] == id) return false; // already announced
    if (free_i < 0 && g_onlineLocks[i].length() == 0) free_i = (int8_t)i;
  }
  // Table full: announce anyway rather than stay silent. A redundant retained
  // write is cheap; a lock stuck reading "factory_reset" forever is not. The
  // cap only bounds how much we remember, never what we are willing to say.
  if (free_i < 0) return true;
  g_onlineLocks[free_i] = id;
  return true;
}

static bool sendToThreadGroup(const IPAddress &group, const String &target,
                              const String &fieldName, const String &valueHex,
                              const char *label) {
  // "via" tags which multicast group carried this datagram. The doorlock reads
  // only "target"/"payload"/"envelope_hex" and ArduinoJson ignores unknown
  // keys, so this is inert to the relay — but the doorlock's rx diagnostic
  // dumps the whole buffer, making it obvious which group actually got through.
  JsonDocument doc;
  doc["target"] = target;
  doc[fieldName] = valueHex;
  doc["via"] = label;
  // Q4 — the lock reads this and echoes it in its reset outcome. Absent for
  // bridge-originated datagrams, which answer no request.
  if (g_fwdMsgId.length()) doc["msg_id"] = g_fwdMsgId;
  // ozkey-21 T3 — stamp UTC on every forwarded command. This is the cheap half
  // of time distribution: the datagram is already being sent, so a lock that
  // gets any traffic at all stays in sync for free. The beacon below exists
  // only for locks that receive no commands for long stretches.
  if (ozBridgeUtc()) {
    doc["utc"] = ozBridgeUtc();
    doc["tz"] = cfgTzMin;
  }
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

/*
 * ozkey-21 T3 — the standalone time beacon.
 *
 * For locks that receive no commands at all: a lock nobody unlocks remotely
 * still has to expire its temporary PINs on schedule.
 *
 * Addressed to ff03::1 (realm-local all-nodes), NOT our own ff03::4f5a group —
 * that custom group has never delivered a single packet in 18 attempts and
 * "mcast joined" in a boot log proves nothing about delivery. `target` is "*"
 * because this is genuinely for every lock; the lock reads `utc` before its
 * target check for exactly that reason.
 */
static bool sendTimeBeacon() {
  const uint32_t utc = ozBridgeUtc();
  if (!utc || !threadUdpReady) return false;

  JsonDocument doc;
  doc["target"] = "*";
  doc["kind"] = "time";
  doc["utc"] = utc;
  // 1.38: "server" = ozlockserv pushed this to us; "nvs" = we restored it and
  // have not heard from the server since booting. doorlock-1.74 treats the
  // latter as NOT a confirmation and keeps its need_time flag raised.
  doc["utc_src"] = g_utcFromServer ? "server" : "nvs";
  // Locks have no other way to learn the timezone — they are never paired with
  // a phone directly on Thread, the bridge is. Carried on every beacon rather
  // than once, so a lock that joins later still gets it without a special case.
  doc["tz"] = cfgTzMin;
  String out;
  serializeJson(doc, out);

  IPAddress all;
  if (!all.fromString("ff03::1")) return false;
  if (!threadUdp.beginPacket(all, OZ_THREAD_UDP_PORT)) return false;
  threadUdp.write((const uint8_t *)out.c_str(), out.length());
  if (!threadUdp.endPacket()) {
    Serial.println("[TIME] beacon endPacket failed");
    return false;
  }
  lastTimeBeaconAt = millis();
  Serial.printf("[TIME] beacon -> ff03::1 utc=%lu\n", (unsigned long)utc);
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

void forwardOverThread(const String &target, const String &fieldName,
                       const String &valueHex) {
  if (!target.length() || !valueHex.length()) {
    Serial.println("[UDP] drop — command missing target/payload");
    return;
  }
  if (!threadUdpReady) {
    Serial.println("[UDP] drop — socket not open");
    return;
  }
  logThreadChildren(); // DIAGNOSTIC (temporary) — is the lock even on this mesh?

  // ── D2 (ozkey-35 §3.2) — UNICAST FIRST, and why it is not optional ────────
  //
  // A Sleepy End Device polls its parent for UNICAST only. Realm-local
  // multicast is a link-layer broadcast and a sleeping radio is simply not
  // present for it, so every multicast downlink is lost the moment a lock
  // stops being rx-on-when-idle. That is the entire reason SED ships disabled
  // (doorlock's cfgThreadSed defaults false) and this is the prerequisite that
  // unblocks it — ~3 days of battery on a full Thread device vs years on a
  // sleepy one.
  //
  // Unicast is the better path even for an rx-on lock: it is the only
  // destination that gets link-layer ACKs and MAC retries. Multicast is
  // retained ONLY as the fallback for a lock we have not heard from yet, since
  // the address is learned from the lock's own uplinks and a bridge that has
  // just rebooted knows nobody until the next beacon (~60 s).
  //
  // This replaces the 2026-07-28 diagnostic that hard-coded one bench lock's
  // mesh-local EID. That experiment answered its question — unicast arrives,
  // multicast does not — and the answer is now the design.
  IPAddress dest;
  if (ozLockIp6(target.c_str(), dest)) {
    sendToThreadGroup(dest, target, fieldName, valueHex, "unicast");
    return; // delivered to a known address; do not also spray the mesh
  }

  Serial.printf("[UNICAST] %s not in the address map yet — falling back to "
                "multicast (a SLEEPY lock will NOT receive this)\n",
                target.c_str());
  sendToThreadGroup(OZ_THREAD_GROUP, target, fieldName, valueHex, "ff03::4f5a");
  sendToThreadGroup(OZ_REALM_ALLNODES, target, fieldName, valueHex, "ff03::1");
}

void mqttConnect() {
  if (mqttClient.connected()) return;
  Serial.printf("[MQTT] connecting to %s:%u as %s\n", cfgBrokerHost.c_str(), cfgBrokerPort,
                deviceId.c_str());
  notifyStatus("BROKER_JOINING");

  // ── ozkey-20 R1 — MQTT Last Will ────────────────────────────────────────
  //
  // The broker publishes this the instant our session drops, however it drops
  // — power cut, Wi-Fi loss, crash, cable pulled. No polling, no heartbeat, no
  // timeout to tune, and it costs nothing while we are alive.
  //
  // RETAINED on purpose: a subscriber that connects later still learns the
  // current state immediately instead of waiting for the next transition. That
  // is what makes the server's `bridge_offline` verdict work after a server
  // restart rather than sitting at `unknown`.
  //
  // This matters more for the bridge than for any lock: every Thread lock is
  // invisible without it, so one bridge dying is N locks going dark, and
  // ozkey-20 R6 must report that as ONE `bridge_offline`, not N unreachable
  // locks. That aggregation needs this signal to exist.
  //
  // Note the will QoS is the BROKER's to honour when it publishes on our
  // behalf — unaffected by PubSubClient's own publish() being QoS 0 only
  // (ozkey-19 v2 §2.1).
  const String willTopic = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/presence";
  static const char *kWillOffline = "{\"state\":\"offline\",\"reason\":\"lwt\"}";

  if (mqttClient.connect(deviceId.c_str(), nullptr, nullptr, willTopic.c_str(),
                         1 /*willQos*/, true /*willRetain*/, kWillOffline)) {
    Serial.println("[MQTT] connected");
    notifyStatus("BROKER_OK");

    // Clear our own will immediately. Until this lands the retained value at
    // that topic is still "offline" from last time, so a server reading it in
    // the gap would call a live bridge dead.
    const String online = String("{\"state\":\"online\",\"id\":\"") + deviceId +
                          "\",\"role\":\"bridge\"}";
    mqttClient.publish(willTopic.c_str(), online.c_str(), true /*retain*/);
    Serial.printf("[MQTT] presence ONLINE -> %s\n", willTopic.c_str());
    mqttClient.subscribe(mqttCommandTopic.c_str());
    if (mqttCommandTopicLegacy.length())
      mqttClient.subscribe(mqttCommandTopicLegacy.c_str()); // S16 transition
    Serial.printf("[MQTT] subscribed %s\n", mqttCommandTopic.c_str());
  } else {
    Serial.printf("[MQTT] connect failed, rc=%d\n", mqttClient.state());
    notifyStatus("BROKER_FAIL");
  }
  mqttLastAttempt = millis();
}

void mqttBegin() {
  // S16: publish/primary on `ozkie/`; also subscribe the legacy `ozkey/`
  // root during migration so update order between firmware, server and app
  // does not matter and nothing goes dark by being flashed second.
  mqttCommandTopic = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/command";
  mqttCommandTopicLegacy = "ozkey/" + cfgSiteId + "/bridges/" + deviceId + "/command";
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

  // ── ozkey-20 §15.3 TOPOLOGY FIX — the bridge must be a ROUTER ────────────
  //
  // Found 2026-08-11: this bridge was attached as a **Child**, and so was
  // DoorA — both parented to some third node. Consequences, in order of how
  // much they cost:
  //
  //  1. A Child has NO child table, so R2's liveness report is structurally
  //     blind: 0 children while every lock is alive and working.
  //  2. Lock traffic routes via that third node instead of one hop to us.
  //     This is very likely the same fact recorded during ozkey-19 — "DoorA
  //     and the bridge are both attached as Child to the same parent, so they
  //     are NOT link-layer neighbours" — which was observed and never chased.
  //  3. The border router depending on a battery-powered lock to stay
  //     reachable is backwards. This file already said so in logThreadChildren():
  //     "a battery lock should not parent it."
  //
  // Router eligibility is the FTD default, but eligibility only means "may
  // promote" — a REED sits as a Child until the network decides it needs
  // another router, on its own schedule. For the mains-powered border router
  // that is the wrong default: we always want to be a Router, immediately,
  // so locks can parent to us and so we own a child table.
  //
  // Both calls, deliberately: SetRouterEligible(true) is the standing
  // permission (harmless if already set, and explicit beats inherited),
  // BecomeRouter() is the request to do it NOW rather than at the network's
  // convenience.
  //
  // BecomeRouter() returns OT_ERROR_INVALID_STATE while still detached — that
  // is expected, not a failure, so it is retried from loop() until the role
  // actually changes (see ozRouterPromotionTick()).
  {
    otInstance *inst = esp_openthread_get_instance();
    if (inst) {
      esp_openthread_lock_acquire(portMAX_DELAY);
      const otError e1 = otThreadSetRouterEligible(inst, true);

      // ── LEADER WEIGHT — why the bridge kept losing ──────────────────────
      //
      // Every node ships with leader weight 64, so the Leader is simply
      // whoever attached first. On this bench that was a DOORLOCK, twice:
      // LockB held it, then DoorA took it after a reset. A battery lock
      // leading the partition while the mains-powered border router hangs off
      // it as a Child is backwards, and it is why R2 saw zero children.
      //
      // Raising ours means the bridge WINS elections and partition merges
      // instead of losing them by accident of boot order. 128 rather than 255
      // to leave headroom above us for a future OZLODGE appliance that should
      // outrank a bridge.
      otThreadSetLocalLeaderWeight(inst, 128);
      // Promote fast. The default selection jitter (120 s) is tuned for
      // battery meshes deciding democratically who should route; we are the
      // border router and there is nothing to decide.
      otThreadSetRouterSelectionJitter(inst, 1);
      esp_openthread_lock_release();
      Serial.printf("[THREAD] router-eligible set (err=%d), selection jitter=1s\n",
                    (int)e1);
    }
  }

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

  // ── ozkey-20 R2 — event-driven half ─────────────────────────────────────
  // Push the moment a child attaches or is evicted, so those two transitions
  // carry zero latency instead of waiting up to 30 s for the sweep. The sweep
  // still runs (see loop()) — these events cannot report a child that is
  // merely going stale, only one that has already gone.
  {
    otInstance *inst = esp_openthread_get_instance();
    if (inst) {
      esp_openthread_lock_acquire(portMAX_DELAY);
      otSetStateChangedCallback(inst, ozThreadStateChanged, nullptr);
      esp_openthread_lock_release();
      Serial.println("[LIVENESS] child add/remove callback registered");
      ozLoadLockMap(); // survive our own reboots (see ozSaveLockMap)
    }
  }

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
                       uint16_t &portOut, String &siteOut, int16_t &tzOut) {
  modeOut = (const char *)(doc["mode"] | "");
  if (modeOut != "mqtt-uplink" && modeOut != "matter-bridge") return false;

  hostOut = (const char *)(doc["broker_host"] | "");
  portOut = (uint16_t)(doc["broker_tcp_port"] | 0);
  siteOut = (const char *)(doc["site_id"] | "");
  // Optional and additive — a bridge provisioned by an older app simply stays
  // at UTC, which is exactly what it did before this field existed.
  tzOut = (int16_t)(doc["tz"] | doc["tz_offset_min"] | 0);
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
  int16_t tzMin = 0;
  if (!validModePayload(doc, mode, brokerHost, brokerPort, siteId, tzMin)) {
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
  cfgTzMin = tzMin;
  wifiProvisioned = true;
  saveConfig();
  Serial.printf("[TIME] timezone from app: %+d min (%+.2f h)\n",
                (int)cfgTzMin, cfgTzMin / 60.0);

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

  // ⚠ BROADCAST-ID SCAN RESPONSE REVERTED 2026-08-12 — IT BROKE DISCOVERY.
  //
  // bridge32-1.31 set the scan response EXPLICITLY to carry a 4-byte broadcast
  // id (XF-94), mirroring doorlock-1.55. On hardware the app then could not
  // find the bridge at all: "Xoá Bridge" had worked minutes earlier on 1.30 and
  // after 1.31/1.32 produced NO BLE connection whatsoever — the bridge's own
  // log showed nothing but the operator's BOOT press.
  //
  // The byte arithmetic said this was safe (ADV full at 31/31 with the name, so
  // the name lives there and the scan response was free). The arithmetic was
  // wrong, or the ESP32 BLE stack does not lay it out the way the arithmetic
  // assumed — calling setScanResponseData() replaces whatever the library was
  // generating implicitly, and discovery depended on it.
  //
  // I flagged this exact risk in the 1.31 commit as "worth an explicit check
  // rather than an assumption", then shipped it anyway on the strength of the
  // calculation. The check is what found it, one flash later.
  //
  // The lock's equivalent (doorlock-1.55) is NOT affected and stays: its ADV is
  // 29/31 with a shorter name and it already set scan response data before this
  // change, for the M3 busy byte. Only the bridge is reverted.
  //
  // To restore this feature: verify with a real BLE scanner that the name and
  // service UUID survive in the ADV before trusting the byte math again.

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

  // 1.38: before anything else that might stamp or beacon. A restored clock is
  // a guess, but a guess beats 0 — 0 means every Thread lock behind this bridge
  // gets no time at all until ozlockserv is reachable again.
  ozBridgeClockRestore();

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

  // 1.38: keep the on-flash copy fresh. Self-throttled to hourly inside, so
  // calling it every iteration costs a millis() compare.
  ozBridgeClockPersist(false);

  // ── ozkey-20 R2 — periodic liveness sweep ──────────────────────────────
  //
  // BOTH this sweep AND the CHILD_ADDED/CHILD_REMOVED events, per the server
  // team's refinement (ozkey-20 §5). Not either/or, and the reason matters:
  //
  //   • The events catch the two HARD transitions with zero latency, but
  //     CHILD_REMOVED only fires after OpenThread's own MLE child timeout —
  //     minutes, not the ~90 s `lock_unreachable` target.
  //   • The sweep is the only thing that sees a child that is STILL ATTACHED
  //     but drifting stale (`mAge` climbing), which is the early warning the
  //     events structurally cannot give: between ADDED and REMOVED there is
  //     no event, because `mAge` is a continuous value, not a transition.
  //
  // Costs one Wi-Fi publish per interval and NO mesh traffic at all.
  // Keep pushing for Router until we get it (ozkey-20 §15.3).
  if (threadFormed) ozRouterPromotionTick();

  // ── ozkey-21 T3 — be the mesh's clock ───────────────────────────────────
  // First beacon fires as soon as NTP answers (lastTimeBeaconAt == 0), then
  // hourly. A lock that just booted should not wait an hour to learn the time.
  if (threadFormed && threadUdpReady &&
      (g_timeBeaconDue || lastTimeBeaconAt == 0 ||
       millis() - lastTimeBeaconAt > OZ_TIME_BEACON_MS)) {
    // Clear the child-attach request ONLY if the beacon actually went out.
    // sendTimeBeacon() aborts when the bridge has no clock of its own yet, and
    // consuming the flag on that path would silently drop the one beacon a
    // freshly-attached lock is waiting for — it would then sit clock-less for
    // 24 h, which is the exact failure this flag exists to prevent.
    if (sendTimeBeacon()) g_timeBeaconDue = false;
  }

  if (threadFormed &&
      (g_livenessPushDue ||
       millis() - g_lastLivenessAt > OZ_LIVENESS_INTERVAL_MS)) {
    g_livenessPushDue = false;
    g_lastLivenessAt = millis();
    publishThreadLiveness();
  }

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

  // ozkey-21 — the clock line has to tick. This screen only ever redrew on
  // events, which would have left a frozen timestamp looking like a hung
  // bridge. Full-screen redraw at 1 Hz is acceptable here because the panel
  // blanks on idle anyway (LCD_IDLE_OFF_MS), so this costs nothing in the
  // state the bridge actually spends its life in.
  // Redraw ONLY the clock row, never the whole screen. A fillScreen at 1 Hz
  // flickers visibly and, on the doorlock, the equivalent mistake cost touch
  // sensitivity outright (see tuyaWireSend()'s screenDirty note). Same lesson,
  // applied here before it could bite.
  static unsigned long lastClockDraw = 0;
  if (lcdOn && !lcdRxFlashActive && millis() - lastClockDraw > 1000) {
    lastClockDraw = millis();
    bool allUp = (WiFi.status() == WL_CONNECTED) && threadFormed;
    if (allUp) {
      // Same renderer as the full redraw. This used to be a SECOND copy that
      // omitted cfgTzMin, so the panel alternated between local time and UTC
      // once a second — 'jumping between 2 timing systems' at the bench.
      drawClockRow(false);
    }
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

  // ozkey-17 U2: same retry discipline as threadUdpBegin() above, and for the
  // same reason — the socket can fail to open in the first moments after Thread
  // forms, so this is re-polled rather than attempted once.
  if (state == ST_OPERATIONAL && !uplinkRxReady &&
      millis() - uplinkRxLastAttempt > THREAD_UDP_RETRY_MS) {
    uplinkRxBegin();
  }
  pollUplinkUdp();

  delay(50);
}
