/*
 * ozdoorlock_core.h — shared OZLOCK COMM MODULE logic, board-independent.
 *
 * Crypto, BLE GATT, bond table, MCU forwarding, self-tests, Wi-Fi/Thread
 * transport, MQTT, dispatch — everything that is byte-identical between the
 * 1.47" (doorlock.ino) and 1.9" (doorlock19.ino) boards. Extracted 2026-08-07
 * so a fix here reaches every board without hand-copying, and per-board
 * hardware differences (LCD/touch driver, pin map, palette, coordinate
 * transform) can never silently diverge the shared logic.
 *
 * A per-board .ino must, BEFORE including this file, define:
 *   Pins:      LCD_DC/CS/SCK/DIN/MISO/RST/BL, I2C_SDA/SCL, TOUCH_ADDR,
 *              TUYA_TX_PIN/TUYA_RX_PIN, SRDY_PIN/MRDY_PIN, USER_BUTTON
 *              (the mechanical factory-reset button — the ONLY reset path on
 *              a screen-less board, so this must be a real, hardware-
 *              verified GPIO, never left to inherit the toolchain's generic
 *              BOOT_PIN default; #error's out if a board omits it)
 *   Palette:   C_BLACK/WHITE/RED/BLUE/GREEN/AMBER/GREY/DIM
 *   Display:   PANEL_W, PANEL_H, LCD_ROTATION, `bus`/`gfx` objects
 *   Backlight: LCD_BL_ON, LCD_BL_OFF (polarity differs per board)
 *   Version:   FW_VERSION, FW_DISPLAY_VERSION
 *   Functions: touchInit(), void mapTouchRaw(int rawX, int rawY, int &x, int &y),
 *              void drawSplash() (no-op is fine if the board doesn't want one)
 *   Optional:  #define HAS_TOUCH_INT if the board wires a real touch INT/RST
 *              pair usable as a light-sleep wake source (only the 1.47" does)
 */


#include <Arduino_GFX_Library.h>
#include <Wire.h>
// XF-115 §7.3 — the ONE definition of a lock's presence payload, shared with
// bridge32 so the Wi-Fi and Thread producers cannot disagree about the shape.
#include "ozpresence.h"
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
#include "oztime.h"   // ozkey-21 T1/T2 — module clock + the MCU time service
#include "ozprofile.h" // ozkey-27 §4.5 — the DP map as DATA, shared with LockSim
#include <OThread.h>     // Thread transport (ported from threadcomm.ino)
#include <OThreadUDP.h>  // F4 UDP relay (bridge32/threadcomm proven, 2026-07-25/26)
#include <openthread/link.h> // otLinkSetPollPeriod — SED poll interval (C9)
// Receive half runs on lwIP, not OpenThread's internal UDP — see the root-cause
// note above threadUdpBegin() (esp_openthread_netif_glue pushes inbound Thread
// packets into lwIP, so otUdp* sockets never see them).
#include <lwip/sockets.h>
#include <errno.h>
#include <fcntl.h>

// ── Forward declarations ────────────────────────────────────────────────────
// Arduino's auto-prototype generator only scans the primary .ino file, not
// #include'd headers — so every function this file's earlier code calls
// before its later definition (which worked "for free" when this was all one
// monolithic .ino) needs an explicit declaration here instead. Generated from
// every top-level function definition in this file — keep in sync if a
// function signature changes.
String asciiOnly(const String &s);
String describeDpid(const uint8_t *f, size_t n);
String isoNow();
bool bleWindowOpen();
bool hexToBytes(const String &hex, uint8_t *out, size_t expectLen);
bool isLocalMode();
bool isThread();
bool mcuLinkUp();
bool srdyAsserted();
void serveMcuTimePush(); // 1.74: ozHarvestTime() calls it long before its body
bool touchRead(int &tx, int &ty);
char keyAt(int tx, int ty, int &r, int &c);
void drawKeypad();
void drawHexReadout();
int hexNibble(char c);
static bool ozControlTry(bool final);
// XF-114 §10.3 — NO_BOND is split out of FAILED_DEFINITE deliberately.
//
// "This sender holds no bond on me" and "this message is malformed or forged"
// are both definite failures, and until 1.95 both surfaced to the app as a
// bare UNLOCK_DENIED. For an UNLOCK that conflation is harmless. For a REMOVE
// it is the difference between "refused" and "the thing you asked for has
// already happened" — an unowned lock IS the end state a delete is trying to
// reach, and the app was reporting it as a failure and keeping the entry
// (bench, 2026-08-18).
//
// 🔴 Note what the lock CANNOT say here: with no bond there is no shared
// secret, so the envelope never opens and we do not know which verb was
// requested. NO_BOND is therefore a statement about the SENDER, not about the
// command — "I do not know you" — and it is the app's job to combine that with
// the request it has outstanding.
enum OzCtlOpen {
  OZCTL_OPENED,
  OZCTL_FAILED_DEFINITE,
  OZCTL_FAILED_NO_BOND,
  OZCTL_FAILED_MAYBE_INCOMPLETE
};
static OzCtlOpen ozControlOpen(const uint8_t *buf, size_t n, int *outSlot,
                                uint8_t *pt, size_t ptCap, size_t *outPtLen,
                                uint64_t *outCounter);
static void ozControlVerifyAndDispatch(int slot, uint8_t *pt, size_t ptLen,
                                        uint64_t counter, bool hasChallenge,
                                        bool viaBle);
static size_t ozHexDecode(const String &hex, uint8_t *out, size_t cap);
static bool ozDpForwardable(uint8_t dp);
static void ozReportOutcome(int slot, const char *code, const String &detail);
static bool ozM4SelfTest();
static bool ozTuyaFrameOk(const uint8_t *f, size_t n);
static bool touchReadRegs(uint8_t *buf);
static size_t ozBuildDpFrame(uint8_t dp, uint8_t type, const uint8_t *val, size_t vlen, uint8_t *out);
static void addIdentity(JsonDocument &doc, bool full = false);
static void bond0Accept(OzBondVerdict v, const uint8_t provPub[32]);
static void copyLabelUtf8(const char *src, char *dst, size_t cap);
static void ctlConsume(size_t n);
static void ctlReset();
static void handleBondRevoke(int senderSlot, const uint8_t *v, size_t vlen);
static void handleInviteCancel(int senderSlot, const uint8_t *v, size_t vlen);
static void handleListBonds(int senderSlot, const uint8_t *v, size_t vlen);
static void ozControlDispatch(int slot, const uint8_t *frame, size_t flen);
static void ozSemanticDispatch(int slot, const char *json, size_t len, bool viaBle);
static void ozNotifyChunked(const String &json);
uint32_t clampHeartbeatS(uint32_t s);
uint32_t txlogCountLines(const char *path);
uint32_t txlogTotal();
void applyProvision(JsonDocument &doc);
void bleRearmAdvertising(bool connectable, const char *why);
void bleSetBusy(bool busy);
void buildTopics();
void checkFactoryResetButton();
void closeBleWindow(const char *why);
void drawAdvertising();
void drawFlash(const char *msg, uint16_t bg, uint16_t fg);
void drawJoining();
void drawOperational();
void drawStatusLine();
void ensureMqtt();
void enterKeepAliveSleep();
void factoryReset();
void forwardFrameToMcu(const uint8_t *frame, size_t fn);
void forwardHexToMcu(const String &hex);
void handleMcuFrame(const uint8_t *f, size_t n);
void handleMemberEnroll(JsonDocument &doc);
void loadConfig();
void loop();
void markDoorUnlocked();
void mrdySet(bool assertLow);
void notifyStatus(const char *wire);
void onMqttMessage(char *topic, byte *payload, unsigned int length);
void openBleWindow(const char *gesture);
void ozThreadApplyPoll(bool fast); // C9 §5 — defined with the Thread code
void pollThreadUdp();
void publishEnroll();
void publishHeartbeat();
void publishLog(const char *result, const char *detail, int actorSlot = -1);
void publishUnpairedAnnounce();
// XF-114 §13.4 — declared here because the BOOT-hold gesture and the DL MCU's
// 0x34 reset both call it well above its definition.
static void ozPublishResetOutcome(const char *reason, bool retain);
void saveConfig();
void setup();
void startBle();
void threadUdpBegin();
void tuyaWirePump();
void tuyaWireSend(const uint8_t *f, size_t n);
void txlogAppend(const char *result, const char *detail, int actorSlot = -1);
static void ozEvtPush(const char *verb, const char *result, const char *detail,
                      int actorSlot, uint32_t seq); // realtime push, defined below
extern bool g_clockLive;   // 1.74 clock provenance — defined with the clock code
static bool ozUplinkSend(int slot, const String &json); // defined below ozEvtPush's caller

// ── Pins, palette, TUYA UART pins, SRDY/MRDY, `bus`/`gfx` — all board-specific,
// defined by the per-board .ino before #include "../common/ozdoorlock_core.h" ──

// ── GATT contract (blelock/CONTRACT.md — unchanged so BANOI/MAOI pair as-is) ─
#define BLE_NAME "OZLOCK"
#define SVC_UUID "4f5a4b31-0001-4c4f-434b-000000000001"
#define CHR_PROVISION "4f5a4b31-0002-4c4f-434b-000000000001"
#define CHR_STATUS "4f5a4b31-0003-4c4f-434b-000000000001"
#define CHR_INFO "4f5a4b31-0004-4c4f-434b-000000000001"
// M3/M4 (CONTRACT.md "Operational / member profile"). …0006 `control` exists as
// of M4; until then it was deliberately absent, because an advertised-but-inert
// characteristic would let the app's capability probe conclude the lock can
// authorise an unlock when it could not.
#define CHR_CHALLENGE "4f5a4b31-0005-4c4f-434b-000000000001"
#define CHR_CONTROL "4f5a4b31-0006-4c4f-434b-000000000001"
#define CHR_MEMBER "4f5a4b31-0007-4c4f-434b-000000000001"

// Advertising connectable/non-connectable selector for
// BLEAdvertising::setAdvertisementType(uint8_t), which forwards the raw byte
// straight into the active backend's own param struct with NO translation
// (BLEAdvertising.cpp:140-148) — so the numeric value must already mean the
// right thing for whichever backend this board actually compiles.
//
// BUG, found + fixed 2026-08-06 (doorlock-1.10 regression — see XF-62 §9's
// correction and CONTRACT.md): this used to hardcode Bluedroid's
// esp_ble_adv_type_t values (IND=0x00, SCAN_IND=0x02) unconditionally. This
// board's actual build is NimBLE (confirmed via `arduino-cli --verbose` +
// a #ifdef probe sketch — CONFIG_NIMBLE_ENABLED, not CONFIG_BLUEDROID_ENABLED),
// whose conn_mode enum (ble_gap.h) is BLE_GAP_CONN_MODE_NON=0 /
// _DIR=1 / _UND=2 — DIFFERENT meanings at the same numbers. The old values
// were landing exactly backwards: "connectable" (0x00) actually set
// non-connectable, and the busy state (0x02) actually set connectable. This
// is why the lock always advertised (conn_mode doesn't gate basic scanning)
// but never accepted a BLE connection when it believed it was available, on
// every client, from the very first attempt of every boot.
#if defined(CONFIG_NIMBLE_ENABLED)
  #define OZ_ADV_TYPE_IND      BLE_GAP_CONN_MODE_UND  // connectable
  #define OZ_ADV_TYPE_SCAN_IND BLE_GAP_CONN_MODE_NON  // scannable, NON-connectable
#else
  // Bluedroid esp_ble_adv_type_t — Bluetooth Core spec PDU type codes,
  // spelled out rather than included (esp_gap_ble_api.h is not on the sketch
  // include path in core 3.3.11).
  #define OZ_ADV_TYPE_IND      0x00  // connectable, scannable, undirected
  #define OZ_ADV_TYPE_SCAN_IND 0x02  // scannable, NON-connectable, undirected
#endif
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
//   1.5  M3 — the member ceremony. `challenge` …0005 (16 fresh bytes per read,
//             destroyed on disconnect), `member_enroll` …0007, the bond TABLE
//             (M2's three flat keys migrate into slot 0 of 16), the 64-entry
//             nonce replay cache, and the advertised busy flag.
//             Also the M3 PREREQUISITE: a keypad touch now opens the same 60 s
//             BLE window a short BOOT press does. Without it the ceremony is
//             unreachable — BOOT is inside the door, the keypad is outside, so
//             a member standing at a commissioned lock had no way to make it
//             advertise. Serves WiFi-owner unlock and member unlock too
//             (XF-55 §13/§14 — one mechanism, four callers).
//             Also a battery fix M3 would otherwise have made routine: the nap
//             gate tested `bleServer == nullptr`, and bleServer is never nulled
//             once built — so the FIRST window a lock ever opened stopped it
//             sleeping again, permanently. Gated on the window/link instead.
//             Also XF-57 (AN), operator directive: the lock now REPORTS ITSELF —
//             `transport` + `caps` on every enroll and heartbeat. It never did,
//             so ozlockserv inferred capability from a bound bridge and the app
//             kept a private copy of `transport` from commissioning that nothing
//             corrected. A lock converted Thread→Wi-Fi stayed "Thread + bridge"
//             in the app forever and every "Mở cửa" took a remote path the
//             server then refused. Now it self-heals within one heartbeat.
//   1.6  Two bench-legibility fixes, both "the evidence was never emitted":
//             (a) selftest leg 9 `invite-b64url` FAILED on the first 1.5 flash —
//             our TEST, not our decoder. Transcribing ftpos's frozen OZINV1 QR
//             into a wrapped C literal dropped 16 chars out of the middle of the
//             64-`a` issuer run (324 in source vs 340 real); the decoder returned
//             242 bytes, exactly right for 323 data chars. Vector is now GENERATED
//             and chunked programmatically so it cannot drift by hand again.
//             (b) XF-53 (Y): [STATUS] now says whether it NOTIFIED or only stored
//             the value. It printed identically either way, so the one hypothesis
//             (Y) is about — a stale teardown clearing bleClientConnected, so
//             notify() is skipped — was invisible in our own serial.
//   1.7  XF-58 press-then-touch: the lock now ARMS an assisted unlock it cannot
//             satisfy yet, and fires it on the next keypad touch within 60 s.
//             1.6 required touch-BEFORE-press, which contradicts both XF-58 §3.1.2
//             and the countdown BANOI already ships (it starts on the press and
//             asks the visitor to touch during it). A SLEEPING lock hid this — the
//             press queues while it is offline, so the touch that wakes it is
//             recent. An AWAKE lock receives instantly and discarded the press;
//             at hb=60s with a 30s idle window that is ~half the time, failing
//             intermittently with the app countdown still running.
//   1.8  M4 — `control` …0006 and the DP dispatch split. An authenticated,
//             challenged, counter-floored command path: envelope opened under
//             the SENDER's bond (not just bond #0), challenge compared on EVERY
//             verb, floor advanced only on execution. DPID 101 `bond_revoke` and
//             102 `invite_cancel` are handled IN-LOCK and are the first verbs
//             that never touch the Tuya MCU.
//             The negative property is the load-bearing one: forwardHexToMcu()
//             now has an ALLOW-LIST (1, 21-24) instead of forwarding anything
//             that parsed. Before this, 101/102 arriving on the UNAUTHENTICATED
//             MQTT path would have been handed straight to a MCU that has no
//             concept of a bond — and anyone on the site Wi-Fi can publish there.
//             Also the 1.7 carry-in: the armed assisted unlock now fires BEFORE
//             openBleWindow() rather than after it. startBle() allocates ~43 KB
//             of Bluedroid and cost the unlock ~230 ms of latency behind a task
//             that has nothing to do with opening the door.
//   1.9  BENCH FIX — a `control` write could vanish with no answer at all.
//             ozControlTry() consumed the whole buffer with ctlReset() AFTER
//             notifying, and notifyStatus() holds a 150 ms settle after
//             notify(). So the client got its answer while the lock was still
//             inside that delay, wrote its next message immediately, the BLE
//             task appended it to the not-yet-cleared buffer, and the reset
//             then wiped both. ctlLen went to zero, so the idle backstop had
//             nothing to time out and never fired: a write producing NO status,
//             which is the exact XF-53 hang the backstop was added to prevent.
//             I had reasoned this race away while writing M4 on the grounds
//             that "the app waits for the status before writing again" — which
//             is backwards; waiting is what lands the write inside the window.
//             Now ctlConsume(n) drops only the processed bytes, under the
//             critical section, BEFORE any status goes out.
//   1.10 THE ONE-CONNECT-PER-BOOT BUG. A commissioned lock accepted exactly one
//             BLE connection after each reboot; every later touch window was
//             discoverable but NON-CONNECTABLE, so at-the-door unlock, member
//             enrolment and `Ghép lại` all failed with "sees the lock, cannot
//             connect" until the lock was power-cycled. Reproduced 2026-08-05
//             six times across TWO unrelated BLE stacks (bleak/CoreBluetooth and
//             flutter_blue_plus/iOS), which is what ruled the app out.
//             CAUSE: bleSetBusy() -> setScanResponseData() ->
//             esp_ble_gap_config_scan_rsp_data_raw() is ASYNC, and the Arduino
//             BLE library's completion handler for it calls
//             esp_ble_gap_start_advertising(&m_advParams) — restarting the
//             advert with whatever adv_type is set AT COMPLETION TIME. Both
//             callbacks called bleSetBusy() BEFORE bleSetConnectable(), so the
//             queued restart fired with the previous SCAN_IND while the struct
//             was then edited to IND and never re-applied (Bluedroid ignores
//             adv-param changes while advertising is running). Radio SCAN_IND,
//             struct IND, and openBleWindow()'s bare startAdvertising() a no-op.
//             FIX: one bleRearmAdvertising(connectable, why) — stop, settle, set
//             the TYPE FIRST, then the scan response, settle, start, and say on
//             the console which mode it is. bleSetConnectable() is deleted; a
//             helper that sets adv_type without the surrounding sequence is the
//             trap that caused this.
// 1.11 (2026-08-06): fixed the REAL cause of 1.10's "sees the lock, cannot
// connect" — OZ_ADV_TYPE_IND/SCAN_IND were Bluedroid values on a NimBLE build,
// landing exactly backwards on conn_mode. See the definitions above and
// bleRearmAdvertising()'s comment. 4 consecutive BLE connects verified with
// zero reboots between them (the acceptance test 1.10 was originally meant to
// pass). XFtposDecisions-62.md has the full story, including the wrong async-
// race theory this correction replaces.
// 1.12 was the 1.47" board's last version at fork time (DPID 103 list_bonds).
//
// ── doorlock19 — separate version namespace from here, own hardware port ───
//   19-1.0 (2026-08-06): first port to the Waveshare ESP32-C6 Touch-LCD 1.9".
//           All crypto/BLE/bond/MCU logic unchanged from doorlock-1.12 —
//           only the display/touch driver, SRDY/MRDY pins, and backlight
//           polarity changed for the new board. See the file header for the
//           full list. NOT yet hardware-verified — compiled only until the
//           existing test suite (M1-M4 self-tests, member ceremony, unlock,
//           bond ceremony) is re-run on this board and confirmed passing.
// FW_VERSION / FW_DISPLAY_VERSION — defined per-board before this #include.

// ── State machine ───────────────────────────────────────────────────────────
enum CommState { ST_ADVERTISING, ST_JOINING, ST_OPERATIONAL };
CommState state = ST_ADVERTISING;

Preferences prefs; // namespace "blelock" — shared with blelock deliberately

String cfgSsid, cfgPass, cfgBrokerHost, cfgServerIp, cfgSiteId, cfgName, cfgDeviceId;
// Broker credentials the server mints at enrollment. Persisted since the very
// first enrollment_ack handler ("buser"/"bsecret") — and, until 1.57, NEVER
// PRESENTED to the broker. See ensureMqtt().
String cfgBrokerUser, cfgBrokerSecret;
uint16_t cfgBrokerPort = 1883, cfgServerPort = 4200;
uint32_t cfgHeartbeatS = 300; // C9 §3 — was 60
bool provisioned = false, enrolled = false;
String cfgMode = "ozkey-cloud", cfgRoomNo, cfgMacToken;
bool isLocalMode() { return cfgMode == "ozkey-local"; }

String deviceId, macStr;

// ── Transport (2026-07-26, ozkey-10 unification) ────────────────────────────
// "wifi" (blecomm's original path) or "thread" (ported from threadcomm.ino).
// Selected once, at provisioning, by payload shape — see applyProvision().
String cfgTransport = "wifi"; // NVS "xport"
bool isThread() { return cfgTransport == "thread"; }

// ── C9 §1/§2 — Thread SED (sleepy) mode + configurable poll interval ────────
//
// The lock has run as a FULL Thread device (rx-on-when-idle) since 2026-07-28.
// That guarantees sub-second delivery and costs a continuously-powered radio:
// order 30-40 mA, i.e. ~3 days on 4xAA. A Sleepy End Device wakes on a timer,
// polls its parent, and sleeps — which is the only way this product reaches a
// battery life worth quoting.
//
// 🔴 THE TRADE IS NOT JUST LATENCY — A SED CANNOT HEAR MULTICAST.
// It polls its parent for UNICAST only; realm-local multicast is a link-layer
// broadcast that a sleeping radio is not present for. Our downlink today is
// ff03::1 (see threadUdpBegin's ground-truth comment, and bridge32's
// sendToThreadGroup), so a SED lock stops receiving commands, time beacons and
// everything else the bridge multicasts. That is why `cfgThreadSed` DEFAULTS
// OFF: flipping it without bridge-side unicast downlink turns a working lock
// deaf, silently. It is switchable now so the C9 current-draw measurement can
// be taken on a bench board without shipping that regression.
bool cfgThreadSed = false;      // NVS "sed"  — true = sleepy end device
uint32_t cfgThreadPollS = 5;    // NVS "poll" — parent poll interval, seconds

// Operator range 1-10 s. Below 1 s the poll traffic approaches rx-on's duty
// cycle and the saving evaporates; above 10 s a queued unlock feels broken.
uint32_t clampThreadPollS(uint32_t s) { return s < 1 ? 1 : (s > 10 ? 10 : s); }

// §5 — while the BLE window is open somebody is standing at the door, so the
// next command is imminent. 0 = "use the configured interval".
#define OZ_POLL_FAST_MS 1000UL

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
// ozkey-17 §2 U1: uplink gets its OWN port rather than sharing 5052. Two
// directions on one port would mean every receiver has to answer "is this a
// command for me, or my own echo coming back off the multicast group" on every
// datagram — a distinction easy to get subtly wrong and impossible to notice
// when it is. A second port makes it structural instead of conditional.
const uint16_t OZ_THREAD_UPLINK_PORT = 5053;

// ozkey-17 U1 hardening: the last peer that sent us a downlink — in practice
// always the bridge, since nothing else sends this lock commands. Captured in
// pollThreadUdp() and used by the uplink to answer by UNICAST, which is the
// only destination that gets link-layer ACKs and MAC retries. Declared here
// rather than beside the uplink code because pollThreadUdp() writes it and
// appears earlier in this file.
static struct sockaddr_in6 g_lastDownlinkPeer;
static bool g_haveDownlinkPeer = false;

// Diagnostic formatting. Added 1.29 after a uplink was lost WITH unicast
// enabled and 3 retry bursts (counter 197, 05:08:50) — and I could not tell
// why, because ozThreadUdpSendOnce() logged the destination's LABEL and never
// its address. Same blind spot as `mcast joined: ff03::4f5a`, which reported
// success for a group that has never delivered a packet: a log that names
// intent rather than fact cannot be used to diagnose anything.
static String ozIp6Str(const uint8_t a[16]) {
  char b[48];
  snprintf(b, sizeof(b),
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
           a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
  return String(b);
}

// fe80::/10. THE SUSPECTED CAUSE of the counter-197 loss: DoorA and the bridge
// are both attached as Child to the same parent, so they are NOT link-layer
// neighbours — their traffic routes via the parent. A link-local unicast
// between two children is therefore undeliverable, and lwip_sendto() would
// still return success, so `unicast=yes` could have been reporting a send to
// an address that can never arrive.
//
// Thread mesh-local addresses (ML-EID / RLOC) sit under the fd00::/8 ULA
// prefix — fd51:2839:… on this mesh — and ARE routable between children.
// Prefer those; refuse to learn a link-local peer.
static bool ozIp6IsLinkLocal(const uint8_t a[16]) {
  return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

// ── ozkey-19 R6 — is this peer address on a prefix WE are also on? ──────────
//
// 🔴 THE BUG THIS EXISTS FOR, measured 2026-08-11:
//
// The lock learned a peer at 02:51:13 and unicast every uplink to it from then
// on. None arrived. Enrol and revoke both executed correctly on the lock and
// the app was never told — the operator tapped revoke four times watching a
// spinner that could never resolve.
//
// The address was from a PREVIOUS partition. Thread's mesh-local prefix is
// regenerated per dataset, and the mesh re-formed underneath us: the same
// bridge interface ID appeared under fd51:…, fde0:… and fd7e:… within minutes.
// A cached full address does not survive that, and nothing detected it because
// lwip_sendto() happily returns success for an unroutable destination.
//
// This is ozkey-19 R1's one genuine downside, and it is worth naming: unicast
// is strictly better than multicast WHEN THE ADDRESS IS RIGHT (it gets
// 802.15.4's MAC ACK and retries), and strictly worse when it is stale,
// because multicast never needed an address at all. So we must validate.
//
// The check: does the peer share a /64 with any of our own non-link-local
// addresses? If it does not, it belongs to a mesh we are no longer part of and
// no amount of retrying will reach it.
//
// Deliberately permissive on failure — if we cannot read our own addresses we
// return true rather than discarding a peer that may be perfectly good. This
// guard exists to catch a specific, observed staleness, not to be a gatekeeper.
static bool ozPeerPrefixIsOurs(const uint8_t addr[16]) {
  otInstance *inst = esp_openthread_get_instance();
  if (!inst) return true;
  // Bounded wait, same discipline as threadUdpBegin(): a busy OT task must
  // cost us a permissive answer, never a blocked loop().
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) return true;
  bool match = false;
  for (const otNetifAddress *a = otIp6GetUnicastAddresses(inst); a && !match;
       a = a->mNext) {
    const uint8_t *m = a->mAddress.mFields.m8;
    if (ozIp6IsLinkLocal(m)) continue; // link-local prefixes are universal
    if (memcmp(m, addr, 8) == 0) match = true; // /64 compare
  }
  esp_openthread_lock_release();
  return match;
}

// ozkey-19 v2 R2 — PERSIST the uplink peer.
//
// g_haveDownlinkPeer used to be RAM-only, learned solely from an inbound
// downlink. So every reboot dropped the lock back to multicast — which has no
// MAC ACK and no retries (§2) — until the bridge happened to speak first. A
// lock that reboots with a roster change to report was therefore in the WORST
// delivery mode at exactly the moment it mattered, and on hardware with an
// open brownout suspicion that is not a rare path.
//
// 16 raw bytes under one NVS key. Deliberately NOT part of saveConfig(): the
// peer changes on its own schedule and must not drag the whole config blob
// into a write every time the mesh re-parents.
#define OZ_NVS_PEER_KEY "uppeer"

// ozkey-19 v2 R5 / ozkey-20 §7.2 — ROSTER EPOCH.
//
// Monotonic, bumped on every roster mutation, persisted. This is the
// correctness mechanism, not a convenience: the uplink push is a latency
// optimisation, and the epoch is what lets the app notice a missed change
// WITHOUT any push having succeeded. Compare-and-resync beats delivery
// guarantees for idempotent state.
//
// uint32 at one bump per mutation will not wrap in the life of the hardware.
// Persisted on every bump because the whole point is that it survives the
// reboot that lost the notification.
#define OZ_NVS_EPOCH_KEY "repoch"
static uint32_t g_rosterEpoch = 0;

static void ozRosterEpochLoad() {
  prefs.begin("blelock", true);
  g_rosterEpoch = prefs.getUInt(OZ_NVS_EPOCH_KEY, 0);
  prefs.end();
}

// Returns the NEW value. Callers must bump BEFORE building any payload that
// carries it, so the epoch an app receives is the one describing the change it
// is being told about.
static uint32_t ozRosterEpochBump() {
  g_rosterEpoch++;
  prefs.begin("blelock", false);
  prefs.putUInt(OZ_NVS_EPOCH_KEY, g_rosterEpoch);
  prefs.end();
  return g_rosterEpoch;
}

static void ozUplinkSavePeer(const uint8_t a[16]) {
  prefs.begin("blelock", false);
  prefs.putBytes(OZ_NVS_PEER_KEY, a, 16);
  prefs.end();
}

// Clear the NVS copy as well. Without this a reboot would faithfully restore
// the very address we just proved unroutable (ozkey-19 R2 persists it).
static void ozUplinkForgetPeer() {
  prefs.begin("blelock", false);
  prefs.remove(OZ_NVS_PEER_KEY);
  prefs.end();
}

static void ozUplinkLoadPeer() {
  uint8_t a[16];
  prefs.begin("blelock", true);
  const size_t n = prefs.getBytes(OZ_NVS_PEER_KEY, a, sizeof(a));
  prefs.end();
  if (n != 16) return;
  // Refuse a stored link-local for the same reason we refuse a learned one.
  // Also refuse all-zeroes, which is what a half-written key reads back as.
  bool allZero = true;
  for (int i = 0; i < 16; i++)
    if (a[i]) { allZero = false; break; }
  if (allZero || ozIp6IsLinkLocal(a)) return;
  memset(&g_lastDownlinkPeer, 0, sizeof(g_lastDownlinkPeer));
  g_lastDownlinkPeer.sin6_family = AF_INET6;
  memcpy(&g_lastDownlinkPeer.sin6_addr, a, 16);
  g_haveDownlinkPeer = true;
  Serial.printf("[UPLINK] peer restored from NVS: %s\n", ozIp6Str(a).c_str());
}
// ozkey-17 F8: 512 -> 1024. OZKIE semantic JSON is 2-3x the size of the Tuya
// frame it replaces once hex-encoded into `envelope_hex` — a grant_pin with a
// hex credential lands near 430 B of datagram against 512, with RFID
// credentials longer still. Headroom is cheaper than a truncation bug that
// only shows up on the longest credential anyone ever issues.
#define OZ_UDP_RX_BUF 1024
OThreadUDP threadUdp;
bool threadUdpReady = false;
unsigned long threadUdpLastAttempt = 0;
#define THREAD_UDP_RETRY_MS 3000UL

// XF-116 — beacon the moment we can talk, not `cfgHeartbeatS` after BOOT.
//
// The beacon gate is `millis() - lastThreadBeaconAt > cfgHeartbeatS * 1000`
// with lastThreadBeaconAt starting at 0, so the FIRST beacon of a lock's life
// lands 300 s after boot no matter how quickly Thread came up. Measured
// 2026-08-19: LockA attached 40 s after a factory reset and said nothing for
// another 4 m 34 s.
//
// Nothing downstream can compensate. That beacon is the only unprompted thing
// a Thread lock ever says, so until it goes out: bridge32 cannot map our ext
// address to our device_id (so it cannot publish presence for us — the ext
// CHANGES across a factory reset, so it cannot be cached either), the server
// answers "unreachable" and 409s every remote unlock, and `need_time` has not
// been asked so the clock stays UNKNOWN and time-bounded credentials are
// unenforceable. One silent timer, four visible failures.
bool g_threadBeaconDue = false;

// BLE
BLEServer *bleServer = nullptr;
BLECharacteristic *chrStatus = nullptr, *chrInfo = nullptr, *chrMember = nullptr;
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

// ── PAIRING IS GESTURE-GATED, EVEN WHEN UNPROVISIONED (operator, 2026-08-12) ─
//
// An unprovisioned lock used to advertise CONTINUOUSLY AND FOREVER. Three
// factory-reset locks in a hallway therefore showed up as three simultaneous
// rows, and the app could not tell which was which — the problem XF-94 tried to
// solve by labelling the rows. The operator's correction is better: do not
// create the ambiguity. A lock nobody touched should not be advertising at all,
// so the only device discoverable during pairing is the one in your hand.
//
// It also shrinks the attack surface (a lock nobody touched cannot be probed or
// provisioned) and saves radio time, and it matches the rule we already apply
// everywhere else: PHYSICAL PRESENCE GATES THE GRANT.
//
// WHY A BOOT WINDOW AND NOT PURELY THE GESTURE: if a freshly reset lock were
// silent until someone pressed the right button, a lock whose button or touch
// panel misbehaves would look dead to the app with no way in short of
// reflashing — worse on a screen-less production unit. You have just reset it
// and you are standing there, so the first two minutes are free; after that it
// goes dark until a short BOOT press opens the normal 60 s window.
#define BLE_BOOT_ADV_MS 120000UL
unsigned long bleBootAdvUntil = 0; // millis deadline for the post-boot grace

// Single source of truth for "should we be discoverable right now".
bool bleAdvertisingAllowed() {
  if (bleWindowOpen()) return true;                       // gesture, either state
  return !provisioned && millis() < bleBootAdvUntil;      // post-boot grace only
}
bool bleWindowOpen();
String provBuf;

// M3. The challenge is per-CONNECTION, not per-lock: fresh 16 bytes on every
// read of …0005, wiped on disconnect. M4's `control` compares against it
// unconditionally (XF-47) — a revoked-then-re-invited bond restarts at
// counter_floor 0, so captured frames clear the floor and the stale challenge is
// the only thing left stopping the replay. Kept here rather than inside the
// callback so onDisconnect can destroy it.
uint8_t bleChallenge[16];
bool bleChallengeValid = false;
String memberBuf; // chunked …0007 JSON, same reassembly rule as provBuf

// ── M4: the …0006 `control` reassembly buffer ────────────────────────────────
//
// `control` is the one characteristic whose payload is BINARY, so it cannot use
// the "a chunk starting '{' resets the buffer, parse when the JSON completes"
// rule the other two share. The wire shape is frozen —
// utf8(app_id_hex, 64) ‖ envelope — and the envelope carries no length field, so
// there is nothing in the bytes that says how many chunks are still coming.
//
// The GCM tag IS the completeness oracle, so we use it as one: on every chunk,
// once enough bytes exist to be a shortest-possible message, try to open. A
// truncated envelope fails the tag; the complete one passes. That makes the
// happy path instant and needs no wire change. (At the MTU 247 the app requests,
// a control write is one chunk anyway and this never loops.)
//
// What the tag CANNOT tell us is "incomplete" from "forged" — both are just a
// failure. Left there, a corrupted write would get silence, and silence is the
// exact failure mode XF-53 was: the app writes, waits for a status that is never
// coming, and hangs. So an idle timer closes it out: no further chunk for
// OZ_CTL_IDLE_MS means the message is as complete as it is ever going to be, and
// it gets a definite UNLOCK_DENIED.
//
// WHICH TASK runs this is a design decision, not an accident. onWrite() fires on
// the BLE task; the idle timer fires in loop() on the Arduino task. If both
// could open envelopes, they would race over this buffer and over the plaintext
// they decrypt into. So the BLE task ONLY appends — under a critical section,
// because a torn ctlLen is a torn message — and every expensive or blocking step
// (X25519, GCM, the NVS floor write, the 300 ms self-revoke flush) happens in
// loop(). That also keeps a 512-byte plaintext buffer and a key agreement off
// the BLE stack, which is the same constraint that forced M3's invite decode
// buffer to be static.
#define OZ_CTL_MAX 512      // 64 hex + envelope(21+16) + the largest v1 frame
#define OZ_CTL_IDLE_MS 400UL
static uint8_t ctlBuf[OZ_CTL_MAX];
static size_t ctlLen = 0;
static unsigned long ctlLastChunkAt = 0;
static volatile bool ctlNewBytes = false;
static portMUX_TYPE ctlMux = portMUX_INITIALIZER_UNLOCKED;

// Networking
// ── Blocking budget for a broker dial (2026-08-13) ──────────────────────────
// Every one of these is time the MAIN LOOP is stopped dead — see ensureMqtt().
// TCP connect is bounded by NetworkClient's own default (3000 ms,
// WIFI_CLIENT_DEF_CONN_TIMEOUT_MS); we tighten it, and tighten PubSubClient's
// CONNACK wait from its 15 s default (MQTT_SOCKET_TIMEOUT) to match.
#define OZ_MQTT_TCP_TIMEOUT_MS 2000UL
#define OZ_MQTT_CONNACK_TIMEOUT_S 2
#define OZ_MQTT_RETRY_MIN_MS 4000UL
#define OZ_MQTT_RETRY_MAX_MS 60000UL
WiFiClient wifiTcp;
PubSubClient mqtt(wifiTcp);
unsigned long lastHeartbeat = 0, lastMqttAttempt = 0, wifiJoinStart = 0;
// ── MQTT reconnect backoff (2026-08-13) ─────────────────────────────────────
// A flat 4 s retry against an unreachable broker meant the lock re-entered a
// BLOCKING connect every 4 s, forever. See ensureMqtt() for the full reasoning
// — this is the interval, and it doubles on each failure instead.
unsigned long mqttRetryMs = OZ_MQTT_RETRY_MIN_MS;
unsigned long lastThreadBeaconAt = 0; // ozkey-20 R3 — Thread presence beacon
unsigned long lastEnrollSent = 0;
uint8_t enrollAttempts = 0;
unsigned long lastUnpairedAnnounce = 0;
String topicCommand, topicEnroll, topicHeartbeat, topicLog, topicPairConfirm;
// XF-114 §9 — the lock's own presence topic. The SERVER has subscribed to
// `ozkie/<site>/locks/+/presence` and carried a handleLockPresence() since
// ozkey-20 R1, but NOTHING IN THIS FIRMWARE HAS EVER PUBLISHED TO IT. The
// consumer was built and the producer never was, so the topic has been silent
// its whole life. 1.95 makes the lock the first publisher — see
// ozPublishResetOutcome().
String topicPresence;
// XF-114 §13.4 — the `msg_id` of the MQTT command currently being handled, so
// an outcome can be correlated to the request that caused it instead of being
// matched by timing. Empty for anything not delivered over MQTT (BLE control,
// Thread, the BOOT-hold gesture, the DL MCU's own reset button).
String g_cmdMsgId;
String topicUplink; // ozkey-17 U1 — sealed lock->app content, opaque to the server
String topicTime;   // ozkey-33 — site-wide RETAINED {"utc","tz"}, read-only to us
String topicCommandLegacy; // S16 — pre-rename root, subscribed during migration only
#define TOPIC_UNPAIRED "hotel/locks/unpaired/heartbeat"

bool screenDirty = true;
String joinLine1 = "", joinLine2 = "";
bool touchWasDown = false;

// XF-58: the assisted ("visitor at the door") unlock. The server may queue one,
// but ONLY the lock can know whether anybody actually touched the keypad — so
// only the lock can enforce it, and it must, because a queue expiry cannot.
//
// Without this check the touch requirement is not a requirement at all, merely
// a probability: a sleeping lock also wakes on its heartbeat timer, so a queued
// unlock would be collected untouched roughly (window / heartbeat_s) of the
// time — ~25% at a 15 s window on a 60 s heartbeat, and ~100% once the window
// reaches the interval. Lengthening the window to help the visitor would have
// made an unattended open MORE likely, which is the opposite of the intent.
unsigned long lastTouchAt = 0; // 0 = never touched since boot
#define ASSISTED_TOUCH_MAX_MS 30000UL

// The command may arrive on EITHER side of the touch, and both orderings are real:
//   press-then-touch — the owner taps "Mở cửa cho khách" and then tells the
//     visitor to touch. This is the flow BANOI's countdown is built around and the
//     one XF-58 §3.1.2 documents, so it is the primary case.
//   touch-then-press — "hold on, I'll open it", the visitor touches first.
//
// A sleeping lock gets press-then-touch for free: the command sits in the server
// queue because the lock is offline, and by the time the touch wakes it the touch
// is recent. An AWAKE lock does not — it receives instantly, and at the default
// 60 s heartbeat with a 30 s idle window it is awake roughly half the time. So the
// press was silently discarded on a coin flip, with the app's countdown still
// running. Holding it closes that.
//
// Safety is unchanged: still bounded, still requires a touch, still single-shot.
// RAM only — a reboot drops it, which is correct.
unsigned long assistArmedUntil = 0; // 0 = nothing armed
String assistArmedPayload;
#define ASSISTED_ARM_MS 60000UL // matches ozlockserv's expires_at window

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
// ── ozkey-21 T1/T2 — module clock + MCU time service ────────────────────────
// The MODULE is the time source and the lock MCU is its client. We are the
// module. Codec and rules live in oztime.h; this is just the live state.
OzClock ozclock;
// ozkey-21 — timezone, minutes east of UTC. Learned from the bridge's beacon
// (Thread) or the app at provisioning (Wi-Fi); 0 = UTC until told otherwise.
// Persisted so a rebooted lock shows correct local time before the next beacon.
int16_t cfgTzMin = 0;
/*
 * Panel repaint generation.
 *
 * drawStatusLine() and drawHexReadout() both skip work when their content is
 * unchanged, which is what stops the panel flickering. That optimisation is
 * only valid while the pixels they drew are still on the screen — and a full
 * fillScreen() erases them. Bump this counter on every full clear; the gated
 * drawers compare it and force one repaint when it moves.
 *
 * Getting this wrong blanks the entire panel and leaves only the unconditional
 * clock line visible (observed 2026-08-11 on doorlock-1.46), because each
 * gated drawer independently concluded it had nothing to do.
 */
uint32_t panelGen = 0;
bool mcuWantsTimePush = false;   // MCU sent 0x34 sub 0x01 (subscribe)
uint32_t mcuTimeRequests = 0;    // 0x0C/0x1C asked of us
uint32_t mcuTimeServed = 0;      // answered WITH a real time
uint32_t mcuTimeUnknown = 0;     // answered "I do not know" (flag byte 0)
unsigned long lastTimePushAt = 0;
// Push cadence once subscribed: DAILY (operator's call, 2026-08-11).
//
// This said hourly, which was never derived from anything — ozkey-21 §3.3
// specifies "a daily beacon is ample" and the code contradicted its own design
// doc. 24 h is the spec.
//
// ⚠ The drift budget behind "daily is ample" cited an external 32.768 kHz
// crystal at ~20 ppm. That crystal is on our PCB but the Arduino build does
// NOT use it — CONFIG_RTC_CLK_SRC_INT_RC=y in the precompiled IDF libs, so the
// RTC runs on the internal RC. It is recalibrated against the main crystal
// while awake, so a running lock is fine; a production lock that deep-sleeps
// for hours needs this cadence re-derived from MEASURED drift, not assumed.
#define MCU_TIME_PUSH_MS 86400000UL

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
// 1-15 min (60-900 s); clamp whatever provisioning/ack delivers.
//
// C9 §3 (operator, 2026-08-15): range widened 600 -> 900 s and the DEFAULT set
// to 300 s. Note for the record — the directive described the old default as
// 600 s; it was actually 60 s in every one of the three places it was spelled
// (declaration, loadConfig, applyProvision). So this change makes a Wi-Fi lock
// wake FIVE TIMES LESS often than before, not twice as often. That is the
// battery-favouring direction the directive intended, but it also multiplies
// worst-case latency for a queued remote command by five, so it is worth
// knowing which way the number actually moved.
uint32_t clampHeartbeatS(uint32_t s) {
  return s < 60 ? 60 : (s > 900 ? 900 : s);
}

// ── MCU bus health (drives the dashboard) ───────────────────────────────────
// Doorbell-opened BLE windows are rate-limited, because a doorbell can be
// spammed and on a sleepy lock the radio IS the power budget. Ringing during an
// OPEN window still extends it, so this never interrupts a real enrolment — it
// only caps how much advertising a stranger at the door can force.
// 120 s, and the WINDOW is what changed instead (operator, 2026-08-18).
//
// The first fix attempted here was halving this to 60 s, on the theory that
// the cooldown was what stranded a user who missed the window. The operator
// supplied the measurement that showed the real cause: **BLE advertising takes
// ~15 s before the app can even SEE the lock.** So a 30 s window gave a person
// ~15 s to unlock a phone, open the app and tap — and it routinely closed
// underneath them. The window was too short; the cooldown was never the
// problem. Widening the window (BLE_WINDOW_MS, now 60 s) fixes the actual
// failure, and lets the cooldown stay at the full 2 min it needs to be for the
// battery argument to hold.
#define OZ_BELL_COOLDOWN_MS 120000UL // 2 min of quiet after a bell window closes
static unsigned long g_bellWindowEndedAt = 0;

// ── has_doorbell — reported on info, so the app stops telling people to press
// a button their lock may not have (XF-107, operator 2026-08-16) ────────────
//
// TWO SOURCES, and the weaker one is the default:
//
//   1. THE PRODUCT PROFILE. profiles/ is the per-product DP SELECTION, which
//      is precisely "which model is this" expressed as data. tuya-ds013-t3
//      selects DP 53; ozkie-legacy-v0 (our default) does not. So the flag is
//      false unless a profile that declares a doorbell has been selected —
//      exactly the operator's rule: default false unless explicitly
//      configured.
//   2. OBSERVATION. If a DP 53 ever actually arrives from the MCU, the lock
//      HAS a doorbell — that is proof, not configuration, and it cannot be a
//      false positive. Latched and persisted so one press settles it forever.
//
// 🔴 THE ASYMMETRY IS DELIBERATE (operator): a false positive is worse than a
// false negative. Telling someone to press a button that does not exist leaves
// them stuck at a door; telling them to use a fallback merely costs a step. So
// nothing here infers a doorbell from silence, and only real evidence upgrades
// the answer.
static bool g_bellObserved = false;
static bool ozHasDoorbell() { return g_bellObserved || ozDpFind(53) != nullptr; }

// ── Tuya 0x01 — ASK THE MCU WHAT IT IS, instead of being told ──────────────
//
// The protocol has had this all along and we never used it (supplier doc
// §1: module sends `55 aa 00 01 00 00 00`, MCU answers
// `{"p":"<PID>","v":"<mcu fw>"}`). The PID is the product identity Tuya
// assigns, and `profiles/products/*.json` already carry it as `supplier.pid`
// — so the lock can look up its OWN DP map rather than shipping a different
// firmware per model, or trusting a default that is our invented map.
//
// Why this matters beyond has_doorbell: the profile decides which DPs are
// forwardable and how credentials are encoded. Booting on the wrong one is
// how `ozDpForwardable()` came to forward the SETTINGS DPs and block every
// credential operation (ozkey-27 §2.1) — the precise inverse of its intent.
//
// 🔴 UNKNOWN PID KEEPS THE CURRENT PROFILE. Never guess: a lock that reports
// something we have no map for is exactly the lock we must not improvise on.
String cfgMcuPid, cfgMcuVer;
static unsigned long g_pidAskedAt = 0;
static uint8_t g_pidAsks = 0;
// True once an MCU has told us what it is THIS BOOT. A second, contradicting
// answer is refused rather than adopted — see the 0x01 handler.
static bool g_pidLatched = false;
// 🔴 The MCU reported a product this build was not made for. Surfaced on the
// heartbeat because a mismatch that only exists in a serial log nobody is
// reading is not a diagnostic — and this one means every DP on the wire is
// being read under the wrong map.
static bool g_profileMismatch = false;
#define OZ_PID_RETRY_MS   5000UL
#define OZ_PID_MAX_ASKS   6

static void ozAskMcuProductInfo() {
  const uint8_t f[7] = {0x55, 0xAA, 0x00, 0x01, 0x00, 0x00, 0x00};
  Serial.println("[PID] asking the MCU what it is (0x01)");
  tuyaWireSend(f, sizeof(f));
  g_pidAskedAt = millis();
  g_pidAsks++;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x08 — ASK THE MCU WHICH DPs IT ACTUALLY HAS (operator, 2026-08-20)
// ─────────────────────────────────────────────────────────────────────────────
//
// 0x01 tells us the product's IDENTITY (the PID). 0x08 tells us its
// CAPABILITIES: the MCU answers by reporting the current status of every DP it
// supports, as a series of (or one grouped) 0x07 frames. Together they are the
// complete picture; we have only ever asked the first half.
//
// WHY THIS MATTERS HERE. The DP map is pinned at build time and the PID only
// confirms the product NAME. Nothing has ever checked the pinned profile
// against what the hardware really implements — so a profile that selects a DP
// the MCU does not have looks perfectly healthy until a command silently does
// nothing, which is precisely how 2026-08-20 was spent.
//
// It also answers, from the hardware rather than from a document, the question
// blocking remote unlock: does this MCU actually implement DP 76? And it tells
// us which of the 15 RESERVED DPs physically exist — evidence we cannot get
// any other way while the supplier's payload layouts are missing (ozkey-42).
//
// We only OBSERVE. A DP the MCU reports but our profile lacks is logged, never
// adopted: the profile is a build-time decision and the far end of the UART
// does not get a vote (XF-118 §4).
static uint8_t g_dpSeen[32];      // bitmap, DP 0..255 — one bit per DP
static bool g_dpListAsked = false;
static unsigned long g_dpListAskedAt = 0;
static bool g_dpListReported = false;
// Census ends after this much SILENCE from the MCU, not this long after the
// query — see the census branch in handleMcuFrame().
#define OZ_DPQ_IDLE_MS 2500UL

static void ozNoteDpSeen(uint8_t dp) { g_dpSeen[dp >> 3] |= (uint8_t)(1u << (dp & 7)); }
static bool ozDpWasSeen(uint8_t dp) { return (g_dpSeen[dp >> 3] >> (dp & 7)) & 1u; }

static void ozAskMcuDpList() {
  // 55 AA 00 08 00 00 07 — checksum 0x07 over the six preceding bytes.
  const uint8_t f[7] = {0x55, 0xAA, 0x00, 0x08, 0x00, 0x00, 0x07};
  Serial.println("[DPQ] asking the MCU which DPs it has (0x08)");
  tuyaWireSend(f, sizeof(f));
  g_dpListAsked = true;
  g_dpListAskedAt = millis();
}

/** Compare what the MCU reported against the profile we were built with. */
static void ozReportDpListComparison() {
  const OzProfile *p = ozProfile();
  uint8_t missing = 0, extra = 0;
  Serial.printf("[DPQ] MCU DP list vs profile '%s':\n", p->id);
  for (uint16_t i = 0; i < p->count; i++) {
    const uint8_t dp = (uint8_t)p->entries[i].dp;
    if (!ozDpWasSeen(dp)) {
      // Only meaningful for DPs the MCU would volunteer. A command-only DP is
      // not expected in a status dump, so this is a hint, not a verdict.
      Serial.printf("[DPQ]   profile has DP %-3u (%s) — MCU did not report it\n",
                    dp, p->entries[i].name);
      missing++;
    }
  }
  for (uint16_t dp = 0; dp < 256; dp++) {
    if (ozDpWasSeen((uint8_t)dp) && !ozDpFind((uint8_t)dp)) {
      Serial.printf("[DPQ]   🔴 MCU reports DP %-3u which this profile does NOT "
                    "have — logged, NOT adopted\n", (unsigned)dp);
      extra++;
    }
  }
  Serial.printf("[DPQ] %u profile DPs unreported, %u MCU DPs unknown to us\n",
                (unsigned)missing, (unsigned)extra);
  if (extra) g_profileMismatch = true; // the hardware is not what we built for
}
// Set when the CURRENT window was opened by the doorbell. closeBleWindow()'s
// argument is the reason it CLOSED ("30s elapsed"), not what opened it, so the
// gesture has to be remembered rather than sniffed out of that string.
static bool g_bellOpenedWindow = false;

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

// ─────────────────────────────────────────────────────────────────────────────
// ozkey-29 §11.3/§11.4 — SEQUENCE, AND ADMITTING LOSS
//
// The ring above already existed and already survives the network being down;
// ozkey-29 does not need a second one (§11.3 proposed 4096 records before I had
// read this — 10,000 is already 2.4x that, so the sizing question is closed).
// What it lacked is the two properties an AUDIT log needs that an operational
// one does not:
//
//   1. A monotonic `seq` per record, so `query_events` can be a cursor rather
//      than "send me everything" — and so a replayed request is idempotent
//      (ozkey-27 §4.4 R1).
//   2. An honest statement of what was DROPPED. Rotation deletes /txlog.1
//      outright. Until now that happened silently, so an app pulling history
//      could not tell a complete record from one with a 5,000-event hole in it.
//      The Sovereign Edge claim rests on the APP holding the complete history
//      (ozkey-29 §10.5); a log that quietly forgets cannot support that, and
//      silent loss is the exact failure class this project keeps rediscovering
//      — a credential dropped on a bare `break`, UNLOCK_OK with no ack.
//
// `seq` is recovered from the last line of /txlog.0 at boot rather than being
// counted in NVS. NVS has a write-endurance budget and door events are the
// highest-frequency thing this device does; the number is already durably in
// the log, so storing it twice would be both redundant and the more fragile of
// the two copies. `dropped_before_seq` DOES go to NVS — it changes once per
// 5,000 events, which is no wear at all.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t g_evtSeq = 0;            // last seq WRITTEN. Next record is g_evtSeq+1.
uint32_t g_evtDroppedBefore = 0;  // records < this are gone forever. 0 = none.

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

// ── ozkey-29 §11.2 / ozkey-28 §3.4 — records are OZKIE `event.*` VERBS ──────
//
// The log used to store {seq, ts, result, detail} — free text. Not DP frames,
// so it never had the catalogue-dependence trap §11.2 warns about, but not a
// schema either: the app could render it only as a string, and could not
// filter, group or reason about it. §11.2's requirement is a STABLE verb, and
// the point of that stability is that it survives phase 0, when every source DP
// number changes (our DP 8 is invented; a real access event arrives on
// 61/63/64/69/72/73/76 by credential class). Translating at the boundary means
// the log never learns a DP number at all.
//
// The mapping below is deliberately total: every existing publishLog() call
// site is accounted for, and anything unrecognised becomes `event.device`
// rather than being dropped or silently mislabelled. An audit log that
// discards what it does not understand is the same silent-loss failure
// dropped_before_seq exists to prevent, one level down.
static const char *ozEvtVerbFor(const char *result) {
  if (!result) return "event.device";
  // Access outcomes — the MCU's DP 8 today, the 61/63/64/... family after
  // phase 0. Same verb either way, which is the entire point.
  if (!strcmp(result, "granted") || !strcmp(result, "denied") ||
      !strcmp(result, "expired"))
    return "event.access";
  if (!strcmp(result, "battery_alarm")) return "event.battery";
  // XF-118 P4 — DP 53. The catalogue has declared `verb: event.doorbell` for
  // this DP since rev 1; firmware published it as a bare `publishLog("doorbell")`
  // which fell through to `event.device` below, so the app received a generic
  // device line for the one event a visitor actually generates. The DP was
  // handled correctly all along — only its verb was wrong, which is the kind of
  // mismatch that looks like a missing feature from the consumer's end.
  if (!strcmp(result, "doorbell")) return "event.doorbell";
  // Roster changes are in-lock facts, not door events, but they belong in the
  // audit trail: "who was allowed to open this door, and when did that change"
  // is exactly the question an owner asks of a log.
  if (!strcmp(result, "bond_revoked") || !strcmp(result, "bond_expired") ||
      !strcmp(result, "invite_cancelled") || !strcmp(result, "bonds_listed"))
    return "event.roster";
  // mcu_timeout / dp_unclassified are health, not access. Kept because they are
  // how a lock reports that it could NOT do what it was asked — see ozkey-28 §4.
  return "event.device";
}

void txlogAppend(const char *result, const char *detail, int actorSlot) {
  if (!fsUp) return;
  if (txlogCount0 >= TXLOG_ROTATE_LINES) {
    // Rotation DESTROYS /txlog.1. Record what the app can therefore never see
    // again, before the evidence of it is deleted along with the file.
    if (txlogCount1) {
      // What SURVIVES this rotation is /txlog.0's records — they become the new
      // /txlog.1. So the oldest still-readable seq is (head - count0 + 1), and
      // everything below it has just been deleted with the old /txlog.1.
      //
      // My first version subtracted txlogCount1 as well, which named the start
      // of the file being DESTROYED rather than the start of what remains — it
      // would have advertised 5,000 records as available immediately after
      // erasing them. An audit log that under-reports its own loss is worse
      // than one that keeps no log at all, because it is believed.
      g_evtDroppedBefore =
          (g_evtSeq > txlogCount0) ? (g_evtSeq - txlogCount0 + 1) : 1;
      prefs.begin("blelock", false);
      prefs.putUInt("evtdrop", g_evtDroppedBefore);
      prefs.end();
      Serial.printf("[EVT] rotation dropped %lu records; history now starts at seq %lu\n",
                    (unsigned long)txlogCount1, (unsigned long)g_evtDroppedBefore);
    }
    LittleFS.remove("/txlog.1");
    LittleFS.rename("/txlog.0", "/txlog.1");
    txlogCount1 = txlogCount0;
    txlogCount0 = 0;
  }
  File f = LittleFS.open("/txlog.0", "a");
  if (!f) return;
  JsonDocument doc;
  doc["seq"] = ++g_evtSeq;
  doc["kind"] = ozEvtVerbFor(result);   // §11.2 — the stable verb, not a DP
  String ts = isoNow();
  if (ts.length()) doc["ts"] = ts; else doc["up_ms"] = millis();
  // ozkey-28 §3.4 asks event.access to carry a time_basis so the app never
  // treats a guessed clock as authoritative. We have exactly that distinction
  // now (1.74 g_clockLive), so state it rather than let the app infer it from
  // whether `ts` happens to be present: 2 = a real sync, 1 = device-local
  // guess restored from NVS, 0 = no clock at all.
  doc["time_basis"] = !ozClockKnown(ozclock) ? 0 : (g_clockLive ? 2 : 1);
  doc["result"] = result;
  doc["detail"] = detail;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
  txlogCount0++;

  // Written to flash FIRST, then pushed. If the radio is down the record still
  // exists and the app collects it later via query_events; if we pushed first
  // and then failed to persist, the owner would see a notification for an event
  // with no audit trail behind it.
  ozEvtPush(doc["kind"], result, detail, actorSlot, g_evtSeq);
}

/**
 * Recover `g_evtSeq` from the last record on flash. Called once at boot.
 *
 * Reading the tail rather than trusting a counter: the log IS the record, so a
 * second copy in NVS could only ever disagree with it, and would be the copy
 * that lied. A fresh or unreadable log restarts at 0, which is honest — the app
 * sees seq numbers below anything it holds and can tell the lock was wiped.
 */
static void ozEvtSeqRestore() {
  if (!fsUp) return;
  prefs.begin("blelock", true);
  g_evtDroppedBefore = prefs.getUInt("evtdrop", 0);
  prefs.end();

  const char *path = LittleFS.exists("/txlog.0") ? "/txlog.0" : "/txlog.1";
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  // Walk back to the start of the final line rather than parsing 5,000 records.
  const size_t sz = f.size();
  size_t back = sz > 256 ? 256 : sz;
  f.seek(sz - back);
  String tail = f.readString();
  f.close();
  int nl = tail.lastIndexOf('\n', tail.length() - 2);
  String last = nl >= 0 ? tail.substring(nl + 1) : tail;
  JsonDocument doc;
  if (deserializeJson(doc, last) == DeserializationError::Ok)
    g_evtSeq = doc["seq"] | 0u;
  Serial.printf("[EVT] log resumed at seq %lu (dropped_before=%lu)\n",
                (unsigned long)g_evtSeq, (unsigned long)g_evtDroppedBefore);
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

/*
 * Short device_id for the panel: "ozk" + 8 hex — e.g. `ozk-acebe639` from
 * `ozk-acebe639f8c4` (operator, 2026-08-11).
 *
 * 8 hex characters is 4 bytes of MAC, which is unambiguous across any fleet
 * we will ever put on one bench or in one building, and it buys back the
 * width the clock now needs. The FULL id still appears on the advertising
 * screen and in every log line — this shortening is for the operational
 * screen, where you are identifying a lock you are standing in front of, not
 * typing an MQTT topic.
 */
String ozShortId(const String &id) {
  int dash = id.indexOf('-');
  if (dash < 0 || (int)id.length() <= dash + 1) return id;
  return id.substring(0, dash + 1) + id.substring(dash + 1, dash + 9);
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
  // XF-53 (Y): say WHICH of the two things happened. Until now this line printed
  // identically whether the status was notified or merely stored — so the one
  // hypothesis (Y) is actually about, "a stale teardown cleared
  // bleClientConnected and notify() was skipped", was invisible in our own
  // serial. Same class of gap ftpos just closed on their side by tagging
  // notify-vs-poll at the call site; this is its mirror image, and without both
  // halves the T2 re-run cannot be read either way.
  //
  // Printed BEFORE setValue/notify deliberately: the 150 ms settle below would
  // otherwise shift every notified line's timestamp, and the missing-150 ms
  // question in §7.1 is exactly a timing question. Captures stay comparable with
  // every one taken before today.
  const bool live = (chrStatus != nullptr) && bleClientConnected;
  Serial.printf("[STATUS] %s (%s, links=%d)\n", wire,
                live ? "notified" : "SET ONLY — no live link", bleLinkCount);
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
// Screens (rotation 3 landscape 320×170, standard RGB palette)
// ─────────────────────────────────────────────────────────────────────────────

// drawSplash() — board-specific, defined before this #include (no-op is fine).

void drawAdvertising() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, C_AMBER);
  gfx->setTextColor(C_AMBER);
  gfx->setTextSize(1);
  gfx->setCursor(15, 12);
  gfx->println("OZLOCK COMM MODULE (doorlock)");
  gfx->setCursor(15, 28);
  gfx->print("BLE: ");
  // ── SAY WHICH IT IS (operator, 2026-08-14) ───────────────────────────────
  // This line printed "ADVERTISING..." unconditionally, so when the post-boot
  // grace lapsed the panel went on claiming the lock was discoverable — for
  // the entire rest of its life. The redraw was never the problem: the
  // grace-lapse handler already sets screenDirty (:6141) and openBleWindow()
  // sets it again (:1506). The screen repainted faithfully, with text that
  // could not be wrong because it never looked.
  //
  // A lock that says ADVERTISING while dark is worse than one that says
  // nothing: the installer stands there waiting for a phone to find it.
  //
  // Colour carries the state as well as the words (operator's spec): AMBER =
  // you can pair right now, WHITE = you cannot, press BOOT. Amber is the
  // "notice this, it is live and time-limited" colour used by the BLE badge
  // on the operational screen, so the two screens agree.
  if (bleClientConnected) {
    gfx->print("APP CONNECTED");
  } else if (bleAdvertisingAllowed()) {
    gfx->print("ADVERTISING...");
  } else {
    gfx->setTextColor(C_WHITE);
    gfx->print("NOT ADVERTISING");
    gfx->setTextColor(C_AMBER); // restore for the rest of this screen
  }
  gfx->setTextSize(3);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(52, 70);
  gfx->println("OZLOCK");
  gfx->setTextSize(2); // bumped from 1 (2026-07-27) — too small to read
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(172, 86); // version badge beside the logo
  gfx->println(FW_DISPLAY_VERSION);
  // FULL device_id, big (operator, 2026-08-12). This used to be ozShortId()
  // here plus a small "device_id: <full>" line underneath — two renderings of
  // the same fact, and the one you actually needed was the unreadable one.
  // Now: one line, the whole id, at the size you can read from arm's length.
  //
  // Fits: 16 chars x 6px x size 2 = 192px, from x=15 -> 207 of PANEL_W 320,
  // on BOTH panel variants (doorlock 320x172, doorlock19 320x170).
  //
  // The small line's vertical space is what pays for the nudge below — the
  // green line moved 108 -> 113 -> 121 (+5 then +8, operator, 2026-08-12) and
  // the rows under it stay put. At size 2 the glyphs are ~16px tall, so 121
  // runs to ~137 and clears the mac row at 140 by 3px. That is the floor: any
  // further down needs the mac/status rows moved too.
  gfx->setTextSize(2);
  gfx->setTextColor(C_GREEN);
  gfx->setCursor(15, 121);
  gfx->println(deviceId);
  gfx->setTextColor(C_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(15, 140);
  gfx->print("mac: ");
  gfx->println(macStr);
  // One amber status line instead of two dim instruction lines (operator,
  // 2026-08-12). The screen says what the LOCK is doing; it is not the place
  // to teach the app's menu path or the reset gesture.
  gfx->setCursor(15, 153); // 152 -> 153, operator's eye at the bench
  gfx->setTextColor(C_AMBER);
  gfx->println("WAITING TO BE PAIRED...");
}

void drawJoining() {
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, C_BLUE);
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

// OPERATIONAL dashboard layout — four bands, stacked with NO overlap: status
// bar, DOOR STATUS bar, hex-command readout, then the keypad fills the rest.
// Defined here, before first use (drawOperational() is the first consumer;
// keyAt()/drawKeypad() further down reuse the same macros).
//
// 2026-08-07 (operator-caught bug): the door-status block and the hex row
// both started at the same y (STATUS_H), so each later draw call painted
// over the previous one's bottom pixels — the LOCKED/UNLOCKED block's
// bottom got erased by the hex row's black background, and the keypad's top
// row did the same to both. The macro that decided where the keypad starts
// only ever accounted for STATUS_H + the hex row's height, never the status
// block's own height, which is what actually caused it.
//
// Also 2026-08-07 (operator): the LOCKED/UNLOCKED text block was too tall —
// replaced with a thin full-width color bar (green/red, no text) instead,
// which both fixes the overlap risk and gives the keypad more room.
// 2026-08-07 (operator, third pass after seeing it on real hardware): the
// status text, color bar, and BLE countdown are now ALL ONE row — "OZLOCK
// V1.16 THREAD CHILD" (left) + a 60px color bar + "BLE 30s" (far right).
// The earlier two-row version (text alone, then bar+BLE on its own row)
// still wasted a whole row height for a full-width amber fill with mostly
// blank space in it. drawStatusLine() draws only this one row and is called
// directly by the 3s periodic tick (bypassing screenDirty/drawOperational's
// full fillScreen() entirely) — operator: "REMOVE THE 3S screen update..it
// causes bad flicker..can u update a word rather than a whole screen." A
// full redraw still happens on real state changes (door open/close, BLE
// window open/close, a tap) via the normal screenDirty path; only the
// ambient "keep the clock/IP/role current" tick is now a targeted redraw.
#define STATUS_H 20
#define HEX_TOP STATUS_H
// 2026-08-07 (operator): hex readout is now ONE bigger line (setTextSize 2)
// instead of two lines of small text — trades max visible bytes for
// readability, same truncate-with-"..." behavior either way.
#define HEX_H 20
// 2026-08-07 (operator): keypad reduced from 4x3 to 4x2 (see KP_KEYS below —
// these taps only exercise touch zones, no real PIN backend, so the full
// digit set was never needed).
#define KP_TOP (HEX_TOP + HEX_H)
#define KEY_W (PANEL_W / 4)
#define KEY_H ((PANEL_H - KP_TOP) / 2) // 2 rows, not 3 — see KP_KEYS

// OPERATIONAL dashboard (operator spec): DOOR STATUS is the hero element,
// plus door name, IP, network status. Border colour = health summary:
// GREEN = net + MCU link up · AMBER = one leg down · RED = both down.
void drawOperational() {
  panelGen++; // full clear below invalidates every gated row
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
  // MCU health dropped from the border (2026-08-07) — the wire itself is
  // still real (LockSim can still be connected), but most units won't have
  // it wired most of the time now that the hex readout is the primary proof
  // a command was actually issued. Including MCU-down in the border would
  // make an otherwise-healthy, LockSim-less lock look permanently degraded.
  uint16_t border = netUp ? C_GREEN : C_RED;
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, border);
  gfx->drawRect(1, 1, PANEL_W - 2, PANEL_H - 2, border);

  // Status line, hex readout, keypad — see each function's own header for
  // why they're split out (drawStatusLine() specifically needs to be
  // callable on its own, for the flicker-free periodic tick in loop()).
  drawStatusLine();
  drawHexReadout();
  // Drawn keypad fills the rest of the panel — replaces the old hit-test-
  // only invisible grid (2026-08-07). Same gestures as before: any tap
  // re-arms the BLE window, '*' then '5' factory-resets. Net/IP/Owner detail
  // dropped from the LCD in this pass (still in [MON] serial + the app) —
  // this panel is too small to show a keypad and a full text dashboard at
  // once; the keypad won the trade because it's what XF-55's onboarding
  // copy already promises ("chạm/nhấn nút trên khoá").
  drawKeypad();
}

// ONE row: "OZLOCK V1.16 THREAD CHILD" (left) + a 60px color bar + "BLE 30s"
// (far right) — see the STATUS_H block's own comment above for why this is
// a separate function (called directly by the 3s periodic tick, no full
// fillScreen(), no flicker) rather than inlined into drawOperational().
// Deliberately clears a 2px-inset region (not the full row width from y=0)
// so it never touches the border's own outline pixels (y=0,1) — meaning it
// never needs to redraw the border either, which a naive "clear the whole
// row" version would have erased.
// Line 1, everything on it (operator, 2026-08-07 final layout pass):
// "OZLOCK V1.19 THREAD CHILD LOCKNAME10 BLE 20s" — version, transport,
// role/IP, lock name (10 chars), BLE countdown far right. The LOCKED/
// UNLOCKED color bar moved OFF this line entirely — it's on line 2 now,
// paired with the hex readout (see drawHexReadout()), since this line was
// already full without it.
/*
 * The BLE countdown badge, on its own. It is the ONLY thing in the status row
 * that changes every second, and redrawing the whole row for it is what made
 * the panel strobe during the pairing window (operator, 2026-08-11) — the
 * moment a user is most likely to be looking at it.
 */
void drawBleBadge() {
  if (!bleWindowOpen()) return;
  gfx->fillRect(256, 2, PANEL_W - 256 - 2, STATUS_H - 4, C_AMBER);
  gfx->setTextSize(1);
  gfx->setCursor(260, 6);
  gfx->setTextColor(C_WHITE, C_AMBER);
  gfx->printf("BLE %lus", (bleWindowUntil - millis()) / 1000);
}

void drawStatusLine() {
  ot_device_role_t liveRole = isThread() ? OpenThread::otGetDeviceRole() : OT_ROLE_DISABLED;
  bool threadAttached = (liveRole == OT_ROLE_CHILD || liveRole == OT_ROLE_ROUTER ||
                         liveRole == OT_ROLE_LEADER);
  bool netUp = isThread() ? threadAttached
                          : ((WiFi.status() == WL_CONNECTED) && mqtt.connected());
  uint16_t border = netUp ? C_GREEN : C_RED;

  // ── DON'T REPAINT WHAT HASN'T CHANGED (operator, 2026-08-11) ────────────
  //
  // This row was cleared to black and rebuilt every 3 s unconditionally, which
  // is a full-width black flash three times a minute for a line whose contents
  // are usually identical. Everything below is a pure function of this state,
  // so if the state matches the last paint there is nothing to draw.
  //
  // The BLE countdown is deliberately NOT in this signature — it changes every
  // second and would defeat the whole gate. It has its own badge redraw.
  static uint32_t lastSig = 0;
  static bool haveSig = false;
  static uint32_t seenGen = (uint32_t)-1;
  if (seenGen != panelGen) { seenGen = panelGen; haveSig = false; } // screen was cleared
  uint32_t sig = (uint32_t)netUp | ((uint32_t)liveRole << 1) |
                 ((uint32_t)provisioned << 5) | ((uint32_t)mcuLinkUp() << 6) |
                 ((uint32_t)bleWindowOpen() << 7) | ((uint32_t)isThread() << 8);
  {
    // Fold the variable-length text in too, so an IP or name change still
    // repaints. Cheap additive hash — collisions only cost a missed refresh
    // of a line that is redrawn on every real event anyway.
    String v = (isThread() ? String("") : WiFi.localIP().toString()) +
               (cfgName.length() ? cfgName : deviceId) + cfgRoomNo;
    for (size_t i = 0; i < v.length(); i++) sig = sig * 31u + (uint8_t)v[i];
  }
  if (haveSig && sig == lastSig) {
    drawBleBadge(); // the one part that legitimately ticks
    return;
  }
  lastSig = sig;
  haveSig = true;

  gfx->fillRect(2, 2, PANEL_W - 4, STATUS_H - 2, C_BLACK);

  // ── MCU link dot (operator, 2026-08-10) ──────────────────────────────────
  // The MCU is the half of this product we do not control, and its state was
  // visible only in the [MON] serial line — so a bench board with a dead UART
  // looked identical to a healthy one on the panel. It sits in the gap left of
  // the title, vertically centred on the 8px text row.
  //
  // GREEN = frames seen inside MCU_LINK_TIMEOUT_MS. RED = silent.
  // Deliberately NOT amber-for-unknown: there is no unknown state here, the
  // link has either produced a frame recently or it has not.
  gfx->fillCircle(7, 10, 3, mcuLinkUp() ? C_GREEN : C_RED);

  gfx->setTextSize(1);
  gfx->setTextColor(border);
  gfx->setCursor(15, 6);
  gfx->print("OZLOCK ");
  gfx->print(FW_DISPLAY_VERSION);
  gfx->print(" ");
  // An unprovisioned lock has NO transport — cfgTransport merely defaults to
  // "wifi" (:572). Showing "WIFI" on a virgin lock renders a default as a
  // decision, which is how a factory-reset lock looked like a misconfigured one.
  // The app decides the mode, at provision time, by sending network_key or not.
  if (!provisioned) {
    gfx->print("UNSET");
  } else if (isThread()) {
    gfx->print("THREAD ");
    gfx->print(threadAttached ? (liveRole == OT_ROLE_CHILD    ? "CHILD"
                                  : liveRole == OT_ROLE_ROUTER ? "ROUTER"
                                                                : "LEADER")
                               : "NC");
  } else {
    gfx->print(isLocalMode() ? "HOTEL IP:" : "WIFI IP:");
    if (WiFi.status() == WL_CONNECTED) gfx->print(WiFi.localIP().toString());
    else gfx->print(" NO NETWORK");
  }
  if (isLocalMode() && cfgRoomNo.length()) { // room lives in the header now
    gfx->print(" P.");
    gfx->print(cfgRoomNo);
  }
  // The lock's NAME used to sit here; it moved to line 2's right edge
  // (operator, 2026-08-11) where it sits beside the clock. Line 1 is now
  // purely transport/health, which is also what makes its change-gate
  // effective — the name was the most frequently varying thing on it.

  // BLE countdown / reset hint, far right (operator spec — moved here from
  // its own row, which no longer exists).
  // XF-52 (R): while the maintenance window is open, say so instead of showing
  // the reset hint. Pressing a button and getting no visible response is how a
  // user concludes it didn't work and holds it longer — which is the factory
  // reset. The countdown is the whole point: it tells them how long they have.
  if (bleWindowOpen()) {
    // Filled amber badge + white text (operator, 2026-08-07: "BLE 20s in
    // amber background white text") — same "notice this" reasoning as the
    // earlier filled-badge pass, reapplied after this line got rebuilt.
    gfx->fillRect(256, 2, PANEL_W - 256 - 2, STATUS_H - 4, C_AMBER);
    gfx->setCursor(260, 6);
    gfx->setTextColor(C_WHITE);
    gfx->printf("BLE %lus", (bleWindowUntil - millis()) / 1000);
  } else {
    gfx->setCursor(258, 6);
    gfx->setTextColor(C_DIM);
    gfx->print("reset:*5");
  }
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
  cfgBrokerUser = prefs.getString("buser", "");   // written at enrollment since
  cfgBrokerSecret = prefs.getString("bsecret", ""); // day one; read since 1.57
  cfgServerIp = prefs.getString("sip", "");
  cfgServerPort = prefs.getUShort("sport", 4200);
  cfgSiteId = prefs.getString("site", "lab");
  cfgName = prefs.getString("name", "");
  cfgHeartbeatS = clampHeartbeatS(prefs.getUInt("hb", 300));
  {
    // A profile the MCU told us about last boot. Restored so a lock is on the
    // right map from the first millisecond, not from whenever 0x01 is answered.
    String pf = prefs.getString("prof", "");
    if (pf.length()) ozProfileSelect(pf.c_str());
    // Last known identity. Reported immediately so `info` and the heartbeat
    // are never blank for a lock we have already identified; the 0x01 ask
    // still runs and overwrites this if the MCU now says something else.
    cfgMcuPid = prefs.getString("mpid", "");
    cfgMcuVer = prefs.getString("mver", "");
    if (cfgMcuPid.length())
      Serial.printf("[PID] restored '%s' (mcu_fw '%s') — will re-confirm\n",
                    cfgMcuPid.c_str(), cfgMcuVer.c_str());
  }
  g_bellObserved = prefs.getBool("bell", false);
  cfgThreadSed = prefs.getBool("sed", false);
  cfgThreadPollS = clampThreadPollS(prefs.getUInt("poll", 5));
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
  prefs.putBool("sed", cfgThreadSed);
  prefs.putUInt("poll", cfgThreadPollS);
  prefs.putString("mode", cfgMode);
  prefs.putString("room", cfgRoomNo);
  prefs.putString("mtoken", cfgMacToken);
  prefs.putBool("wksim", wakeSim);
  prefs.putString("xport", cfgTransport);
  prefs.end();
}

void factoryReset() {
  Serial.println("[RESET] factory reset — wiping NVS + txlog");
  // M2: prefs.clear() wipes the "blelock" namespace, which holds the ceremony
  // keypair (xpriv/xpub), the whole bond table (M3 "bondtab" — owner AND every
  // member) and the M3 nonce replay cache. So a factory reset is the ONLY way
  // to clear ownership (CONTRACT.md), and it necessarily
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
// mechanism as bridge32.ino/threadcomm.ino. This is also the ONLY reset path
// on a screen-less board, so it must be a real, per-board-verified pin, not
// an inherited toolchain default — see the 2026-08-07 note below.
//
// 2026-08-07: USER_BUTTON used to silently fall back to the Arduino ESP32-C6
// core's generic BOOT_PIN (GPIO9, esp32-hal.h) if a board didn't define it.
// Neither board here ever independently verified that pin has a real button
// behind it — not in HARDWARE.md for the 1.47", not in Waveshare's own repo
// for the 1.9" — it just happened to compile. Operator caught this after the
// 1.9" board's BOOT-hold reset didn't seem to work. Now REQUIRED per board
// (#error if missing) instead of silently inherited, same discipline as
// every other pin in the "must define before #include" contract at the top
// of this file — a future board that forgets it fails to compile instead of
// silently polling the wrong GPIO.
#ifndef USER_BUTTON
#error "USER_BUTTON not defined — each board's .ino must #define it (verified against real hardware) before #include-ing ozdoorlock_core.h. Do not fall back to the toolchain's generic BOOT_PIN."
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
// 30 s, not 60 (operator, 2026-08-16). The window is the lock's only exposed
// surface, so its length is the exposure. Halving it halves both the time a
// passive scanner can see the lock and the advertising energy per open — and
// 30 s is still comfortably longer than a pair-and-connect ceremony.
// 60 s, doubled from 30 s (operator, 2026-08-18, from a real observation on
// the bench):
//
//   "BLE turn on it needs 15s for app to see. by the time he get ready and
//    open door via ble.. the window closed"
//
// That 15 s is the advertising-to-discovery latency on a real phone, and it is
// pure overhead — the user cannot act during it. A 30 s window therefore gave
// someone roughly 15 s to unlock a phone, open the app, find the lock and tap.
// It was closing underneath people at the door, which is a failure of the one
// gesture a production lock has (BOOT is on the INSIDE).
//
// 🔴 DO NOT "OPTIMISE" THIS BACK DOWN without re-measuring discovery latency
// first. The number that matters is not how long the window is, it is how long
// the window is MINUS how long discovery takes — and only the second half is
// measurable from the bench.
#define BLE_WINDOW_MS 60000UL
#define BUTTON_DEBOUNCE_MS 60UL
// bleWindowUntil is declared with the other BLE globals near the top — see the
// note there about drawStatus() needing it before this point.

void startBle(); // defined with the GATT setup, further down
static void ozRefreshInfoChar(); // INFO is rebuilt on change, not snapshotted

bool bleWindowOpen() { return bleWindowUntil && (long)(millis() - bleWindowUntil) < 0; }

// No default argument on `gesture`: the IDE auto-prototypes this file, and a
// generated prototype that repeats a default argument is a hard compile error.
void openBleWindow(const char *gesture) {
  const bool wasOpen = bleWindowOpen();
  bleWindowUntil = millis() + BLE_WINDOW_MS;
  if (bleServer == nullptr) startBle(); // first open: build the GATT server
  else if (!bleClientConnected)
    // Full re-arm, not a bare startAdvertising(). If a previous link left
    // adv_type at SCAN_IND, a bare start leaves the window discoverable but
    // NON-CONNECTABLE — the "one BLE connection per boot" bug. This is the
    // user-facing path (every keypad touch), so it has to be the safe one.
    bleRearmAdvertising(true, "window opened");
  // A tap DURING a live connection must not restart advertising — the link is
  // already up and we are deliberately SCAN_IND while busy (see bleSetBusy).
  // It still extends the deadline, which is the point: a long enrolment must
  // not have its window expire underneath it.
  if (!wasOpen) {
    Serial.printf("[BLE] window OPEN %lus (%s)\n", BLE_WINDOW_MS / 1000, gesture);
    ozThreadApplyPoll(true); // C9 §5 — someone is at the door; poll hard
  }
  screenDirty = true;
}

void closeBleWindow(const char *why) {
  // Start the cooldown from when the window actually ENDED, not when it opened
  // — otherwise a 60 s window inside a 120 s cooldown leaves only 60 s of real
  // quiet.
  if (g_bellOpenedWindow) { g_bellWindowEndedAt = millis(); g_bellOpenedWindow = false; }
  bleWindowUntil = 0;
  BLEDevice::stopAdvertising();
  Serial.printf("[BLE] window closed (%s)\n", why);
  ozThreadApplyPoll(false); // back to the configured interval
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
      // XF-114 §13.4 / nexus-14 §2 Gap B — the MECHANICAL reset, the one path
      // with no server anywhere in it. Someone standing at the door wipes a
      // lock and, until 1.95, the fleet found out by noticing it had gone
      // quiet. This is also the case that makes the key in NEXUS wrong with
      // nobody to notice (a wipe mints a new keypair), so the announcement
      // matters well beyond this ticket.
      ozPublishResetOutcome("factory_reset", true);
      factoryReset(); // does not return
    } else if (held > 800 && (held / 500) % 2 == 0) {
      Serial.printf("[RESET] holding BOOT... %lus/5s\n", held / 1000);
    }
  } else if (!down && buttonWasDown) {
    // Released. A short press opens the window; a long one already wiped the
    // device and never got here.
    unsigned long held = millis() - buttonHeldSince;
    if (held >= BUTTON_DEBOUNCE_MS && held < FACTORY_RESET_HOLD_MS)
      openBleWindow("short BOOT press");
  }
  buttonWasDown = down;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tuya MCU wire (Serial1 → LockSim Mode B, if connected) — STILL a real wire
// write (2026-08-07 operator correction: this stays, LockSim being connected
// or not is irrelevant either way). ADDITIONALLY now puts the same hex on
// screen (drawHexReadout()), so the command is visible with or without a
// wire connected. ⚠ RAW BYTES over the wire, never spaced-hex — LockSim's
// extractFrames() scans for the contiguous 0x55 0xAA header.
// ─────────────────────────────────────────────────────────────────────────────
String lastMcuHex = ""; // last DP frame's hex bytes — read by drawHexReadout()


// ─────────────────────────────────────────────────────────────────────────────
// 🔴 1.61 — WAIT FOR THE MCU BEFORE CLAIMING SUCCESS (ozkey-28 §4)
//
// Until now the module answered UNLOCK_OK the instant it had written bytes to
// the UART:
//
//     forwardFrameToMcu(frame, flen);
//     publishLog("granted", ...);
//     notifyStatus("UNLOCK_OK");      // <- nothing in between
//
// No status report awaited, no timeout, no failure path. So "accepted" meant
// *"your envelope authenticated and I put bytes on a wire"* — it could not
// distinguish a stored credential from one written to a disconnected pin, and
// the operator hit exactly that: the app was told a PIN was issued while the
// MCU never received a decodable one. LockSim's own source predicted it: "an
// ESP32 that has no timeout logic at all looks identical to one that does".
//
// The ack is the MCU ECHOING THE DP back. That is the most likely real
// behaviour (a Tuya MCU answers a DP write with a status report for that DP)
// and, importantly, it is the shape we can support without knowing the
// supplier's exact reply format — still blocked on ozkey-27 Q2. When the real
// contract arrives this narrows; it does not have to be redesigned.
//
// NEGATIVE ACK IS DELIBERATELY ABSENT. A rejected credential simply produces no
// echo, and we time out. Inventing an error code the real MCU may not send
// would be fiction of exactly the kind ozkey-27 §2.1 is about — absence of
// confirmation is honestly "we do not know", which is the whole point.
//
// UNLOCK (DP 1) IS EXCLUDED. Its proof is the bolt moving and it is
// latency-critical — doorlock-1.8 spent effort shaving 230 ms off this path.
// Everything else is a credential operation, is not latency-critical, and is
// precisely where proof matters.
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t g_ackWaitDp = 0;   // 0 = not waiting
static bool    g_ackSeen   = false;

#define OZ_MCU_ACK_TIMEOUT_MS 1500UL

/** True if the MCU echoed `dp` within the window. Pumps the UART inline. */
static bool ozAwaitMcuAck(uint8_t dp) {
  g_ackWaitDp = dp;
  g_ackSeen = false;
  const unsigned long t0 = millis();
  while (millis() - t0 < OZ_MCU_ACK_TIMEOUT_MS) {
    tuyaWirePump(); // drives handleMcuFrame(), which sets g_ackSeen
    if (g_ackSeen) break;
    delay(2);
  }
  const bool ok = g_ackSeen;
  g_ackWaitDp = 0;
  Serial.printf("[MCU-ACK] DP %u %s after %lu ms\n", dp,
                ok ? "CONFIRMED" : "NO ANSWER — credential NOT stored",
                millis() - t0);
  return ok;
}

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
  lastMcuHex = hex;
  // screenDirty REMOVED 2026-08-11. It existed to refresh the hex readout, and
  // that readout is gone — this row shows the clock now, refreshed on its own
  // 1 s tick. Leaving it in meant a full-screen redraw on every frame we send
  // to the MCU, which since 1.42 includes a time reply every few seconds. That
  // is what wrecked touch sensitivity. lastMcuHex is kept: it is still the
  // [TUYA->] serial line's source and costs nothing.
}

// ─────────────────────────────────────────────────────────────────────────────
// ozkey-21 T2 — serve time to the lock MCU
//
// The whole service is three functions: refresh our own clock from whatever
// source we have, answer 0x0C/0x1C on demand, and push on a slow timer once
// subscribed. Codec and the monotonic-forward rule are in oztime.h.
// ─────────────────────────────────────────────────────────────────────────────

/*
 * Feed T1's clock from the system clock.
 *
 * TODAY the only real source is SNTP over Wi-Fi, so a WI-FI LOCK GETS TIME AND
 * A THREAD LOCK DOES NOT. That is not an oversight in T2, it is T3's job — the
 * bridge runs NTP and stamps UTC onto every forwarded command plus a slow
 * beacon. Until T3 lands, a Thread lock answers 0x1C honestly with flag 0, and
 * the [TIME] counters below make that visible instead of silent.
 */
/*
 * ozkey-21 §3.4 rule 3 — persist the clock across a reboot.
 *
 * The battery-pull case is covered by re-provisioning (the app hands time back
 * during commissioning). What is NOT covered is a SPONTANEOUS RESET — brownout,
 * watchdog, crash — where the lock comes straight back up with no app present
 * and no provisioning. Without this it would sit clock-less until the next
 * beacon, and every temporary credential on it is unenforceable meanwhile.
 * This board has a documented history of self-resets, so that is not a
 * theoretical branch.
 *
 * WRITE CADENCE: hourly, never per tick — NVS has finite erase endurance and a
 * clock written every second would wear the sector out. An hour-stale restored
 * time is harmless: monotonic-forward means the first beacon corrects it
 * upward, and being an hour behind never un-expires anything, it only delays
 * an expiry that the next sync then applies.
 */
#define OZ_CLOCK_PERSIST_MS 3600000UL

void ozClockPersist(bool force) {
  static unsigned long lastWrite = 0;
  if (!ozClockKnown(ozclock)) return;
  if (!force && lastWrite && millis() - lastWrite < OZ_CLOCK_PERSIST_MS) return;
  lastWrite = millis();
  prefs.begin("blelock", false);
  prefs.putUInt("utclast", ozClockNow(ozclock, millis()));
  prefs.end();
}

// 🔴 1.74 — "I HAVE A CLOCK" AND "MY CLOCK IS RIGHT" ARE DIFFERENT CLAIMS.
//
// ozClockRestore() below loads a snapshot written at most hourly
// (OZ_CLOCK_PERSIST_MS), and after it runs ozClockKnown() returns TRUE. The
// presence beacon only asks for time when the clock is UNKNOWN, so a lock that
// booted with a stale snapshot believed it knew the time and NEVER ASKED — it
// waited for the bridge's next multicast, up to 24 h away (OZ_TIME_BEACON_MS).
//
// The operator hit the mild version of this on the bench ("the time is slower,
// 5-20 min, due to reset"). The severe version is a BATTERY CHANGE: NVS is
// flash, so a unit that sat in a box for two weeks boots, restores a two-week-
// old timestamp, marks the clock known, and never asks. Temporary credential
// expiry is enforced against that clock (see ozBondExpirySweep's rule 2), and a
// lock that thinks it is two weeks ago keeps honouring PINs that expired two
// weeks ago — failing PERMISSIVE, which is the wrong direction.
//
// The old reasoning ("an hour behind only delays an expiry, it never un-expires
// anything") is sound at one hour and stops being sound at two weeks. Same
// class as the membership-expiry hole found 2026-08-12.
//
// So: track PROVENANCE, not just presence. g_clockLive is true only once a real
// source has spoken THIS BOOT.
bool g_clockLive = false; // declared extern up top for txlogAppend/ozEvtPush

// Harvest `utc`/`tz` out of any inbound message, from any transport.
//
// Was Thread-only until 1.74, which left Wi-Fi locks with NO time source at all:
// their only sync was configTime(..., "pool.ntp.org") and this lab blocks UDP
// 123, so NTP never answers ([[ntp-is-not-a-dependable-time-source]]). A Wi-Fi
// lock therefore ran on whatever NVS happened to hold, forever.
//
// The trust argument is the one already written for the Thread path and is
// unchanged: ozClockSet() is monotonic-forward and floors implausible values, so
// a time we already have, or an older one, changes nothing — which is what makes
// it safe to accept from ANY message rather than only an addressed one. A
// command meant for a different lock still carries a perfectly good UTC stamp,
// and refusing to read it would throw away free syncs.
void ozHarvestTime(JsonDocument &doc, const char *from) {
  // Timezone rides with the time. Stored even when the clock value itself is
  // refused (stale/duplicate beacon) — the offset is still current information.
  if (doc["tz"].is<int>()) {
    const int16_t tzNew = (int16_t)(doc["tz"] | 0);
    if (tzNew != cfgTzMin) {
      cfgTzMin = tzNew;
      prefs.begin("blelock", false);
      prefs.putShort("tzmin", cfgTzMin);
      prefs.end();
      Serial.printf("[TIME] timezone from %s: %+d min\n", from, (int)cfgTzMin);
      screenDirty = true;
    }
  }

  const uint32_t utcIn = doc["utc"] | 0UL;
  if (!utcIn) return;
  const bool wasKnown = ozClockKnown(ozclock);
  const bool wasLive = g_clockLive;
  if (!ozClockSet(ozclock, utcIn, millis())) return;

  // ── 1.75 — IS THE SENDER PASSING ON A SYNC, OR ITS OWN GUESS? ────────────
  //
  // bridge32-1.38 persists its clock across a reboot, which closes a real hole:
  // a bridge that rebooted while ozlockserv was unreachable used to leave every
  // Thread lock behind it with NO time source at all, since this beacon is
  // their only supply.
  //
  // But it means the bridge can now beacon a time it RESTORED rather than one
  // the server gave it. If we treated that as a confirmation, every lock
  // downstream would flip to clock=live on the strength of the bridge's guess
  // and stop asking — which is exactly the failure 1.74 exists to prevent,
  // reintroduced one hop upstream where it is harder to see.
  //
  // So the beacon states its provenance and we believe only "server".
  // ABSENT = trusted: a bridge32-1.37 or earlier only ever sent real syncs, so
  // omitting the field must keep meaning what it always meant.
  const char *src = doc["utc_src"] | "server";
  if (strcmp(src, "nvs") == 0) {
    // Applied anyway — a stale clock beats no clock, and ozClockSet()'s
    // monotonic-forward rule means it can only ever move us forward. We simply
    // do not call it a sync, so need_time stays raised and the next real source
    // still gets asked for.
    if (!wasKnown)
      Serial.printf("[TIME] %s passed on a RESTORED clock (utc_src=nvs): %lu — "
                    "applied, still asking for a real sync\n",
                    from, (unsigned long)utcIn);
    return;
  }

  // A real source spoke. This is what stops need_time being asked forever, and
  // it is deliberately set even when the clock was already "known" — that is the
  // whole point: known-from-NVS is exactly the state we are trying to leave.
  g_clockLive = true;
  if (!wasLive) {
    Serial.printf("[TIME] clock CONFIRMED by %s: %lu%s\n", from,
                  (unsigned long)utcIn,
                  wasKnown ? " (replaces the NVS snapshot)" : "");
    ozClockPersist(true); // don't lose it to a reset in the next hour
    screenDirty = true;
  }
  if (!wasKnown) {
    Serial.println("[TIME] temporal DPs are now enforceable");
    // The MCU has been asking and getting "I do not know". Now that we know,
    // tell it immediately rather than waiting for it to ask again — it may back
    // off for hours after repeated unknown answers.
    if (mcuLinkUp()) serveMcuTimePush();
  }
}

void ozClockRestore() {
  prefs.begin("blelock", true);
  const uint32_t saved = prefs.getUInt("utclast", 0);
  prefs.end();
  if (saved < OZ_TIME_FLOOR) return;
  // Goes through ozClockSet() like any other source, so the floor and the
  // monotonic rule apply to our own NVS exactly as they do to the bridge.
  // NOTE: deliberately does NOT set g_clockLive — a snapshot is a starting
  // guess, not a synchronisation. See the block above.
  if (ozClockSet(ozclock, saved, millis()))
    Serial.printf("[TIME] clock restored from NVS: %lu — a GUESS, not a sync; "
                  "asking for the real time until something answers\n",
                  (unsigned long)saved);
}

void ozClockRefreshFromSystem() {
  time_t sys = time(nullptr);
  if ((uint32_t)sys < OZ_TIME_FLOOR) return;
  // Only log transitions; this is called from loop().
  bool wasKnown = ozClockKnown(ozclock);
  if (ozClockSet(ozclock, (uint32_t)sys, millis())) {
    // SNTP answering is a REAL sync, so it confirms the clock (1.74). Rare in
    // this lab — UDP 123 is blocked — but this is the path that works in the
    // field, and it should not be the one source that leaves need_time stuck on.
    g_clockLive = true;
    if (!wasKnown)
      Serial.printf("[TIME] clock acquired: %lu (SNTP)\n", (unsigned long)sys);
  }
}

void serveMcuTimeRequest(bool local) {
  mcuTimeRequests++;
  const uint32_t now = ozClockNow(ozclock, millis());
  const bool have = ozClockKnown(ozclock);
  // 0x1C is GET_LOCAL_TIME and 0x0C is GET_GMT_TIME — so the local request gets
  // the offset applied and the GMT one never does. Until the app supplied a
  // timezone, cfgTzMin was 0 and "local" was silently UTC; that was the honest
  // best we could do, not a design choice.
  //
  // ⚠ ANSWERED — XF-90 §11 (ftpos, 2026-08-11): DP 21/23 `from`/`to` are TRUE
  // UTC EPOCH SECONDS. They verified it in their own source rather than
  // assuming: the admin picks a local wall-clock time, but it reaches the wire
  // via DateTime.millisecondsSinceEpoch, which is always UTC-based.
  //
  // THEREFORE WE SERVE UTC HERE, TIMEZONE OFFSET DELIBERATELY NOT APPLIED —
  // even though 0x1C is nominally GET_LOCAL_TIME. The MCU is comparing against
  // UTC windows, so it must be given a UTC clock; handing it local time would
  // shift every temporary PIN and RFID by the offset (7 hours in Vietnam) and
  // would be undetectable from our side.
  //
  // `cfgTzMin` is for the PANEL ONLY. Do not "fix" this to use it — the naming
  // of the Tuya command is the trap here, not the guide.
  uint8_t out[16];
  size_t n = ozTuyaBuildTimeReply(out, local, have, now, /*tzOffsetMin=*/0);
  tuyaWireSend(out, n);
  if (have) {
    mcuTimeServed++;
    Serial.printf("[TIME] served %s time to DL MCU: %lu (req=%lu served=%lu)\n",
                  local ? "LOCAL" : "GMT", (unsigned long)now,
                  (unsigned long)mcuTimeRequests, (unsigned long)mcuTimeServed);
  } else {
    mcuTimeUnknown++;
    // Not a failure of this code — an honest report that nothing has told US
    // the time either. On a Thread lock this is the expected answer until T3.
    Serial.printf("[TIME] DL MCU asked for %s time and WE DO NOT KNOW IT "
                  "(flag=0, req=%lu unknown=%lu) — temporal DPs unenforceable\n",
                  local ? "LOCAL" : "GMT",
                  (unsigned long)mcuTimeRequests, (unsigned long)mcuTimeUnknown);
  }
  // NO screenDirty HERE. The MCU polls for time every few seconds, and a full
  // redraw per poll is what destroyed touch sensitivity in 1.44 (operator:
  // "tap x10 to see screen response"). Before 1.42 the lock never transmitted
  // at all, so tuyaWireSend()'s own screenDirty never fired on this path —
  // answering the MCU turned a rare event into a periodic one.
  // The 1 s clock tick in loop() already keeps this row current.
}

void serveMcuTimePush() {
  const uint32_t now = ozClockNow(ozclock, millis());
  const bool have = ozClockKnown(ozclock);
  uint8_t out[20];
  size_t n = ozTuyaBuildTimePush(out, /*local=*/false, have, now);
  tuyaWireSend(out, n);
  lastTimePushAt = millis();
  if (have) mcuTimeServed++; else mcuTimeUnknown++;
  Serial.printf("[TIME] push -> DL MCU: %s\n",
                have ? "time sent" : "flag=0, we do not know the time");
}

// Short human line for the console + dashboard ("what did the MCU say?")
// ── INBOUND DP frames: the MCU reports with 0x07, not 0x06 (1.92) ───────────
//
// The Tuya "general" variant is a TWO-command-word protocol, and the supplier's
// own instruction table (通讯协议(产品功能部分)指令收发表) is explicit:
//
//     命令字 0x06   模块发送   MODULE issues to the MCU
//     命令字 0x07   MCU上报    MCU reports to the module
//
// We had 0x06 hardcoded in BOTH directions, so every DP report from a real lock
// — doorbell, access events, battery alarm, credential reports — would have
// been dropped as an unparsed "cmd 0x7". The outbound path (ozTuyaFrameOk,
// forwardFrameToMcu) is correct at 0x06 and is deliberately NOT changed.
//
// 🔴 WHY THIS SURVIVED SO LONG: locksim/lib/tuya.ts defines DP_REPORT = 0x06.
// LockSim was written to match this firmware, so both halves of the bench
// agreed with each other and disagreed with the supplier — the bench could not
// see the bug because the bench WAS the bug. Structurally identical to the
// DP 1 fiction (XF-110): see ozkey-39 §1.1.
//
// 0x06 stays accepted inbound until LockSim is flipped to 0x07 — cutting the
// old path before the new one is live just breaks the bench, the same
// discipline the CTL plaintext path already documents.
static inline bool ozIsInboundDp(const uint8_t *f, size_t n) {
  return n >= 11 && (f[3] == 0x07 || f[3] == 0x06);
}

String describeDpid(const uint8_t *f, size_t n) {
  if (n >= 4 && f[3] == 0x00) return String("MCU heartbeat");
  if (!ozIsInboundDp(f, n)) return String("cmd 0x") + String(f[3], HEX);
  uint8_t dpid = f[6], type = f[7];
  uint16_t vlen = ((uint16_t)f[8] << 8) | f[9];
  const uint8_t *v = f + 10;
  if (dpid == 8 && type == 0x04 && vlen >= 1) {
    const char *r = v[0] == 0 ? "SUCCESS" : v[0] == 1 ? "DENIED" : v[0] == 2 ? "EXPIRED" : "?";
    return String("ACCESS_RESULT ") + r;
  }
  if (dpid == 1) return String("unlock channel report");
  if (dpid == 5) return String("battery alarm");
  // M4 verbs. Named here so a bench capture reads as English rather than "DP
  // 101 type 0 len 32" — the whole point of the LockSim test is being able to
  // see at a glance that these never crossed the wire.
  if (dpid == 101) return String("bond_revoke (in-lock, never forwarded)");
  if (dpid == 102) return String("invite_cancel (in-lock, never forwarded)");
  if (dpid == 103) return String("list_bonds (in-lock, never forwarded)");
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

  // 1.61 — is this the echo we are blocked on? Checked BEFORE any other
  // classification, and deliberately without a direction test: an inbound DP 21
  // is "down"-only in the profile and would be refused for forwarding, which is
  // correct — but as an ACKNOWLEDGEMENT of the write we just made it is exactly
  // what we asked for. Setting the flag does not consume the frame; it falls
  // through to normal handling below.
  if (g_ackWaitDp && n >= 7 && (f[3] == 0x06 || f[3] == 0x07) && n >= 11 &&
      f[6] == g_ackWaitDp) {
    g_ackSeen = true;
  }

  // 0x08 census — record every DP the MCU has ever reported, whether or not we
  // asked for it. Cheap (a 32-byte bitmap) and it is the only evidence we have
  // of what this hardware ACTUALLY implements, as opposed to what its PID
  // claims or what our build assumed. See ozAskMcuDpList().
  if (ozIsInboundDp(f, n)) ozNoteDpSeen(f[6]);

  // 🔴 A CENSUS REPLY IS STATE, NOT AN EVENT (found on the bench 2026-08-20).
  //
  // Tuya's 0x08 makes the MCU report the CURRENT VALUE of every DP it has. Those
  // arrive as ordinary 0x07 frames — indistinguishable, byte for byte, from a
  // real event. On the first live run firmware dutifully treated them as
  // things that had just happened: DP 53 latched `has_doorbell` and OPENED THE
  // BLE PAIRING WINDOW, and DP 63/64/76 would have been published as fresh
  // access events, writing phantom unlocks into the audit log.
  //
  // Asking a lock what it can do must never make it behave as though all of it
  // just happened. In production this would open the radio on every MCU
  // reconnect — a security-relevant side effect of a diagnostic.
  //
  // So: during the census window we COUNT the DP and drop the frame. The
  // window is short and self-limiting, and anything genuinely urgent will be
  // re-reported by the MCU when it actually occurs.
  if (g_dpListAsked && !g_dpListReported && ozIsInboundDp(f, n)) {
    // The window ends on SILENCE, not on a fixed timer from the query. Tuya
    // lets the MCU answer "at one time or several times" and says nothing
    // about pacing; LockSim spreads 34 DPs over ~7 s. A fixed 3 s window
    // closed mid-census and the remainder were processed as live events —
    // exactly the bug this block exists to prevent, just later. Each reply
    // pushes the deadline out, so the census lasts as long as the MCU keeps
    // talking and no longer.
    g_dpListAskedAt = millis();
    Serial.printf("[DPQ] census: DP %u present (state, not an event)\n",
                  (unsigned)f[6]);
    return;
  }

  mcuRxFrames++;
  lastMcuFrameAt = millis();
  lastActivityAt = millis();
  lastMcuSummary = describeDpid(f, n);
  Serial.printf("[TUYA<-] %s (%u bytes)\n", lastMcuSummary.c_str(), (unsigned)n);
  screenDirty = true;

  if (n >= 4 && f[3] == 0x00) return; // MCU heartbeat = link-alive only

  // ── 0x01 — the MCU's answer to "what are you?" ───────────────────────────
  //
  // {"p":"<tuya pid>","v":"<mcu fw>"}. The PID selects our DP profile, so the
  // lock discovers its own map instead of booting on whatever default was
  // compiled in. See ozAskMcuProductInfo() for why that matters.
  if (n >= 7 && f[3] == 0x01) {
    const uint16_t vlen = ((uint16_t)f[4] << 8) | f[5];
    if (vlen == 0 || (size_t)vlen + 7 > n) {
      Serial.println("[PID] 0x01 reply malformed — ignored");
      return;
    }
    String body;
    body.reserve(vlen + 1);
    for (uint16_t i = 0; i < vlen; i++) body += (char)f[6 + i];

    JsonDocument pd;
    if (deserializeJson(pd, body) != DeserializationError::Ok) {
      Serial.printf("[PID] 0x01 reply is not JSON: %s\n", body.c_str());
      return;
    }
    const String newPid = String((const char *)(pd["p"] | ""));

    // ── 🔴 ONE IDENTITY PER BOOT (2026-08-20) ───────────────────────────────
    //
    // This handler accepts ANY inbound 0x01, solicited or not, because a real
    // MCU may volunteer its product info. That is fine the FIRST time. It is
    // not fine on the second, because it silently rewrites this lock's entire
    // DP map mid-session.
    //
    // Observed on the bench: firmware went ozsim-fullfeature -> tuya-ds013-t3
    // -> back to ozsim-fullfeature while the simulator's UI never moved, so the
    // lock was interpreting every DP under a map for a product that was not
    // attached, and nothing anywhere reported the disagreement. The operator
    // then issued a PIN that vanished without an error at either end
    // (ozkey-42 §2.4, XF-118).
    //
    // A lock is ONE product. If the MCU's answer changes, either the hardware
    // was swapped — which needs a reboot anyway — or something is lying. Both
    // are better served by refusing and saying so than by quietly adopting it:
    // an inconsistent identity must be LOUD, because every DP decision after it
    // depends on which answer we believed.
    if (g_pidLatched && newPid != cfgMcuPid) {
      Serial.printf("[PID] 🔴 CONFLICT — MCU now says '%s' but this boot "
                    "latched '%s'. IGNORING the new answer and staying on "
                    "profile '%s'. Reboot the lock to re-identify.\n",
                    newPid.c_str(), cfgMcuPid.c_str(), ozProfileId());
      return;
    }

    cfgMcuPid = newPid;
    cfgMcuVer = String((const char *)(pd["v"] | ""));
    g_pidLatched = true;
    g_pidAsks = OZ_PID_MAX_ASKS; // answered — stop asking
    Serial.printf("[PID] MCU reports pid='%s' mcu_fw='%s'\n",
                  cfgMcuPid.c_str(), cfgMcuVer.c_str());

    const OzProfile *p = ozProfileByTuyaPid(cfgMcuPid.c_str());
    if (!p) {
      // ── UNKNOWN PRODUCT — REFUSE, DO NOT GUESS (revised 2026-08-20) ───────
      //
      // 🔴 THIS USED TO FALL BACK TO `tuya-generic-lock`, AND THAT WAS WRONG.
      //
      // The reasoning was "generic = real DPs, better than our invented map".
      // It does not survive contact with Tuya's own documentation: DP numbers
      // are assigned PER PRODUCT CATEGORY at PID creation, so there is no
      // generic lock DP map. The same number means different things:
      //
      //   DP 76  = unlock_ble   on Luona DS013-T3   -> opens the door
      //   DP 76  = fill_light   on Tuya Wi-Fi Lock Pro -> turns on a lamp
      //   DP 16  = bulk_password_add on Luona; duress_alarm on Wi-Fi Lock Pro;
      //            sound level of the chime on a Residential Lock
      //
      // So applying ANY map to a lock we cannot identify risks writing a
      // credential into a chime volume or unlocking a door by adjusting a
      // light. That is the DP 21 -> navigation_volume bug we fixed this
      // morning, one level up and with worse consequences.
      //
      // An unidentified lock is precisely the lock whose DP map we must not
      // improvise. Keep whatever this build was pinned to, say so loudly, and
      // let the verb resolver refuse — refusing is a correct answer.
      Serial.printf("[PID] unknown product '%s' — KEEPING '%s'. There is NO "
                    "generic Tuya DP map to fall back on: DP numbers are "
                    "per-product-category. Add a profile for this product "
                    "before trusting this lock's DP map.\n",
                    cfgMcuPid.c_str(), ozProfileId());
      g_profileMismatch = true; // we are running a map this hardware may not share
    } else if (strcmp(p->id, ozProfileId()) == 0) {
      Serial.printf("[PID] profile '%s' confirmed by the MCU — build and "
                    "hardware agree\n", p->id);
    } else {
      // ── 🔴 CONFIRM, NEVER ADOPT (operator, 2026-08-20) ────────────────────
      //
      // This used to call ozProfileSelect(p->id) and persist it, so the MCU
      // could rewrite this lock's entire DP map at runtime. The DP map is now
      // a BUILD-TIME decision (see OZ_PROFILE_BUILD in ozprofile.h) and this
      // is only a cross-check.
      //
      // A mismatch is a REAL and serious condition — this firmware was built
      // for one product and is installed against another — so it is reported
      // as loudly as we can and changes nothing. Adopting would "fix" the
      // symptom by making the lock run a map its build was never tested
      // against, which is how the bench spent 2026-08-20 chasing a PIN that
      // silently vanished (ozkey-42 §2.4).
      //
      // The fix for a genuine mismatch is to rebuild with the right
      // OZ_PROFILE_BUILD, not to let the far end of a UART decide.
      Serial.printf("[PID] 🔴 MISMATCH — MCU reports '%s' (profile '%s') but "
                    "this build is pinned to '%s'. KEEPING '%s'. Rebuild with "
                    "PROFILE=%s if this hardware is correct.\n",
                    cfgMcuPid.c_str(), p->id, ozProfileId(), ozProfileId(),
                    p->id);
      g_profileMismatch = true; // surfaced on the heartbeat — see below
    }
    // Persist the PID ITSELF, not just the profile it selected. Without this
    // the lock boots on the right DP map while reporting no identity at all —
    // and an app asking "what lock is this" gets `unknown` for a lock we had
    // already identified, until the MCU happens to answer again. Overwritten
    // whenever the MCU reports something different, so moving the co-processor
    // PCB to another lock re-identifies rather than lying.
    prefs.begin("blelock", false);
    prefs.putString("mpid", cfgMcuPid);
    prefs.putString("mver", cfgMcuVer);
    prefs.end();
    ozRefreshInfoChar(); // pid / has_doorbell / profile all just changed
    return;
  }

  // ── ozkey-22 R1 — 0x34, the MULTIPLEXED extended command ──────────────────
  //
  // 0x34 is not one command. Its first payload byte is a sub-command:
  //   0x01 subscribe to time push    0x02 time push    0x0A factory reset
  // Dispatch on it properly; assuming "0x34 means time" is a bug waiting for
  // the first reset frame to arrive.
  if (n >= 7 && f[3] == 0x34) {
    const uint8_t sub = f[6];
    if (sub == 0x0A) {
      // THE PHYSICAL RESET BUTTON ON THE LOCK BODY.
      //
      // That button is wired to the DL MCU, not to us. Until now we never
      // heard about it at all: the lock mechanism would reset and this chip
      // would carry on owned, bonded and on the same mesh — a half-reset lock
      // that looks fine to the app and belongs to nobody.
      //
      // Tuya: "The MCU can locally reset the module to factory settings
      // through this command." Reply is 0x34 0x0A with data[1] 0=ok, 1=fail.
      Serial.println("[RESET] DL MCU signalled FACTORY RESET (0x34 0x0A)");

      // ⚠ ACK FIRST. factoryReset() ends in otInstanceFactoryReset(), which is
      // a PLATFORM RESET and NEVER RETURNS (see factoryReset()'s own 2026-08-02
      // ordering note). A success byte sent after it is a byte never sent, and
      // the DL MCU would sit waiting for an answer that cannot arrive.
      //
      // Answering before acting means we are promising rather than reporting.
      // That is the correct trade here: the DL MCU needs to know its message
      // was understood, and there is no post-reset state left in which to tell
      // it anything. If the wipe somehow fails, the board reboots and comes up
      // still owned — visible on the panel and to the app.
      const uint8_t ack[] = {0x55, 0xAA, 0x00, 0x34, 0x00, 0x02, 0x0A, 0x00, 0x00};
      uint8_t frame[sizeof(ack)];
      memcpy(frame, ack, sizeof(ack));
      uint8_t sum = 0;
      for (size_t i = 0; i + 1 < sizeof(frame); i++) sum += frame[i];
      frame[sizeof(frame) - 1] = sum;
      tuyaWireSend(frame, sizeof(frame));

      // NOTE ON SCOPE: this wipes OUR half only — bonds, keypair, txlog, mesh.
      // The DL MCU's own credentials (PINs/RFID/fingerprints) are ITS to clear,
      // locally, as part of handling its own button; that is ozkey-22 §2.1
      // row 1 and §6 Q0, still unconfirmed with the manufacturer. We must not
      // pretend to have done it.
      //
      // XF-114 §13.4 — tell the network as well. NOBODY ASKED FOR THIS ONE: it
      // is the lock body's own reset button, so there is no pending request and
      // no msg_id. Without this the server's first hint that a deployed lock
      // wiped itself is that it stops answering, which is indistinguishable
      // from a flat battery.
      ozPublishResetOutcome("factory_reset", true);
      factoryReset(); // never returns
      return;
    }
    // ozkey-21 T2 — 0x34 sub 0x01: the MCU asks to be PUSHED time from now on.
    // Answer immediately as well as registering: the MCU asked because it does
    // not know the time, and making it wait a full push interval for the first
    // answer is the same defect in slower motion.
    if (sub == OZ_TUYA_SUB_SUBSCRIBE) {
      mcuWantsTimePush = true;
      Serial.println("[TIME] DL MCU subscribed to time push (0x34 sub 0x01)");
      serveMcuTimePush();
      return;
    }
    Serial.printf("[TUYA<-] 0x34 sub 0x%02X — not handled\n", sub);
    return;
  }

  // ── ozkey-21 T2 — 0x0C / 0x1C: the MCU asks us what time it is ────────────
  //
  // Until now this fell through to "UNCLASSIFIED" and we said nothing at all.
  // That silence is the ozkey-21 §2.3 defect: the MCU checks DP 21/23 windows
  // against a clock nobody ever set, so temporary PINs and RFIDs never expire.
  //
  // We answer even when we have no time, with the success flag clear. A module
  // that says "I do not know" is diagnosable; a module that says nothing is
  // indistinguishable from a broken wire, which is exactly the ambiguity that
  // let this defect sit unnoticed.
  if (n >= 7 && (f[3] == OZ_TUYA_GET_GMT_TIME || f[3] == OZ_TUYA_GET_LOCAL_TIME)) {
    const bool local = (f[3] == OZ_TUYA_GET_LOCAL_TIME);
    serveMcuTimeRequest(local);
    return;
  }

  if (ozIsInboundDp(f, n)) {
    uint8_t dpid = f[6], type = f[7];
    uint16_t vlen = ((uint16_t)f[8] << 8) | f[9];
    const uint8_t *v = f + 10;
    if (dpid == 8 && type == 0x04 && vlen >= 1) { // ACCESS_RESULT → door log
      const char *result = v[0] == 0 ? "granted" : v[0] == 1 ? "denied" : "expired";
      if (v[0] == 0) markDoorUnlocked(); // mirror the bolt for the dashboard
      publishLog(result, "MCU report");

      // ── KEYPAD AS THE PAIRING GESTURE (operator, 2026-08-12) ─────────────
      //
      // "even after factory reset, does the keypad signal from MCU reach the
      // ESP? if so a user can press any key to turn on BLE?"
      //
      // Not for a bare key press: DP 60 is a PLACEHOLDER the manufacturer has
      // never allocated, and no shipping DL MCU emits one (ozkey-22 §7, still
      // open). Only LockSim sends it.
      //
      // But THIS frame does arrive on real hardware — it is the MCU reporting a
      // completed unlock attempt. Crucially it fires on a DENIED attempt too,
      // so it works on a lock whose credentials were just wiped: walk up, press
      // anything that ends an entry, and the lock becomes discoverable.
      //
      // That is the closest thing to "press a key to pair" that exists today
      // without waiting on the manufacturer, and it is a genuine physical
      // presence signal — which is the property that justifies opening the
      // window at all. On a production lock the BOOT button may be awkward to
      // reach; the keypad never is.
      //
      // ── 🔴 GATED TO FAILED ATTEMPTS ONLY (operator's privacy rule, 1.57) ──
      //
      // "Safe because the window only makes the lock DISCOVERABLE" was the
      // justification in 1.56, and it is true about ACCESS — every bond,
      // envelope and role check still applies. It was WRONG about PRIVACY, and
      // that is a separate question we did not ask.
      //
      // A provisioned lock is otherwise DARK: startBle() runs only inside a
      // window. So if every entry opened one, then `advertising` would mean
      // `someone used this door in the last 60 s`, near 1:1. The ADV carries
      // flags + service UUID + "OZLOCK" and is readable by a PASSIVE scanner
      // from the footpath; an ACTIVE scan also returns the scan response, which
      // since XF-94 carries 4 bytes of MAC as a STABLE per-lock id. Together
      // that is a timestamped, per-door entry log for anyone parked outside:
      // when the occupant leaves, when they return, when the place is empty.
      //
      // We would have been sealing door events away from our own server (C7,
      // ozkey-23) while broadcasting the fact of every entry to the street.
      //
      // The fix is one condition, because the RECOVERY case never needed the
      // granted path: a lock whose credentials were wiped DENIES everything,
      // and a member standing at the door with no credential is denied too. So
      // gating on failure keeps every use case the window exists for —
      //   • wiped lock, owner re-pairs        -> denied  -> window opens
      //   • member enrolment at the door      -> denied  -> window opens
      //   • owner pairing a new phone         -> deliberate wrong PIN, which is
      //                                          a BETTER gesture than 1.56's:
      //                                          intentional, not incidental
      // — and removes the one that leaked: the routine successful unlock, which
      // is the only high-frequency event and the only one that tracks presence.
      //
      // Residual: a mistyped PIN still advertises. Rare, and uncorrelated with
      // routine occupancy, so it does not rebuild the channel.
      //
      // This also means DP 60 is no longer worth waiting for. It buys a
      // deliberate gesture; this buys the same privacy property today, on
      // shipping hardware, with no manufacturer allocation (ozkey-22 §7).
      // ── 🔴 A FAILED PIN NO LONGER OPENS ANYTHING (operator, 2026-08-16) ───
      //
      // 1.81 raised the bar from one failed entry to three. The operator's
      // objection is better than the fix: a pairing gesture made out of FAILED
      // ATTEMPTS teaches people to jab at the keypad, and every jab is radio
      // time. "We encourage people to play more with the doorlock and wear out
      // the battery." On a Sleepy End Device that is the whole power budget,
      // and it was reachable by anyone standing at the door, indefinitely,
      // with no cooldown.
      //
      // A wrong PIN is now what it always should have been: a logged denial and
      // nothing else. The deliberate gesture moved to the DOORBELL (DP 53) —
      // see its handler below for why that is the right button.
            return;
    }
    if (dpid == 5) { publishLog("battery_alarm", "MCU report"); return; }

    // ── DP 53 DOORBELL — the pairing gesture, kept after a market check ────
    //
    // Reinstated 2026-08-16 after the operator surveyed real product
    // catalogues. It briefly did NOT open a window ("pressing the doorbell to
    // turn BLE on opens more chance for BLE hacker") and `*01#` was to be the
    // only gesture. The survey killed that: **`*01#` cannot work on a real
    // lock**, and worse, on some Tuya models the doorbell button PHYSICALLY
    // REPLACES the `*` or `#` key — so the two gestures can be mutually
    // exclusive in hardware. There is no universal keypad behaviour to build
    // on; `#`-as-submit varies by model.
    //
    // 🔴 AND THE DOORBELL IS NOT UNIVERSAL EITHER. DP 53 is a catalogue entry
    // products SELECT (`支持有人按门铃上报` — "supports reporting…"; DP 54's
    // own note says "do not select this DP for the core-board solution").
    // Tuya market it as a tier: doorbell/video-intercom solutions, not a
    // baseline. Our whole catalogue derives from ONE product, DS013-T3, which
    // the T3-U doc classifies as a **Video Lock** — a category that has a
    // doorbell by definition. Do not read "confirmed in the catalogue" as
    // "present on every lock". See ozkey-36 §9.
    //
    // So this is the best gesture available, not a general solution: it works
    // on locks that selected DP 53, and nothing works on the ones that did
    // not. That is a supplier question, not a firmware one.
    //
    // Battery: a doorbell can be spammed, so a window opened this way will not
    // re-open for OZ_BELL_COOLDOWN_MS after it CLOSES. Ringing during an open
    // window still extends it, so an enrolment in progress is never cut off.
    if (dpid == 53) {
      // Proof this lock has a doorbell, which no configuration can give us.
      if (!g_bellObserved) {
        g_bellObserved = true;
        prefs.begin("blelock", false);
        prefs.putBool("bell", true);
        prefs.end();
        Serial.println("[BELL] first doorbell seen — has_doorbell latched true");
        ozRefreshInfoChar(); // info must stop saying false immediately
      }
      publishLog("doorbell", "MCU report");
      const unsigned long now = millis();
      if (bleWindowOpen()) {
        g_bellOpenedWindow = true;
        openBleWindow("doorbell (extending)");
      } else if (!g_bellWindowEndedAt ||
                 (now - g_bellWindowEndedAt) > OZ_BELL_COOLDOWN_MS) {
        g_bellOpenedWindow = true;
        openBleWindow("doorbell");
      } else {
        Serial.printf("[BELL] doorbell — window suppressed, %lus of cooldown "
                      "left (battery)\n",
                      (OZ_BELL_COOLDOWN_MS - (now - g_bellWindowEndedAt)) / 1000);
      }
      return;
    }

    // ── REAL T3 ACCESS-EVENT DPs (1.92) ─────────────────────────────────────
    //
    // DP 61/63/64/72/73/76 — `status: confirmed`, `type: value`,
    // `verb: event.access`, payload = `cred_id`. These are what a REAL lock
    // reports when a credential opens the door, and they REPLACE our fiction
    // (DP 1/2/3) the moment a lock adopts a real product profile.
    //
    // Until now they fell through to UNCLASSIFIED and we published nothing but
    // a shape — so on a real lock, every access event would have been invisible
    // while the invented DPs carried the whole audit trail. Same class of
    // problem as XF-110's DP 1: the fiction worked and the real thing did not.
    //
    // WHY cred_id MATTERS: our fiction could never express WHICH credential
    // opened the door — DP 2 carried a raw card UID, DP 3 a bare bool. These
    // carry the stored slot, which is exactly what an audit line needs.
    //
    // 🔴 REPORT-ONLY. These say a credential WAS USED. They are not credential
    // writes — provisioning is DP 13/14/15, all `status: reserved`, blocked on
    // the supplier's RAW payload layout (ozkey-27 Q2 / ozkey-39 §2).
    //
    // Gated on ozDpFind() so a lock only honours the DPs its own profile
    // selects — an access event on a DP this product does not have is either a
    // bug or a spoof, and the profile is the only thing that can tell.
    if (type == 0x02 && (dpid == 61 || dpid == 63 || dpid == 64 || dpid == 72 ||
                         dpid == 73 || dpid == 76)) {
      if (ozDpFind(dpid) == nullptr) {
        Serial.printf("[ACCESS] DP %u not selected by profile '%s' — ignored\n",
                      dpid, ozProfileId());
        return;
      }
      uint32_t credId = 0;
      for (uint16_t i = 0; i < vlen && i < 4; i++) credId = (credId << 8) | v[i];
      const char *kind = dpid == 61   ? "pin"
                         : dpid == 63 ? "fingerprint"
                         : dpid == 64 ? "rfid"
                         : dpid == 72 ? "remote"
                         : dpid == 73 ? "remote_voice"
                                      : "ble";
      char detail[64];
      snprintf(detail, sizeof(detail), "%s cred_id=%lu (DP %u)", kind,
               (unsigned long)credId, dpid);
      Serial.printf("[ACCESS] %s\n", detail);
      markDoorUnlocked(); // a real access event means the bolt moved
      publishLog("granted", detail);
      return;
    }

    // ── DP 104 `*NN#` KEYPAD COMMAND — REMOVED 2026-08-16 ─────────────────
    //
    // Built, verified on the bench, and deleted the same day once the operator
    // checked real product catalogues. It could never have worked outside
    // LockSim:
    //
    //   • the supplier catalogue carries NO keystroke channel — DP 60 `alarm`
    //     reports THAT a key was pressed (`key_in`), never which; DP 61 carries
    //     a matched `cred_id`, not digits;
    //   • a real MCU sends nothing until the entry is submitted, then only
    //     pass/fail;
    //   • `#`-as-submit is not universal — it varies by model, and on some
    //     locks `#` IS the doorbell button;
    //   • so `*01#` is indistinguishable from any other rejected entry.
    //
    // Keeping a verb only our own emulator can send would have meant a bench
    // that passes and a product that cannot. The gesture is the doorbell
    // (DP 53) where a lock has one, and an open supplier question where it
    // does not.

    // ── 🔴 DP 60 HANDLER DELETED 2026-08-13 — the number was already taken ──
    //
    // This opened the BLE pairing window on an inbound DP 60, described in
    // ozkey-22 §7 as a PLACEHOLDER pending manufacturer allocation and
    // annotated "no shipping DL MCU sends this; only LockSim does".
    //
    // The first real supplier DP list answers that request, and the answer is
    // that the number is taken: on Smart Lock DS013-T3, **DP 60 is the door-lock
    // ALARM channel** — an 18-value enum carrying wrong_finger, wrong_password,
    // pry, low_battery, unclosed_time, system_lock and the rest. It is not a
    // gesture, it is one of the most heavily used reports on the bus.
    //
    // Left in place, a pry alarm or a low-battery report would have opened the
    // lock's BLE window. The comment claiming only LockSim emits it was true of
    // LockSim and false of the supplier.
    //
    // The capability is not lost. Since doorlock-1.57 the primary gesture is a
    // FAILED entry attempt (see the DP 8 branch above), which is a real physical
    // presence signal, needs no manufacturer allocation, and carries the privacy
    // property that gating on failure was chosen for. Re-homing it onto the real
    // catalogue — DP 60's wrong_password/wrong_card values — is ozkey-27 Q3 and
    // is the architect's call, not a silent substitution made here.
  }
  // ── UNRECOGNISED DP — log locally, do NOT publish the payload ────────────
  //
  // 🔴 CHANGED 2026-08-11. This used to hex the WHOLE FRAME and hand it to
  // publishLog(), i.e. straight to the server. That is a blind forward of
  // bytes we have not classified, from a chip whose firmware is not ours.
  //
  // The DL MCU is the lock manufacturer's. We handle exactly two of its
  // reports — DP 8 (access result) and DP 5 (battery). We do not have an
  // enumeration of what else it emits. If any of that carries credential
  // material — an entered PIN, a card UID, a fingerprint template ID — the old
  // path shipped it off-device and into the door log, by DEFAULT, without
  // anyone deciding to. That is exactly what XF-47's no-plaintext-credential
  // rule exists to prevent, and "we did not know it was in there" is not a
  // defence.
  //
  // Found because LockSim reports keypad PIN entry as DP 1 with the digits as
  // a VALUE; our firmware does not recognise DP 1 inbound, so it fell here and
  // the PIN went to the server as a `dp_report` log line. In the simulator
  // that is a modelling artifact — the MECHANISM is real firmware, and we do
  // not know what production hardware sends down it.
  //
  // So: keep full visibility on the SERIAL console (local, for exactly the
  // discovery work we still owe), and publish only the shape — DP id, type,
  // length — never the value. Once the manufacturer tells us what these DPs
  // are (ozkey-22 §6 Q2), the known-safe ones can be promoted to real
  // handlers above and reported properly.
  String hex; hex.reserve(n * 3);
  for (size_t i = 0; i < n; i++) {
    char b[4]; snprintf(b, sizeof(b), "%02X ", f[i]); hex += b;
  }
  hex.trim();
  Serial.printf("[TUYA<-] UNCLASSIFIED DP, NOT published: %s\n", hex.c_str());

  if (ozIsInboundDp(f, n)) {
    // Shape only. Enough to discover which DPs exist and how often, with no
    // possibility of leaking what they carry.
    char shape[64];
    snprintf(shape, sizeof(shape), "unclassified dp=%u type=%u len=%u", (unsigned)f[6],
             (unsigned)f[7], (unsigned)(((uint16_t)f[8] << 8) | f[9]));
    publishLog("dp_unclassified", shape);
  } else {
    publishLog("dp_unclassified", "non-DP frame");
  }
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
void publishLog(const char *result, const char *detail, int actorSlot) {
  txlogAppend(result, detail, actorSlot); // transaction buffer first — works offline
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
  // R5 / ozkey-20 R4: the epoch rides the heartbeat so an app can detect a
  // missed roster change with NO push having succeeded. This is the pull half
  // of the design and it is what makes the system converge.
  //
  // NOTE the `mqtt.connected()` guard above: on a THREAD lock this whole
  // function never runs, which is ozkey-20 §2.1 — the reason Thread locks have
  // never reported liveness. Not fixed here; that is ozkey-20 R3 and it needs
  // the uplink path, not this one.
  doc["roster_epoch"] = g_rosterEpoch;
  // XF-108 §5 — the PID rides the heartbeat too, not just BLE `info`.
  //
  // ftpos capture `tuya_pid` where a BLE session already reads `info`:
  // commissioning and member enrolment. That leaves every ALREADY-COMMISSIONED
  // lock with no PID until someone re-runs one of those — which for an
  // installed door is approximately never. The heartbeat reaches the server
  // unprompted, so the app can learn a lock's identity without anyone standing
  // at it. Same argument the `name` field above is here for.
  if (cfgMcuPid.length()) doc["tuya_pid"] = cfgMcuPid;
  doc["profile"] = ozProfileId();
  doc["has_doorbell"] = ozHasDoorbell();

  // ── ozkey-32 §5 Option A — THE LOCK IS AUTHORITATIVE FOR ITS OWN NAME ─────
  // (operator, 2026-08-14)
  //
  // Once `set_name` can be driven from two directions — BLE while standing at
  // the lock, and remotely once the server route lands — `locks.label` in the
  // server's database and the lock's own `cfgName` can silently disagree. The
  // BLE path is the one that makes this unavoidable rather than unlikely: it
  // works with no connectivity at all, so the server cannot even observe that a
  // rename happened.
  //
  // Reporting the name here makes the server's row a CACHE that reconciles
  // against the device, which is the same shape as roster_epoch, bonds and
  // transport already use. The alternative (server authoritative, re-push on
  // divergence) loses every offline rename and would have the cloud overwrite
  // what the user just typed at the door.
  //
  // Empty is meaningful and is sent as empty, not omitted: "this lock has never
  // been named" is a real state the server should be able to see and fix, and
  // it is exactly the state DoorA has been in all along.
  doc["name"] = cfgName;
  // On EVERY heartbeat, not just enroll. A transport change does not always
  // re-enroll (a re-provision of an already-enrolled lock does not), and this is
  // the only message a lock in service sends unprompted — so it is the only
  // thing that can make a stale server row self-heal within one interval.
  //
  // `full=false`: the coarse `caps` summary rides every heartbeat because it is
  // small and self-healing, but the detailed `verbs` list does NOT. Capability
  // is discovered once at pairing and stored by the app (operator, 2026-08-20);
  // re-sending the whole list every interval spends airtime restating something
  // that cannot change without a reflash. If it DOES change, `profile_mismatch`
  // below is the signal that the app's stored copy is stale.
  addIdentity(doc, false);
  String out; serializeJson(doc, out);
  mqtt.publish(topicHeartbeat.c_str(), out.c_str());
}

// XF-57 (AN), operator directive 2026-08-03: "the doorlock should inform the app
// of its changes." Until now the lock never told the server what it WAS — the
// server inferred capability from whether a bridge was bound, and the app kept
// its own copy of `transport` written at commissioning that nothing ever
// corrected. A lock converted from Thread to Wi-Fi therefore stayed "Thread with
// a bridge" in the app forever, and every "Mở cửa" took a remote path the server
// then refused. The device is the authority on what it is; it must say so, on
// every enroll AND every heartbeat, so the fact self-heals however the change
// was made.
//
// `caps` here is what this TRANSPORT can support, not what this deployment can
// deliver — the lock knows it is on Thread but cannot know its bridge is alive.
// The server intersects this with its own bridge binding (effectiveCaps), so
// neither side can over-promise alone.
static void addIdentity(JsonDocument &doc, bool full) {
  doc["transport"] = cfgTransport;
  JsonArray caps = doc["caps"].to<JsonArray>();
  // Wi-Fi/economy locks sleep and wake on the heartbeat interval, so they cannot
  // promise a live "open now" — unlock there is BLE-at-the-door, by design and
  // by product tier (XF-48 §3). Thread locks are reached through the bridge in
  // ~1 s, proven.
  //
  // XF-58: what a Wi-Fi lock CAN offer is `assisted_unlock` — the owner
  // authorises remotely, the visitor's keypad touch completes it, and this
  // firmware refuses it without a recent touch (see onMqttMessage). Deliberately
  // a DIFFERENT capability name, not a widened `remote_unlock`: it makes a
  // weaker promise (someone must be at the door) and an app that could not tell
  // them apart would offer "unlock remotely" on a lock that only opens when
  // somebody is standing at it.
  // ── 🔴 CAPABILITY IS NOW RESOLVED, NOT ASSERTED (operator, 2026-08-20) ────
  //
  // `pin_sync` used to be added unconditionally. On a real supplier profile the
  // credential DPs are RESERVED — the payload layout has never been supplied —
  // so the lock was telling the app "I can store PINs" about a lock that
  // physically cannot. The app then issued one, everything reported success,
  // and it vanished (ozkey-42 §2.4).
  //
  // A capability is a PROMISE. It must be derived from whether the verb
  // actually resolves to a usable DP on this product, not from what tier of
  // lock we think this is. The transport still decides WHICH unlock we can
  // offer — an economy Wi-Fi lock sleeps and cannot promise "open now" — but it
  // no longer decides WHETHER we can unlock at all.
  const bool canUnlock = ozVerbUsable(ozResolveVerb("lock.unlock", nullptr, OZ_DIR_DOWN));
  const bool canPin    = ozVerbUsable(ozResolveVerb("cred.put", "pin", OZ_DIR_DOWN));
  const bool canRfid   = ozVerbUsable(ozResolveVerb("cred.put", "rfid", OZ_DIR_DOWN));

  if (canUnlock) caps.add(isThread() ? "remote_unlock" : "assisted_unlock");
  if (canPin) caps.add("pin_sync");
  if (canRfid) caps.add("rfid_sync");
  // Ours, not the MCU's: the txlog is written by this firmware and does not
  // depend on any DP being implemented.
  caps.add("audit");

  // The explicit verb list the app asked for. `caps` is a coarse product-tier
  // summary and several consumers already branch on it; this is the precise
  // answer — every OZKIE verb this lock can actually carry out, so the app can
  // disable what it cannot do rather than sending a command nobody understands.
  //
  // Derived from the profile, which is build-time truth. The 0x08 census
  // refines it: a DP the MCU never reports sets `profile_mismatch`, and the
  // operator can then see that the promise and the hardware disagree.
  if (!full) return; // heartbeat: coarse caps only — see publishHeartbeat()
  JsonArray verbs = doc["verbs"].to<JsonArray>();
  static const char *const kProbe[][2] = {
      {"lock.unlock", nullptr},     {"cred.put", "pin"},
      {"cred.put", "rfid"},         {"cred.delete", "pin"},
      {"cred.delete", "rfid"},      {"cred.sync", nullptr},
      {"lock.settings.set", "autolock"}, {"lock.settings.set", "volume"},
  };
  for (const auto &pr : kProbe) {
    const OzVerbMap *m = ozResolveVerb(pr[0], pr[1], OZ_DIR_DOWN);
    if (!ozVerbUsable(m)) continue;
    JsonObject v = verbs.add<JsonObject>();
    v["verb"] = pr[0];
    if (pr[1]) v["field"] = pr[1];
    v["dp"] = m->dp;
  }
}

void publishEnroll() {
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  // `full=true`: enrolment IS pairing, and it is the moment the app learns what
  // this lock can and cannot do. Everything the app needs to disable an
  // unsupported action goes here, once.
  addIdentity(doc, true);
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
// ── M4: the DP dispatch split (CONTRACT.md [XF-47]) ──────────────────────────
//
// | DP       | handling                                        |
// |----------|-------------------------------------------------|
// | 1, 21-24 | forwarded to the Tuya MCU — the existing path   |
// | 101, 102 | handled IN-LOCK, never forwarded                |
// | unknown  | REJECTED, never forwarded                       |
//
// The reject rule is why this is an allow-list and not a pair of `if (dp == 101)`
// guards. Blind forwarding of an authenticated-but-unrecognised verb is not a
// property worth keeping, and on THIS path the frames are not authenticated at
// all: the MQTT broker is anon-open (a guest on the site Wi-Fi can publish to the
// command topic), so everything arriving here is attacker-reachable. 101 and 102
// are bond verbs. The MCU has no concept of a bond, so forwarding one would at
// best be silently ignored and at worst fault the MCU — and executing one here
// would let an unauthenticated publisher revoke the owner's members. They are
// BLE-`control`-only verbs, where a bond, a challenge and a counter floor all
// have to line up first. On this path they are DROPPED, not executed.
// ── 🔴 REPLACED 2026-08-13 — the allow-list was built on an INVENTED map ────
//
// It read `dp == 1 || (dp >= 21 && dp <= 24)`. Against the real Tuya catalogue
// those are: 1 unallocated, 21 navigation_volume, 22 unallocated, 23 auto_lock,
// 24 auto_lock_delay. So on real hardware this permitted exactly the SETTINGS
// DPs and blocked every credential operation — the inverse of its intent
// (ozkey-27 §2.1).
//
// Now a lookup in the active device profile (`ozprofile.h`), which is generated
// from the same `profiles/` JSON LockSim loads. The two ends of the wire can no
// longer disagree about what a number means.
//
// ⚠️ NO BEHAVIOUR CHANGE AT THE DEFAULT PROFILE. `ozkie-legacy-v0` marks DP 1
// `both` and 21-24 `down`, everything else `up`, so the downstream disposition
// permits exactly {1,21,22,23,24} — the old expression, bit for bit. Behaviour
// changes only when the profile is switched, which is a deliberate act.
//
// The reject rule below is unchanged and still the point: on this path frames
// are NOT authenticated (the broker is anon-open, so a guest on the site Wi-Fi
// can publish to the command topic). Blind forwarding of an unrecognised verb
// is not a property worth keeping. 101/102 are bond verbs the MCU has no
// concept of; they are BLE-`control`-only and are DROPPED here, not executed.

// Send a parsed, already-vetted frame. Split out of forwardHexToMcu() so the M4
// `control` path can reach the MCU with bytes it never had to re-hex.
void forwardFrameToMcu(const uint8_t *frame, size_t fn) {
  Serial.printf("[FWD] cmd -> MCU: %s\n", describeDpid(frame, fn).c_str());
  tuyaWireSend(frame, fn);
  // remote unlock (DP 1 BOOL 01): LockSim unlocks on receipt — mirror it
  if (fn >= 11 && frame[3] == 0x06 && frame[6] == 1 && frame[7] == 0x01 &&
      frame[10] == 0x01)
    markDoorUnlocked();
}

// ozkey-13 F2: decode a hex string (spaces/colons tolerated, same convention
// forwardHexToMcu already uses) into `out`, capped at `cap` bytes. Returns
// the decoded length, or 0 on bad hex / an odd nibble count / overflow — 0 is
// never a valid envelope (shorter than the minimum sealed envelope alone),
// so it doubles as the error signal with no separate out-parameter needed.
static size_t ozHexDecode(const String &hex, uint8_t *out, size_t cap) {
  size_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < hex.length(); i++) {
    char c = hex[i];
    if (c == ' ' || c == ':') continue;
    int v = hexNibble(c);
    if (v < 0) return 0;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= cap) return 0;
      out[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  if (hi >= 0) return 0; // odd number of nibbles — truncated hex
  return n;
}

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

  // The dispatch gate. A DPID only exists on a 0x06 DP-write, which is the only
  // shape a server has ever sent — so anything else is unrecognised by
  // definition and falls to the same rejection.
  if (fn < 11 || frame[3] != 0x06) {
    Serial.printf("[FWD] REJECTED — not a DP write (cmd 0x%02X, %u B), not forwarded\n",
                  fn >= 4 ? frame[3] : 0, (unsigned)fn);
    return;
  }
  const uint8_t dp = frame[6];
  if (!ozDpForwardable(dp)) {
    Serial.printf("[FWD] REJECTED DP %u — %s, NOT forwarded to the MCU\n", dp,
                  (dp == 101 || dp == 102)
                      ? "a bond verb; BLE `control` is the only path that may carry it"
                      : "not on the forward allow-list (1, 21-24)");
    return;
  }
  forwardFrameToMcu(frame, fn);
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

// ── C9 §5 — fast poll while somebody is standing at the door ────────────────
//
// A 5 s poll means a queued unlock can sit up to 5 s before the lock asks its
// parent for it. That is fine for a command arriving while nobody is present,
// and awful for the one case where a person is waiting. The BLE window already
// marks exactly that case (a keypad touch or BOOT press opened it), so drop to
// 1 s for its duration and restore afterwards.
//
// No-op unless SED is active: with rx-on there is no poll timer to speed up.
// Takes the OpenThread lock itself, so it must NOT be called from inside a
// section that already holds it (see threadUdpBegin's warning about the
// wrapper methods deadlocking).
void ozThreadApplyPoll(bool fast) {
  if (!cfgThreadSed || !isThread()) return;
  otInstance *inst = esp_openthread_get_instance();
  if (!inst) return;
  const uint32_t ms = fast ? OZ_POLL_FAST_MS
                           : clampThreadPollS(cfgThreadPollS) * 1000UL;
  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
    Serial.println("[THREAD] poll change skipped — stack busy");
    return;
  }
  otError e = otLinkSetPollPeriod(inst, ms);
  esp_openthread_lock_release();
  Serial.printf("[THREAD] poll -> %lu ms (%s) rc=%d\n", (unsigned long)ms,
                fast ? "BLE window open" : "idle", (int)e);
}

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
    // C9 §1: the forced rx-on above is now conditional. `cfgThreadSed` is the
    // ONLY thing that selects sleepy; the ground-truth read-back stays, because
    // the reason it was added — rx-on was ASSUMED from CONFIG_OPENTHREAD_FTD=y
    // and never verified — applies just as much to assuming we went sleepy.
    if (cfgThreadSed) {
      if (lm.mRxOnWhenIdle || lm.mDeviceType || lm.mNetworkData) {
        lm.mRxOnWhenIdle = false; // the whole point: radio off between polls
        lm.mDeviceType   = false; // MTD — a sleepy node must not be routable
        lm.mNetworkData  = false; // stable-only network data, less to carry
        otError le = otThreadSetLinkMode(inst, lm);
        Serial.printf("[THREAD] SED requested — rx-on cleared -> %d\n", (int)le);
      }
      const uint32_t pollMs = clampThreadPollS(cfgThreadPollS) * 1000UL;
      otError pe = otLinkSetPollPeriod(inst, pollMs);
      Serial.printf("[THREAD] SED poll period %lu ms -> %d (readback %lu)\n",
                    (unsigned long)pollMs, (int)pe,
                    (unsigned long)otLinkGetPollPeriod(inst));
      Serial.println("[THREAD] 🔴 SED: this node can no longer hear multicast — "
                     "downlink must be UNICAST or commands will not arrive");
    } else if (!lm.mRxOnWhenIdle) {
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
  // XF-116 — the rising edge of "we can transmit" is the earliest honest
  // moment to announce ourselves, and this function only runs on that edge
  // (its caller guards on !threadUdpReady). Arm the beacon rather than
  // sending here: this runs inside socket setup, and the beacon builder wants
  // the OpenThread lock plus a settled roster/bond count.
  if (ok) g_threadBeaconDue = true;
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

  // ozkey-17 U1 hardening: remember who sent us this, so the uplink can answer
  // by UNICAST rather than relying only on multicast. In practice this is
  // always the bridge — it is the only thing that sends us commands. Unicast
  // is the one destination that gets link-layer ACKs and MAC retries, and a
  // measured multicast loss (counter 131) is why we no longer trust multicast
  // on its own. Captured before the target filter below, deliberately: a
  // datagram addressed to a DIFFERENT lock still proves where the bridge is.
  //
  // 1.29: REFUSE a link-local source. See ozIp6IsLinkLocal() — two Children of
  // the same parent are not link-layer neighbours, so fe80:: between them is
  // undeliverable while lwip_sendto() still reports success. Learning one would
  // make the log say `unicast=yes` for a send that can never arrive, which is
  // worse than having no peer at all.
  {
    const uint8_t *sa = (const uint8_t *)&src.sin6_addr;
    if (ozIp6IsLinkLocal(sa)) {
      if (!g_haveDownlinkPeer)
        Serial.printf("[UPLINK] peer %s is LINK-LOCAL — not routable between "
                      "children, ignoring; uplink stays multicast-only\n",
                      ozIp6Str(sa).c_str());
    } else {
      // ozkey-19 R6: refuse a peer we cannot route to. Cheaper to stay on
      // bootstrap multicast than to unicast into a dead prefix in silence.
      if (!ozPeerPrefixIsOurs(sa)) {
        Serial.printf("[UPLINK] peer %s is on a FOREIGN PREFIX — refusing; "
                      "staying on bootstrap multicast\n", ozIp6Str(sa).c_str());
        goto peer_done;
      }
      {
      const bool changed = !g_haveDownlinkPeer ||
                           memcmp(&g_lastDownlinkPeer.sin6_addr, sa, 16) != 0;
      memcpy(&g_lastDownlinkPeer, &src, sizeof(g_lastDownlinkPeer));
      g_haveDownlinkPeer = true;
      if (changed) {
        Serial.printf("[UPLINK] peer learned: %s\n", ozIp6Str(sa).c_str());
        // R2: only on CHANGE, never on every datagram — NVS has finite write
        // endurance and the bridge's address is stable for long stretches.
        ozUplinkSavePeer(sa);
      }
      }
    peer_done:;
    }
  }

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    Serial.println("[UDP] payload not valid JSON, dropped");
    return;
  }
  // ── ozkey-21 T3 — take the time BEFORE the target check ─────────────────
  //
  // Two reasons this cannot sit below the target filter:
  //   • the standalone beacon is addressed to "*", so it would be dropped as
  //     "not for us" and no Thread lock would ever learn the time;
  //   • a command addressed to a DIFFERENT lock still carries a perfectly good
  //     UTC stamp, and refusing to read it would throw away free syncs.
  //
  // Safe to accept from any datagram because ozClockSet() is monotonic-forward
  // and floors implausible values — see the trust-model note in bridge32.ino's
  // T3 block. A time we already have, or an older one, changes nothing.
  // Timezone rides with the time. Stored even when the clock value itself is
  // refused (stale/duplicate beacon) — the offset is still current information.
  ozHarvestTime(doc, "bridge");

  // XF-115 §4 / Q4 — the request id this datagram is carrying, so an outcome we
  // publish while handling it can NAME the request instead of being matched by
  // timing. bridge32-1.41 stamps it on the downlink; older bridges do not send
  // it and the field is simply absent, which degrades to exactly the pre-1.97
  // behaviour rather than breaking.
  //
  // Scope-guarded for the same reason as the MQTT handler: this function has
  // several early returns, and an id left set would later be attached to a
  // BOOT-hold reset that answers no request at all.
  g_cmdMsgId = doc["msg_id"] | "";
  struct ThreadMsgIdScope {
    ~ThreadMsgIdScope() { g_cmdMsgId = ""; }
  } threadMsgIdScope;

  String target = (const char *)(doc["target"] | "");
  if (target != deviceId) { // not for us
    // "*" is the T3 time beacon: genuinely for everyone, and its whole payload
    // was consumed above. Not an error, so don't log it as one.
    if (target != "*")
      Serial.printf("[UDP] not for us (target='%s' me='%s')\n", target.c_str(), deviceId.c_str());
    return;
  }
  lastActivityAt = millis();

  // ozkey-13 §8 F7: `envelope_hex` (sealed, relayed by bridge32 BR1) is
  // checked first and routed through the SAME F1 core the BLE `control`
  // characteristic and the direct-MQTT path (F2) use — no live challenge
  // exists for a Thread-relayed command any more than a direct-MQTT one, so
  // freshness is counter-only here too (ozkey-13 §5). Falls back to the
  // legacy `payload` pure-forward, unchanged, when absent.
  const char *envHex = doc["envelope_hex"] | (const char *)nullptr;
  if (envHex) {
    Serial.printf("[UDP] << target=%s envelope_hex=%u chars\n", target.c_str(),
                  (unsigned)strlen(envHex));
    uint8_t ebuf[OZ_CTL_MAX];
    const size_t n = ozHexDecode(String(envHex), ebuf, sizeof(ebuf));
    if (n == 0) {
      Serial.println("[UDP] envelope_hex is not valid hex — denied");
      ozReportOutcome(-1, "ENVELOPE_BAD_HEX",
                      String("thread relay, ") + (unsigned)strlen(envHex) + " chars");
      return;
    }
    int slot = -1;
    uint8_t pt[OZ_CTL_MAX];
    size_t ptLen = 0;
    uint64_t counter = 0;
    const OzCtlOpen r =
        ozControlOpen(ebuf, n, &slot, pt, sizeof(pt), &ptLen, &counter);
    // Delivered whole in one UDP datagram — no "still arriving" case, same
    // reasoning as F2's MQTT entry point.
    if (r != OZCTL_OPENED) {
      // 🔴 THE HOLE THAT COST TWO BENCH RUNS. This is a sealed envelope that
      // arrived intact and could not be opened under ANY bond — wrong key,
      // failed MAC, or a truncated body. On the Thread path notifyStatus() goes
      // nowhere, so it was indistinguishable from the datagram never arriving.
      // Now it says so, addressed to the owner, with the size and which failure
      // mode ozControlOpen reported.
      // 1.95: name the failure instead of printing the enum's ordinal. Adding
      // OZCTL_FAILED_NO_BOND shifted MAYBE_INCOMPLETE from 2 to 3, so every
      // "r=2" in an older capture means something different from an "r=2"
      // written today — a log whose meaning silently changed under it is worse
      // than no log (silent-failures rule).
      const char *why = r == OZCTL_FAILED_NO_BOND ? "sender holds no bond"
                        : r == OZCTL_FAILED_MAYBE_INCOMPLETE
                            ? "truncated or still arriving"
                            : "wrong key, failed MAC, or malformed";
      ozReportOutcome(-1, "ENVELOPE_NOT_OPENED",
                      String("thread relay, ") + (unsigned)n + " B — " + why);
      return;
    }
    ozControlVerifyAndDispatch(slot, pt, ptLen, counter, false /*hasChallenge*/,
                               false /*viaBle — arrived over Thread/UDP*/);
    return;
  }

  String payloadHex = (const char *)(doc["payload"] | "");
  Serial.printf("[UDP] << target=%s payload=%s\n", target.c_str(), payloadHex.c_str());
  forwardHexToMcu(payloadHex);
}

// ─────────────────────────────────────────────────────────────────────────────
// U1 (ozkey-17 §6): the lock->app uplink — the half of OZKIE that has never
// existed.
//
// Until now the lock could only ANSWER, and only over an active BLE session to
// a phone physically at the door. That single gap produced most of the
// 2026-08-09 XF traffic: XF-75/78 (an admin tapping cancel six times against a
// roster it could not query), XF-77 (a revoke that happened but could not be
// observed), XF-72/73/74/76 (retry loops compensating for unknowable state).
// Every one of them is "the app cannot ask, and the lock cannot tell."
//
// The crypto for this direction has existed since ozkey-06 and was never used:
// ozEnvSeal() + ozEnvKey(appToLock=false) are byte-verified against the Dart
// side and their only caller was a self-test. What was missing is transport,
// a counter that survives reboot (U0), and something to say.
// ─────────────────────────────────────────────────────────────────────────────

// One datagram to the uplink group. Reuses the socket the receive half is bound
// to — a bound UDP socket sends perfectly well, and a second fd would only be
// another thing to leak. The DESTINATION port differs from the bind port, so
// uplink never lands back in pollThreadUdp()'s stream as an echo.
// ff03::1 — realm-local ALL-NODES. Every Thread node joins this automatically,
// with no explicit subscription and no lwIP join required.
static const uint8_t OZ_ALLNODES_BYTES[16] = {0xff, 0x03, 0, 0, 0, 0, 0, 0,
                                              0,    0,    0, 0, 0, 0, 0, 0x01};

// ozkey-17 U1, corrected 2026-08-10 after the first live uplink went nowhere.
//
// MEASURED, not assumed: across every downlink datagram this bench has ever
// logged, 18 of 18 arrived `"via":"ff03::1"` and ZERO arrived via our own
// group ff03::4f5a — while bridge32 was demonstrably sending 9 copies to each.
// Our custom group has never delivered a single packet here.
//
// Why: `IPV6_JOIN_GROUP failed errno=125` on BOTH boards. OpenThread's
// otIp6SubscribeMulticastAddress() succeeds and makes the stack accept the
// frame off the radio, but lwIP never joined the group, so it never delivers
// it to the socket. Subscribed at the Thread layer, dead at the socket layer.
//
// This went unnoticed for months because bridge32's downlink sprays every
// command at ff03::4f5a, ff03::1 AND unicast — the shotgun worked, which hid
// that two of the three barrels are blanks. The first thing to depend on
// ff03::4f5a alone was this uplink, and it failed immediately.
//
// So: send to both, exactly as the proven downlink does. Success on either is
// success — a duplicate is harmless (the app's counter dedups, and the bridge
// republishing twice is idempotent at the topic level), whereas a miss is the
// whole feature not working.
// ozkey-17 U1, hardened 2026-08-10 after a MEASURED loss.
//
// Uplink delivery observed: 1 unexplained loss in 4 valid attempts. The lost
// one (counter 131, 03:39:17) had the bridge powered, its listener running,
// a successful downlink relay 83 s later and a successful uplink relay 2.5 min
// later — three boards a metre apart. Nothing was broken; the datagram just
// did not arrive. The mesh was reconverging at the time (bridge partition had
// changed from 0x353a218a to 0x0f4788a7), which is exactly when multicast is
// lost.
//
// Root issue is structural, not a bug: UDP multicast on 802.15.4 has NO
// acknowledgement and NO retransmission. One send is one chance. Worse, the
// sender cannot tell — lwip_sendto() returns success for a datagram that
// nothing ever receives, which is why the lock logged a confident "-> thread"
// for a message the app never saw.
//
// ────────────────────────────────────────────────────────────────────────────
// ozkey-19 v2, 2026-08-10 — the burst is GONE. Unicast is the delivery path.
//
// The comment block above reached the right conclusion in mitigation 1 and
// then did not act on it: unicast stayed CONDITIONAL (`if (g_haveDownlinkPeer)`)
// while the multicast burst stayed primary. Net effect of the old
// ozUplinkBurst(): 3 tries x 3 destinations = 9 datagrams per event, of which
//   - 3 went to ff03::4f5a, measured dead here, 0 of 18 ever delivered
//   - 3 went to ff03::1, which has NO MAC ACK and NO retries
//   - 3 went unicast, and only when a downlink had already been seen
// So six copies bought nothing that the radio would not have done better with
// one, and mitigation 2 (retry the multicast) was compensating for retries we
// had switched off by choosing multicast in the first place.
//
// Now: ONE unicast datagram. 802.15.4 retransmits it up to macMaxFrameRetries
// per hop, which is the mechanism the burst was a bad imitation of.
//
// Multicast survives ONLY as bootstrap — a lock that has never seen a downlink
// and has nothing in NVS (R2) still has to reach the bridge somehow. That path
// genuinely has no MAC ARQ, so it keeps a small spaced retry. ff03::4f5a is
// dropped entirely: IPV6_JOIN_GROUP fails errno=125 on BOTH boards, so it is
// dead in both directions, and sending to it is pure airtime.
//
// Jitter still matters on the bootstrap path: several locks reacting to the
// same mesh event would otherwise retry in lockstep and collide repeatedly.
// The old 40 ms spacing was far too tight for the failure it was meant to ride
// out — mesh reconvergence takes hundreds of ms, not tens.
#define OZ_UPLINK_BOOTSTRAP_TRIES  3
#define OZ_UPLINK_GAP_MS         500   // base spacing, bootstrap multicast only
#define OZ_UPLINK_JITTER_MS      250   // random 0..N added per attempt

static bool ozThreadUdpSendOnce(const String &payload, const uint8_t addr[16],
                                const char *label) {
  struct sockaddr_in6 dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin6_family = AF_INET6;
  dst.sin6_port = htons(OZ_THREAD_UPLINK_PORT);
  memcpy(&dst.sin6_addr, addr, 16);
  const int n = lwip_sendto(ozRxFd, payload.c_str(), payload.length(), 0,
                            (struct sockaddr *)&dst, sizeof(dst));
  // 1.29: log the ADDRESS, not just the label. A send that "succeeds" to an
  // unroutable address is indistinguishable from a real one otherwise, which
  // is exactly how counter 197 was lost while the log read `unicast=yes`.
  // Note lwip_sendto() returning >= 0 means QUEUED LOCALLY, never delivered —
  // there is no ACK at this layer for multicast, and for unicast the MAC ACK
  // is invisible from here. Treat this line as "left the building", not
  // "arrived".
  if (n < 0) {
    Serial.printf("[UPLINK]   -> %-14s %s FAILED errno=%d\n", label,
                  ozIp6Str(addr).c_str(), errno);
    return false;
  }
  Serial.printf("[UPLINK]   -> %-14s %s queued %d B\n", label,
                ozIp6Str(addr).c_str(), n);
  return true;
}

// Bootstrap only — no peer known, so we have no unicast target yet. ff03::1 is
// the realm-local ALL-NODES group every Thread node joins implicitly; it needs
// no lwIP join, which is precisely why it works where ff03::4f5a does not.
static bool ozUplinkBootstrapSend(const String &payload) {
  return ozThreadUdpSendOnce(payload, OZ_ALLNODES_BYTES, "ff03::1 BOOTSTRAP");
}

// ── XF-114 §13.4 — say on the WIRE what happened to a reset request ──────────
//
// THE BUG THIS CLOSES, measured on the bench 2026-08-18:
//
//   [MQTT<-] …/command {"action":"factory-reset","envelope_hex":…}
//   [CTL]    OPENED — bond 0, counter 4, OZKIE (24 B)
//   [OZKIE]  factory_reset authorised by owner — wiping
//   [STATUS] FACTORY_RESET (SET ONLY — no live link, links=0)   ← never sent
//   [RESET]  factory reset — wiping NVS + txlog
//
// The command arrived over MQTT; the only acknowledgement we had was a BLE GATT
// write. With no BLE client connected the value went into a characteristic
// nobody was subscribed to, and 150 ms later the platform reset destroyed it.
// The app waited for a signal that was never transmitted on any wire — twice,
// on two different phones, which is why it read as intermittent.
//
// 🔴 THE ACK IS A PROMISE, NOT A REPORT. factoryReset() ends in a platform
// reset and NEVER RETURNS, so this MUST be published BEFORE the wipe. Anything
// published after it is never published. Same ordering rule as the ozkey-22 R1
// MCU reset ack and bridge32's own reset signal.
//
// Shape is bridge32:947's, deliberately unchanged — the server already parses
// {"state","reason"} on the bridge topic and can build the lock half by copying
// a pattern it has running rather than learning a new one (XF-114 §13.1: that
// pattern is bridge-only today; server has to build it a second time, which is
// exactly what it said it would do).
//
// `retain` differs by outcome, and the distinction is bridge32's too:
//   • SUCCESS   → retained. The lock is about to vanish for a while; a late
//                 subscriber must still learn it went away deliberately rather
//                 than reading the silence as a dead lock.
//   • REFUSED   → NOT retained. A refusal is an EVENT, not a liveness state.
//                 Retaining it would replay "I refused" on every reconnect,
//                 long after the request is gone.
static void ozPublishResetOutcome(const char *reason, bool retain) {
  if (!mqtt.connected()) {
    // ── 1.95b — THREAD LOCKS GO VIA THE BRIDGE ───────────────────────────
    //
    // A Thread lock has no MQTT session, so the branch below can never run for
    // it. That was 1.95's stated limit, and it left the whole Thread fleet
    // exactly where XF-114 started: the lock wipes and nobody is told.
    //
    // The carrier already exists and needed no new machinery. The lock sends
    // an UNSEALED presence beacon over Thread UDP every 60 s and bridge32
    // republishes it to `locks/<id>/heartbeat` (ozkey-20 R3). This rides the
    // same socket with a different `kind`, and the bridge routes it to the
    // presence topic instead.
    //
    // UNSEALED is correct here and worth stating: the payload is
    // {state, reason, msg_id} — a device announcing its own reset. There is
    // nothing private in it, and sealing it would be worse than useless since
    // the wipe destroys the keys a moment later and no bond may even exist to
    // seal to (the no_bond case).
    //
    // ⚠ Still fire-and-forget. lwip_sendto() returning >= 0 means QUEUED
    // LOCALLY — never delivered. Dual-sent (unicast for the MAC ACK, ff03::1
    // because it needs no address) exactly as the beacon is, which is the best
    // this layer offers. A lock off the mesh still cannot report, and says so.
    if (isThread() && ozRxFd >= 0) {
      // 🔴 THIS IS AN INTERNAL TRANSPORT FORMAT, NOT THE PUBLISHED SHAPE.
      //
      // Until 1.97 the bridge forwarded this object VERBATIM to MQTT, so these
      // field names — chosen for the bridge's own routing — became the public
      // schema on one of two paths and crashed the app (XF-115). bridge32-1.41
      // now REBUILDS the published message with the shared builder
      // (ozpresence.h) from the fields below. What goes on the wire to the
      // broker is decided in exactly one place, for both transports.
      //
      // `from` and `kind` stay because the bridge routes on them.
      JsonDocument t;
      t["from"] = deviceId;
      t["kind"] = "reset_outcome"; // bridge32 routes on this
      t["state"] = retain ? "offline" : "online";
      t["reason"] = reason;
      if (g_cmdMsgId.length()) t["msg_id"] = g_cmdMsgId;
      String out;
      serializeJson(t, out);
      if (g_haveDownlinkPeer)
        ozThreadUdpSendOnce(out, (const uint8_t *)&g_lastDownlinkPeer.sin6_addr,
                            "reset unicast");
      ozThreadUdpSendOnce(out, OZ_ALLNODES_BYTES, "reset ff03::1");
      // Same reasoning as the MQTT settle below — what follows is a platform
      // reset, so give the radio a moment to actually transmit.
      delay(150);
      Serial.printf("[RESET->] thread relay, reason='%s'\n", reason);
      return;
    }
    // Not silent: this is the REMAINING limit, and a limit you cannot see is
    // the failure mode this whole ticket is about (silent-failures rule). A
    // BLE-delivered reset to a Wi-Fi lock whose broker is down lands here, as
    // does a Thread lock that is off the mesh.
    Serial.printf("[RESET->] '%s' NOT published — no mqtt, no thread (app cannot be told)\n",
                  reason);
    return;
  }
  // 1.97 — built by the SHARED builder (ozpresence.h), not hand-rolled here.
  // Hand-rolling is exactly how the Thread path came to emit a different object
  // than this one and crash the app (XF-115). `retain` and `online` are the same
  // fact — a lock that went away vs one that refused — so the builder derives
  // the retain rule rather than trusting the two to be passed consistently.
  const String out = ozBuildLockPresence(deviceId, !retain, reason, g_cmdMsgId);
  mqtt.publish(topicPresence.c_str(), out.c_str(), ozPresenceShouldRetain(!retain));
  // PubSubClient hands the bytes to the socket but does not wait for them to
  // leave it. The wipe that follows is a platform reset, so give the TCP write
  // a moment to actually go out — the same reasoning as notifyStatus()'s 150 ms
  // BLE settle, for the same reason: the thing we are racing is our own death.
  mqtt.loop();
  delay(120);
  Serial.printf("[RESET->] %s reason='%s'%s\n", topicPresence.c_str(), reason,
                retain ? " (retained)" : " (not retained)");
}

// Retries are scheduled, NOT blocking. ozUplinkSend() is reached from the BLE
// `control` callback (via handleBondRevoke / handleMemberEnroll), and sleeping
// ~110 ms inside a NimBLE host callback to space out retransmits would delay
// the status notification the app is waiting on and risk disturbing the
// connection. The first burst goes immediately; the rest ride loop().
static String  g_uplinkPending;
static uint8_t g_uplinkTriesLeft = 0;
static uint32_t g_uplinkNextAt = 0;

static bool ozThreadUdpSend(const String &payload) {
  if (!threadUdpReady || ozRxFd < 0) return false;

  // ── R1: the normal path. ONE datagram. ────────────────────────────────────
  // 802.15.4 ACKs it per hop and retransmits on silence, up to
  // macMaxFrameRetries. Nothing is armed here on purpose: an application-layer
  // retry on top of a link layer that is already retrying is the mistake this
  // change exists to undo.
  // ozkey-19 R6: the prefix can go stale BETWEEN learning and sending — the
  // mesh re-forms without asking us. Re-check every time; it is one pass over
  // a short list and it is the difference between a delivered message and a
  // silent hole.
  if (g_haveDownlinkPeer &&
      !ozPeerPrefixIsOurs((const uint8_t *)&g_lastDownlinkPeer.sin6_addr)) {
    Serial.printf("[UPLINK] cached peer %s went STALE (mesh re-formed) — "
                  "dropping it, falling back to bootstrap multicast\n",
                  ozIp6Str((const uint8_t *)&g_lastDownlinkPeer.sin6_addr).c_str());
    g_haveDownlinkPeer = false;
    ozUplinkForgetPeer();
  }

  if (g_haveDownlinkPeer) {
    g_uplinkPending   = String(); // supersede any bootstrap attempt in flight
    g_uplinkTriesLeft = 0;
    // ── ozkey-19 R7 — unicast AND ff03::1. Two datagrams, not one. ────────
    //
    // 🔴 WHY, and this is a deliberate retreat from R4's "one datagram":
    //
    // R1 replaced a 9-datagram multicast burst with a single MAC-acknowledged
    // unicast, and that is genuinely better — WHEN THE ADDRESS IS RIGHT. On
    // 2026-08-11 it was not, twice, and the failure was silent: enrol and
    // revoke both executed on the lock, both notifications vanished, and the
    // operator tapped revoke four times at a spinner that could never resolve.
    //
    // I first assumed a stale prefix from a re-formed partition and shipped a
    // validity check for it (ozPeerPrefixIsOurs, still above and still worth
    // having). It never fired — the peer IS on a prefix we hold. So the
    // address is plausible and the datagram still does not arrive, and I do
    // not yet know why.
    //
    // THE POINT: we have NO ACKNOWLEDGEMENT. We cannot tell a delivered
    // uplink from a lost one, so we cannot pick the right single path — that
    // is ozkey-19's whole thesis applied to itself. Until an ACK exists,
    // sending both is the only honest answer:
    //
    //   • unicast  — gets 802.15.4's MAC ACK and retries. Best when routable.
    //   • ff03::1  — needs NO address at all. Every downlink on this bench
    //                arrives this way, so it is demonstrably working.
    //
    // Cost: 2 datagrams instead of 1, against the 9 we started with. Cheap
    // insurance against a whole class of addressing failure we have now been
    // bitten by twice.
    //
    // REMOVE THE MULTICAST COPY when ozkey-20 R3/R7 give us a real
    // reachability signal — not before, and not because one datagram is
    // tidier.
    const bool uni =
        ozThreadUdpSendOnce(payload,
                            (const uint8_t *)&g_lastDownlinkPeer.sin6_addr,
                            "unicast");
    const bool mc = ozThreadUdpSendOnce(payload, OZ_ALLNODES_BYTES, "ff03::1");
    Serial.printf("[UPLINK] 2 datagrams: unicast=%s ff03::1=%s\n",
                  uni ? "ok" : "FAILED", mc ? "ok" : "FAILED");
    return uni || mc;
  }

  // ── Bootstrap: no peer, in RAM or NVS. Multicast, which has no MAC ARQ. ───
  // A newer message supersedes an older pending one rather than queueing
  // behind it — roster_changed is idempotent state ("something changed,
  // resync"), so the freshest is always the useful one.
  const bool sent = ozUplinkBootstrapSend(payload);
  g_uplinkPending   = payload;
  g_uplinkTriesLeft = OZ_UPLINK_BOOTSTRAP_TRIES - 1;
  g_uplinkNextAt    = millis() + OZ_UPLINK_GAP_MS +
                      (esp_random() % (OZ_UPLINK_JITTER_MS + 1));

  Serial.printf("[UPLINK] NO PEER — bootstrap multicast 1/%d, %u retr%s queued "
                "(unacknowledged transport)\n",
                OZ_UPLINK_BOOTSTRAP_TRIES, g_uplinkTriesLeft,
                g_uplinkTriesLeft == 1 ? "y" : "ies");
  return sent;
}


// Called from loop(). Cheap when idle. Only ever active on the BOOTSTRAP path —
// a unicast send arms nothing, because the MAC is already retrying it.
static void ozUplinkRetryTick() {
  if (g_uplinkTriesLeft == 0) return;
  if ((int32_t)(millis() - g_uplinkNextAt) < 0) return; // wrap-safe compare

  // A downlink may have arrived since we armed this, which means we now have a
  // real unicast target. Take it: one acknowledged send beats any number of
  // unacknowledged ones, and it ends the retry immediately.
  if (g_haveDownlinkPeer) {
    ozThreadUdpSendOnce(g_uplinkPending,
                        (const uint8_t *)&g_lastDownlinkPeer.sin6_addr,
                        "unicast (peer arrived)");
    g_uplinkTriesLeft = 0;
    g_uplinkPending = String();
    return;
  }

  ozUplinkBootstrapSend(g_uplinkPending);
  g_uplinkTriesLeft--;
  Serial.printf("[UPLINK] bootstrap retry, %u left\n", g_uplinkTriesLeft);

  if (g_uplinkTriesLeft)
    g_uplinkNextAt = millis() + OZ_UPLINK_GAP_MS +
                     (esp_random() % (OZ_UPLINK_JITTER_MS + 1));
  else {
    g_uplinkPending = String(); // release the buffer
    // R5 territory: this is a send we could not confirm and will not retry.
    // Loud on purpose — silence on failure is what caused the ozkey-19
    // investigation in the first place.
    Serial.println("[UPLINK] bootstrap attempts exhausted, still NO PEER — "
                   "message not acknowledged by anything");
  }
}

// ── REAL-TIME EVENT PUSH — the owner learns NOW, not next time they visit ───
// (operator, 2026-08-15: "I am the doorlock owner, I need to know who open my
//  door in realtime ... guest who got invited thru digital passport, open the
//  door, I should know — that is a beauty of network doorlock.")
//
// The BLE pull (query_events) answers "what happened while I was away". It
// cannot answer "someone is at my door right now", because it requires the
// owner to be standing at the lock. So every real event is ALSO pushed the
// moment it happens, over whatever uplink this lock has — MQTT for Wi-Fi,
// Thread UDP via the bridge otherwise.
//
// Sealed to bond #0 with the lock->app key, so ozlockserv relays a notification
// it cannot read. That is the difference between this and every competitor: the
// owner gets the push, the cloud gets ciphertext.
//
// COMPACT ON PURPOSE. This rides a 152-byte-class datagram on a shared mesh
// (ozkey-20 §4.1 — airtime is the cost that matters), and it is a notification,
// not the record. The record is the log; if the owner wants detail they pull it.
//
// 🔴 WHAT WE HONESTLY KNOW ABOUT *WHO*. When the unlock came through us we have
// the bond slot, so "owner" / "member" is a fact. When it came from the MCU —
// a PIN or card typed at the keypad — DP 8 carries a result byte and NOTHING
// ELSE: no credential id, no slot. So we say `actor:"keypad"`, which is true,
// rather than guessing. The real supplier catalogue does carry `cred_id` on
// 61/63/64/69/72/73/76 (ozkey-28 §3.4), so "which guest's PIN" becomes
// answerable at phase 0 — and not before. Claiming it now would be inventing
// evidence about who entered someone's home.
static void ozEvtPush(const char *verb, const char *result, const char *detail,
                      int actorSlot, uint32_t seq) {
  // event.device is health noise (mcu_timeout, dp_unclassified) — real, kept in
  // the log, but not something to buzz a phone about at 3am.
  if (!strcmp(verb, "event.device")) return;
  if (!ozBond0Present()) return; // nobody to tell

  JsonDocument ev;
  ev["kind"] = verb;
  ev["seq"] = seq;
  ev["result"] = result;
  ev["actor"] = (actorSlot == 0)   ? "owner"
                : (actorSlot > 0)  ? "member"
                                   : "keypad"; // MCU-sourced: identity unknown
  if (actorSlot > 0) ev["bond"] = actorSlot;
  if (detail && *detail) ev["method"] = detail;
  const uint32_t now = ozClockNow(ozclock, millis());
  if (now) ev["at"] = now;
  // Same three-state honesty as the log record: 2 = real sync, 1 = restored
  // guess, 0 = no clock. An owner deciding whether an entry at "3am" matters
  // should know whether the lock actually knew what time it was.
  ev["time_basis"] = !ozClockKnown(ozclock) ? 0 : (g_clockLive ? 2 : 1);

  String out;
  serializeJson(ev, out);
  const bool sent = ozUplinkSend(0, out);
  Serial.printf("[EVT] push %s seq=%lu actor=%s -> %s\n", verb,
                (unsigned long)seq, (const char *)ev["actor"],
                sent ? "uplink" : "NO UPLINK (buffered in log only)");
}

// Seal [json] to the bond in [slot] and emit it on whatever transport this lock
// has. Returns true only if it actually went out — a caller that needs to know
// whether the app can possibly have heard must check, because "sealed fine but
// nothing carried it" is the normal state of a Thread lock whose bridge is down.
// Plaintext outcome code for the NEXT uplink only, cleared after use. See
// ozReportOutcome() for why this is on the wrapper and not inside the seal.
static const char *g_uplinkCode = nullptr;

// ── ozkey-29 §5.1 — SEAL IT BEFORE IT LEAVES, over BLE (the PULL half) ──────
//
// 🔴 This replaces a FALSE CLAIM IN OUR OWN COMMENT. query_events' header
// asserted the response "rides the existing sealed notify path ... That is
// §5.1's 'sealed before it ever leaves'." It did not. It called
// ozNotifyChunked(), which does chrMember->setValue() on the raw string — the
// complete door history, in clear, over the air, to any passive BLE scanner in
// range. That is precisely the data ozkey-29 exists to protect. Documented as
// fixed and shipped unfixed is worse than a known gap, because nobody re-checks.
//
// Same derivation as ozUplinkSend() — lock->app key, per-bond monotonic TX
// counter, ozEnvSeal — reused rather than reinvented, because two crypto paths
// is how the two ends drift apart. Only the carrier differs: chrMember, because
// the app pulls history at the door and a Thread lock has no MQTT session.
//
// Returns false if it could not seal. The caller MUST NOT fall back to
// plaintext: failing closed is the entire point.
static bool ozNotifySealedTo(int slot, const String &json) {
  if (slot < 0 || slot >= OZ_BOND_MAX || !g_bonds[slot].present) return false;
  char appIdHex[65];
  ozHex(g_bonds[slot].pub, 32, appIdHex);
  uint8_t ps[32], key[32];
  const bool haveKey = ozBondSecret(slot, ps) &&
      ozEnvKey(ps, 32, deviceId, String(appIdHex), false /*lock->app*/, key);
  memset(ps, 0, sizeof(ps));
  if (!haveKey) {
    Serial.printf("[EVT] no lock->app key for bond %d — NOT sending\n", slot);
    memset(key, 0, sizeof(key));
    return false;
  }
  const uint64_t counter = ozBondNextTx(slot);
  if (counter == 0) { memset(key, 0, sizeof(key)); return false; }
  uint8_t env[OZ_CTL_MAX];
  const int elen = ozEnvSeal(key, deviceId, counter,
                             (const uint8_t *)json.c_str(), json.length(),
                             env, sizeof(env), nullptr);
  memset(key, 0, sizeof(key));
  if (elen < 0) {
    Serial.printf("[EVT] seal failed (%u B) — NOT sending\n", (unsigned)json.length());
    return false;
  }
  String hex; hex.reserve((size_t)elen * 2 + 16);
  hex = "{\"sealed\":\"";
  char b[3];
  for (int i = 0; i < elen; i++) { snprintf(b, sizeof(b), "%02x", env[i]); hex += b; }
  hex += "\"}";
  Serial.printf("[EVT] sealed %u B -> %d B envelope (bond %d, ctr %llu)\n",
                (unsigned)json.length(), elen, slot, (unsigned long long)counter);
  ozNotifyChunked(hex);
  return true;
}

static bool ozUplinkSend(int slot, const String &json) {
  if (slot < 0 || slot >= OZ_BOND_MAX || !g_bonds[slot].present) return false;

  char appIdHex[65];
  ozHex(g_bonds[slot].pub, 32, appIdHex);

  uint8_t ps[32], key[32];
  const bool haveKey =
      ozBondSecret(slot, ps) &&
      ozEnvKey(ps, 32, deviceId, String(appIdHex), false /*lock->app*/, key);
  memset(ps, 0, sizeof(ps));
  if (!haveKey) {
    Serial.printf("[UPLINK] could not derive bond %d's lock->app key\n", slot);
    memset(key, 0, sizeof(key));
    return false;
  }

  // U0's counter. Claimed BEFORE sealing: a counter burned on a send that then
  // fails to transmit is free, but reusing one on a retry would hand the app two
  // different ciphertexts under the same counter — which is exactly the replay
  // it is there to reject.
  const uint64_t counter = ozBondNextTx(slot);
  if (counter == 0) { memset(key, 0, sizeof(key)); return false; }

  uint8_t env[OZ_CTL_MAX];
  const int elen =
      ozEnvSeal(key, deviceId, counter, (const uint8_t *)json.c_str(),
                json.length(), env, sizeof(env), nullptr /*random nonce*/);
  memset(key, 0, sizeof(key));
  if (elen < 0) {
    Serial.printf("[UPLINK] seal failed (%u B payload, buffer %u)\n",
                  (unsigned)json.length(), (unsigned)sizeof(env));
    return false;
  }

  String hex;
  hex.reserve((size_t)elen * 2 + 1);
  for (int i = 0; i < elen; i++) {
    char b[3];
    snprintf(b, sizeof(b), "%02x", env[i]);
    hex += b;
  }

  // ── ozkey-20 R2 — state our Thread identity so the bridge can join ──────
  //
  // 🔴 WHY THIS FIELD EXISTS. The bridge needs to map a Thread child to a
  // device_id so its liveness report can name locks. Two attempts failed:
  //
  //  1. Match the uplink's SOURCE ADDRESS against the child's registered
  //     IPv6 addresses. Measured 2026-08-11: `otThreadGetChildNextIp6Address()`
  //     returns ZERO addresses for every child on this mesh, so there is
  //     nothing to match against. Structurally impossible, not a tuning issue.
  //  2. Derive it from the MAC. The Thread extended address is RANDOM, not
  //     MAC-derived — DoorA is ozk-acebe639f8c4 but its link-local is
  //     fe80::d879:a06:ac36:7e6f. Unrelated.
  //
  // So the lock states it. The bridge already has every child's mExtAddress
  // from its own child table, making the join a direct 8-byte compare with no
  // addressing, no prefixes, and nothing to go stale.
  //
  // Costs ~22 bytes on an uplink already at 343 B — still 4 6LoWPAN fragments,
  // so no airtime change (ozkey-20 §4.1).
  char extHex[17] = {0};
  {
    otInstance *inst = esp_openthread_get_instance();
    if (inst && esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
      const otExtAddress *ea = otLinkGetExtendedAddress(inst);
      if (ea) ozHex(ea->m8, 8, extHex);
      esp_openthread_lock_release();
    }
  }

  JsonDocument doc;
  doc["from"] = deviceId;      // which lock — the bridge routes on this
  if (extHex[0]) doc["ext"] = extHex; // Thread identity, for the bridge's join
  doc["to"] = appIdHex;        // which app — already public, it is an MQTT topic segment today
  // 🔴 1.64 — the outcome CODE travels in the clear, the detail does not.
  //
  // Four grants failed today and the only thing anyone could observe was a
  // sealed 154-byte blob. I spent three rounds inferring which failure it was
  // from its LENGTH, and got it wrong at least once — exactly the guessing this
  // project keeps paying for. A diagnostic you cannot read is not a diagnostic.
  //
  // This is header-class metadata, not content: it says WHICH STAGE failed, never
  // whose credential or what value. ozkey-27 §4.4 R3 already accepts the same
  // trade for `verb` — relays may read the routing header so they can route,
  // rate-limit and alert; `args` stay sealed. An operator watching MQTT can now
  // see MCU_TIMEOUT vs ENVELOPE_NOT_OPENED without holding any key.
  if (g_uplinkCode) { doc["code"] = g_uplinkCode; g_uplinkCode = nullptr; }
  doc["envelope_hex"] = hex;   // the only part with anything in it
  String out;
  serializeJson(doc, out);

  bool sent = false;
  const char *via = "nothing";
  if (mqtt.connected()) {
    sent = mqtt.publish(topicUplink.c_str(), out.c_str());
    via = "mqtt";
  } else if (threadUdpReady) {
    sent = ozThreadUdpSend(out);
    via = "thread";
  }
  Serial.printf("[UPLINK] bond %d counter %llu %u B -> %s%s\n", slot,
                (unsigned long long)counter, (unsigned)out.length(), via,
                sent ? "" : " (FAILED)");
  return sent;
}

// Push to every admin bond. Used for unprompted events (roster_changed) where
// the audience is "whoever administers this lock" rather than one requester.
// Members are deliberately not told: a member learning that OTHER bonds changed
// is not something the access model ever promises, and it would leak the roster
// to anyone holding a temporary invite.
// ─────────────────────────────────────────────────────────────────────────────
// 🔴 1.62 — REPORT A REFUSAL, NOT JUST A SUCCESS (ozkey-27 §12)
//
// `notifyStatus()` is BLE-only. A Thread lock also has no MQTT, so
// `publishLog()` never leaves it either (ozkey-26 §5). The consequence, found
// the hard way today: a command that arrives over the SERVER path and is
// REFUSED produces no signal anywhere. From outside, "the lock rejected your
// grant as a replay" and "the datagram never arrived" are the same observation
// — silence — and we spent an afternoon unable to tell them apart.
//
// The uplink already exists for successes (ozkey-17 U1). Failures ride the same
// path, sealed to the same bond, so the relay stays blind and no new mechanism
// is invented.
//
// Only sent when NO BLE client is attached: with one, notifyStatus() has
// already answered and a second copy would be noise. And only when the bond is
// known — an envelope we could not open under any bond cannot be sealed back to
// anyone, which is a real remaining hole and is called out in ozkey-27 §12.
// ─────────────────────────────────────────────────────────────────────────────
static void ozReportOutcome(int slot, const char *code, const String &detail) {
  notifyStatus(code);
  if (bleClientConnected) return;
  // An envelope we could not OPEN has no identified sender, so there is no
  // obvious key to answer with — which is precisely the case that stayed
  // invisible and cost us two bench runs. Fall back to bond #0: the owner is
  // who cares that a command arrived this lock could not process, and the
  // notice carries a code and a byte count, never content. Better a report
  // addressed to the owner than no report at all.
  if (slot < 0 || slot >= OZ_BOND_MAX || !g_bonds[slot].present) {
    if (g_bonds[0].present) {
      Serial.printf("[OUTCOME] %s — sender unidentified, reporting to owner (bond 0)\n", code);
      slot = 0;
    } else {
      Serial.printf("[OUTCOME] %s — no bond at all; STAYS INVISIBLE off-device (%s)\n",
                    code, detail.c_str());
      return;
    }
  }
  JsonDocument d;
  d["kind"] = "command_outcome";
  d["code"] = code;
  if (detail.length()) d["detail"] = detail;
  const String ts = isoNow();
  if (ts.length()) d["ts"] = ts;
  String out;
  serializeJson(d, out);
  Serial.printf("[OUTCOME] %s -> uplink (bond %d): %s\n", code, slot, detail.c_str());
  g_uplinkCode = code; // surfaced in the plaintext wrapper — see ozUplinkSend()
  ozUplinkSend(slot, out);
}
static void ozUplinkBroadcastAdmins(const String &json) {
  for (int i = 0; i < OZ_BOND_MAX; i++)
    if (g_bonds[i].present && g_bonds[i].role == OZ_ROLE_ADMIN)
      ozUplinkSend(i, json);
}

// The event that retires XF-75/78 at the root: the lock says the roster moved,
// instead of an admin phone finding out 20 minutes later off a stale cache —
// or not finding out, and tapping "cancel invite" six times.
static void ozNotifyRosterChanged(const char *reason) {
  // R5: bump BEFORE building the payload — the epoch an admin receives must be
  // the one that describes this change, not the one before it. Every caller of
  // this function is a roster mutation, so this is the single correct choke
  // point; bumping at the call sites would eventually miss one.
  const uint32_t epoch = ozRosterEpochBump();

  JsonDocument doc;
  doc["kind"] = "roster_changed";
  doc["reason"] = reason;
  doc["bonds"] = ozBondCount();
  doc["roster_epoch"] = epoch;
  String out;
  serializeJson(doc, out);
  Serial.printf("[ROSTER] epoch -> %lu (%s)\n", (unsigned long)epoch, reason);
  ozUplinkBroadcastAdmins(out);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String body; body.reserve(length);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  Serial.printf("[MQTT<-] %s %s\n", topic, body.c_str());
  lastActivityAt = millis();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return;

  // 1.74 — BEFORE any dispatch, and regardless of what this message is for.
  //
  // A Wi-Fi lock already wakes every 60-600 s, reconnects and resubscribes
  // (enterKeepAliveSleep) — so the broker is an authoritative clock it is
  // ALREADY talking to on every wake, and until now we threw that away and
  // relied on NTP, which this lab blocks. Harvesting here means a retained
  // `utc` on a shared time topic reaches the lock within milliseconds of every
  // reconnect: no polling, no request/response, no extra round trip, and it
  // works on the very first wake after a battery change. Operator's call,
  // 2026-08-14 — "why cant we make use of time".
  //
  // Server side of this is ozkey-33: a RETAINED {"utc":…,"tz":…} on
  // ozkie/<site>/time. It must not be the command topic — a retained message
  // there would be redelivered as a replayed command on every reconnect.
  ozHarvestTime(doc, "server");

  // XF-114 §13.4 — remember which request we are acting on, for the whole of
  // this handler. Every reset path below can then name it in its outcome
  // instead of leaving the app to match on timing.
  //
  // Cleared by a scope guard rather than a line at the bottom: this function
  // has ~20 early returns, so a trailing assignment would miss nearly all of
  // them and leave a stale msg_id to be attached to the NEXT reset — including
  // a BOOT-hold gesture minutes later, which would then claim to answer a
  // request nobody made. Wrong correlation is worse than none.
  g_cmdMsgId = doc["msg_id"] | "";
  struct MsgIdScope {
    ~MsgIdScope() { g_cmdMsgId = ""; }
  } msgIdScope;

  const char *op = doc["op"] | (const char *)nullptr;
  if (op && (strcmp(op, "factory_reset") == 0 || strcmp(op, "unpair") == 0)) {
    Serial.println("[MQTT<-] factory_reset (unpaired by app/server)");
    // 🔴 STILL UNAUTHENTICATED — no seal, no bond, no sender identity, on a
    // broker that enforces no credentials (ozkey-13 S8/S9). PM has directed
    // this path's removal, gated on the server always taking the sealed branch
    // (XF-114 §7.7). It is acked here rather than left half-done: while it
    // exists it is a real way locks get wiped, and a wipe nobody is told about
    // is the exact defect 1.95 exists to close.
    ozPublishResetOutcome("factory_reset", true);
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
  // ── C9 §1/§2 — the way SED and the poll interval are actually SETTABLE ────
  //
  // {"op":"thread_power","sed":true,"poll_s":5}
  //
  // Added because the NVS keys existed with nothing able to write them, which
  // is the same class of mistake as a diagnostic nobody can read: a setting you
  // cannot set is not a setting. Both fields optional, so poll can be tuned
  // without touching the mode.
  //
  // Takes effect immediately for the poll interval; the LINK MODE change needs
  // the stack re-read in threadUdpBegin(), so `sed` is persisted and applied on
  // the next boot. Said out loud in the log rather than left for someone to
  // discover from a current measurement that did not change.
  if (op && strcmp(op, "thread_power") == 0) {
    bool changed = false;
    if (doc["sed"].is<bool>()) {
      const bool want = doc["sed"].as<bool>();
      if (want != cfgThreadSed) { cfgThreadSed = want; changed = true; }
    }
    if (doc["poll_s"].is<uint32_t>()) {
      const uint32_t want = clampThreadPollS(doc["poll_s"].as<uint32_t>());
      if (want != cfgThreadPollS) { cfgThreadPollS = want; changed = true; }
    }
    if (changed) {
      prefs.begin("blelock", false);
      prefs.putBool("sed", cfgThreadSed);
      prefs.putUInt("poll", cfgThreadPollS);
      prefs.end();
    }
    Serial.printf("[THREAD] thread_power: sed=%d poll=%lus%s\n",
                  (int)cfgThreadSed, (unsigned long)cfgThreadPollS,
                  changed ? "" : " (no change)");
    // Poll period can be retimed live; rx-on-when-idle cannot, here.
    ozThreadApplyPoll(bleWindowOpen());
    if (changed && cfgThreadSed)
      Serial.println("[THREAD] SED takes effect on the NEXT BOOT — and requires a "
                     "bridge running bridge32-1.39+ for unicast downlink, or this "
                     "lock will stop receiving commands");
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
    if (topicCommandLegacy.length()) mqtt.subscribe(topicCommandLegacy.c_str(), 1); // S16
    // ozkey-33: RETAINED, so subscribing is itself the request — the broker
    // delivers the current time within ms, on every wake, with no round trip.
    if (topicTime.length()) mqtt.subscribe(topicTime.c_str(), 1);
    // ozkey-33: retained, so this delivers a clock within ms of every wake.
    if (topicTime.length()) mqtt.subscribe(topicTime.c_str(), 1);
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
  // ozkey-13 F2/F5: `envelope_hex` — a sealed control envelope delivered over
  // MQTT instead of BLE, opened/verified/dispatched through the SAME core F1
  // built for the `control` characteristic (ozControlOpen +
  // ozControlVerifyAndDispatch). Checked BEFORE `payload_hex` below so a
  // sealed-capable server can send both during rollout and the authenticated
  // one wins; `payload_hex` alone (pre-migration servers, or a verb not yet
  // moved to sealed delivery) keeps working unchanged — F6 drops it once
  // every server has cut over. No live challenge exists for a queued/remote
  // command (ozkey-13 §5, confirmed acceptable) — freshness is counter-only.
  const char *envHex = doc["envelope_hex"] | (const char *)nullptr;
  if (envHex) {
    uint8_t buf[OZ_CTL_MAX];
    const size_t n = ozHexDecode(String(envHex), buf, sizeof(buf));
    if (n == 0) {
      Serial.println("[MQTT] envelope_hex is not valid hex — denied");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    int slot = -1;
    uint8_t pt[OZ_CTL_MAX];
    size_t ptLen = 0;
    uint64_t counter = 0;
    const OzCtlOpen r =
        ozControlOpen(buf, n, &slot, pt, sizeof(pt), &ptLen, &counter);
    // MQTT delivers the envelope whole in one message — there is no "still
    // arriving" case the way BLE's chunked writes have one, so both failure
    // kinds are equally final here.
    if (r != OZCTL_OPENED) {
      // XF-114 §10 — the run-2 case, on the transport where it actually hurts.
      // notifyStatus() writes a BLE characteristic; a command that arrived over
      // MQTT has no BLE client to hear it, so an unopenable envelope was
      // silence. If the sender simply holds no bond, say so ON MQTT with the
      // msg_id attached: the app is holding a pending removal for a lock that
      // is ALREADY unowned, which is success, not failure.
      notifyStatus(r == OZCTL_FAILED_NO_BOND ? "NO_BOND" : "UNLOCK_DENIED");
      if (r == OZCTL_FAILED_NO_BOND) ozPublishResetOutcome("no_bond", false);
      return;
    }
    ozControlVerifyAndDispatch(slot, pt, ptLen, counter, false /*hasChallenge*/,
                               false /*viaBle — arrived over MQTT*/);
    return;
  }

  // Command envelope {action, grant_id, payload_hex}: PURE FORWARD to the
  // MCU — the comm module never executes credentials.
  const char *hex = doc["payload_hex"] | (const char *)nullptr;
  if (!hex) return;

  // XF-58: `assisted-unlock` is the ONE action the comm module is allowed to
  // refuse. It is the "visitor standing at the door" unlock — the owner
  // authorises it remotely while on the phone, and the visitor's touch is what
  // completes it. The server cannot check that: it has no idea whether anyone
  // is there, and a queue expiry only bounds WHEN the command may run, never
  // WHETHER someone was present. The lock is the only party that knows, so the
  // lock enforces it. Fails closed and silent — a door that does not open is
  // the correct outcome of "nobody was there".
  const char *action = doc["action"] | (const char *)nullptr;
  if (action && strcmp(action, "assisted-unlock") == 0) {
    const bool touched =
        lastTouchAt != 0 && (millis() - lastTouchAt) <= ASSISTED_TOUCH_MAX_MS;
    if (!touched) {
      // ARM, do not discard. The owner has authorised; nobody has arrived yet.
      // A second press replaces the first — two presses must never arm two
      // unlocks, or one touch could open the door twice.
      if (assistArmedUntil)
        Serial.println("[ASSIST] replacing the previously armed unlock");
      assistArmedPayload = String(hex);
      assistArmedUntil = millis() + ASSISTED_ARM_MS;
      Serial.printf("[ASSIST] ARMED %lus — waiting for a keypad touch "
                    "(last touch %s)\n",
                    ASSISTED_ARM_MS / 1000,
                    lastTouchAt ? String((millis() - lastTouchAt) / 1000).c_str()
                                : "never");
      return;
    }
    Serial.printf("[ASSIST] touch %lus ago — assisted unlock ALLOWED\n",
                  (millis() - lastTouchAt) / 1000);
    // Consume it. Without this one touch would satisfy every assisted unlock
    // arriving in the next 30 s, and the owner's second press would open the
    // door for whoever is there by then.
    lastTouchAt = 0;
  }

  forwardHexToMcu(String(hex));
}

// 🔴 THIS FUNCTION BLOCKS THE MAIN LOOP. Everything the lock does — touch
// polling, screen repaint, tuyaWirePump(), and checkFactoryResetButton() —
// runs on this same task, so every millisecond spent inside mqtt.connect() is
// a millisecond the door is unresponsive to the person standing at it.
//
// Until 1.66 the budget was: TCP connect (3 s, NetworkClient default) +
// CONNACK wait (15 s, PubSubClient's MQTT_SOCKET_TIMEOUT) — up to ~18 s dead,
// re-entered every 4 s. A Wi-Fi lock whose broker was unreachable was a brick
// for roughly 18 s out of every 22, INCLUDING the factory-reset gesture that
// is the only physical way out of a bad config. The operator saw this as a
// panel that ignored touches for 5-10 s.
//
// Three changes, none of which alter behaviour against a reachable broker:
//   1. bound both phases (2 s + 2 s) — worst case ~4 s, not ~18 s;
//   2. back off 4 s -> 60 s so a broker that is DOWN is dialled rarely
//      rather than relentlessly;
//   3. say how long the stall actually was, because a blocking call nobody
//      measures is how this survived since the MQTT path was written.
//
// The honest fix is a non-blocking connect on its own task; PubSubClient's API
// is synchronous, so that is a bigger change than this one. This caps the
// damage — it does not make the loop hard-real-time.
void ensureMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < mqttRetryMs) return;
  lastMqttAttempt = millis();
  if (state == ST_JOINING) { joinLine2 = "Server: connecting..."; screenDirty = true; notifyStatus("BROKER_JOINING"); }
  Serial.printf("[MQTT] connecting %s:%u as %s\n", cfgBrokerHost.c_str(), cfgBrokerPort, deviceId.c_str());
  mqtt.setServer(cfgBrokerHost.c_str(), cfgBrokerPort);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(onMqttMessage);
  // Both timeouts set on EVERY attempt, not once at boot: PubSubClient and
  // NetworkClient each own their value, and a reconnect after a WiFi bounce
  // can hand us a fresh socket. Cheap, and it cannot silently revert.
  wifiTcp.setConnectionTimeout(OZ_MQTT_TCP_TIMEOUT_MS);
  mqtt.setSocketTimeout(OZ_MQTT_CONNACK_TIMEOUT_S);
  const unsigned long dialStart = millis();
  // ── 🔴 PRESENT THE BROKER CREDENTIALS (1.57) ────────────────────────────
  //
  // Until 1.57 this was a bare mqtt.connect(deviceId) — client id only, no
  // username, no password. Meanwhile the enrollment_ack handler has ALWAYS
  // stored what the server minted (prefs "buser"/"bsecret"). So the server
  // minted credentials, the lock persisted them, and the lock then
  // authenticated with none of them. ozlockserv's own header called this "the
  // wiring is there; enforcement is not" — accurate about the broker, but the
  // DEVICE half was not wired either, and that is ours.
  //
  // Why this could not wait for the ACL decision: the failure is ONE-WAY. The
  // day anyone enables ACLs on the production broker, every lock and bridge in
  // the field stops connecting at once — a fleet-wide outage caused by a config
  // change on a different team's box. The fix has to already BE in the field
  // before that switch is thrown, which means an OTA rollout, which means weeks
  // of lead time. Shipping it now costs nothing against a broker that ignores
  // credentials and removes the trap entirely.
  //
  // Fallback is deliberate: a lock enrolled before the server minted secrets
  // (or restored from an older NVS) has an empty bsecret and MUST still connect
  // exactly as it does today. Anonymous-until-provisioned stays working.
  const bool haveCreds = cfgBrokerUser.length() && cfgBrokerSecret.length();
  // ── 1.96 — LAST WILL + RETAINED ONLINE, mirroring bridge32 ───────────────
  //
  // 🔴 THIS EXISTS BECAUSE 1.95 SHIPPED HALF A CONTRACT AND WOULD HAVE LIED.
  //
  // 1.95 publishes a RETAINED {"state":"offline","reason":"factory_reset"} to
  // this topic before wiping. Nothing ever published "online" to it, so that
  // retained value would have survived the lock being re-paired and brought
  // back to life — and every future subscriber (a server restart, a new
  // consumer) would read a working lock as factory-reset, indefinitely. A
  // retained message with no counterpart is not a signal, it is a permanent
  // false statement.
  //
  // ozkey-20 R1 specified exactly this and the server has carried
  // handleLockPresence() for it ever since — {"state":"online"|"offline",
  // "reason":"lwt"} — but NO FIRMWARE EVER PUBLISHED IT. The consumer was
  // built and the producer never was, which is the same shape of gap XF-114
  // turned out to be.
  //
  // ⚠ SERVER-VISIBLE BEHAVIOUR CHANGE: Wi-Fi locks now flip `presence` on
  // disconnect instead of only ever aging out via last_seen_at. That is the
  // documented intent, but it is new traffic on a handler that has never
  // received any.
  //
  // ── 🔴 1.97 — THE WILL IS **NOT RETAINED**, and that is load-bearing ──────
  //
  // 1.96 retained it, and measurement showed the Will then DESTROYS the reset
  // outcome it was meant to complement (ozkey-41 §12, LockB, 2026-08-19):
  //
  //   t+0s    {"state":"offline",…,"reason":"factory_reset","msg_id":"ozl-482-…"}
  //   t+~60s  {"state":"offline",…,"reason":"lwt"}          <- retained slot now
  //
  // The collision is STRUCTURAL, not a race that might not happen: a factory
  // reset always ends in a platform reset, which always drops the connection,
  // which always fires the Will. So the retained slot ALWAYS ended up saying
  // "not connected" with no reason and no msg_id, on every reset, on every
  // Wi-Fi lock — losing exactly the information the reset outcome exists to
  // carry.
  //
  // Not retained, the Will still does its real job: it is delivered LIVE to
  // whoever is subscribed, so the server marks the lock offline in real time
  // and handleLockPresence() is unaffected. What it stops doing is overwriting
  // a deliberate, durable statement with a generic one.
  //
  // The retained slot now holds the lock's last DELIBERATE statement — `online`
  // on connect, or `factory_reset` before a wipe.
  //
  // ⚠ KNOWN TRADE (ozkey-41 §12.4a, operator's call): a lock that dies without
  // saying so — flat battery, crash, unplugged — leaves a stale retained
  // `online`. A late subscriber would read it as alive. That is already covered
  // server-side by last_seen_at ageing, and it is the cheaper of the two
  // wrongs: a stale "alive" self-corrects the moment the lock is looked at,
  // whereas a lost factory_reset is unrecoverable.
  const String willPayload =
      ozBuildLockPresence(deviceId, false, OZ_PRESENCE_LWT, String(""));
  const bool ok =
      haveCreds ? mqtt.connect(deviceId.c_str(), cfgBrokerUser.c_str(),
                               cfgBrokerSecret.c_str(), topicPresence.c_str(),
                               1 /*willQos — the BROKER honours this, not us*/,
                               false /*willRetain — see the 1.97 note above*/,
                               willPayload.c_str())
                : mqtt.connect(deviceId.c_str(), nullptr, nullptr,
                               topicPresence.c_str(), 1, false /*willRetain*/,
                               willPayload.c_str());
  if (!haveCreds) {
    // Not fatal today (the lab broker enforces nothing) but it is exactly the
    // device that breaks the moment ACLs land, so say so out loud.
    Serial.println("[MQTT] no broker credentials stored — connecting anonymously");
  }
  // The number that matters for the DOOR is not whether the connect succeeded,
  // it is how long the person outside could not make the panel respond.
  const unsigned long dialMs = millis() - dialStart;
  if (dialMs > 250)
    Serial.printf("[MQTT] dial blocked the main loop for %lums (ok=%d)\n",
                  dialMs, ok ? 1 : 0);
  if (ok) {
    mqttRetryMs = OZ_MQTT_RETRY_MIN_MS; // reachable — dial promptly next time
    lastActivityAt = millis();
    mqtt.subscribe(topicCommand.c_str(), 1);
    if (topicCommandLegacy.length()) mqtt.subscribe(topicCommandLegacy.c_str(), 1); // S16
    // ozkey-33: RETAINED, so subscribing is itself the request — the broker
    // delivers the current time within ms, on every wake, with no round trip.
    if (topicTime.length()) mqtt.subscribe(topicTime.c_str(), 1);
    // ozkey-33: retained, so this delivers a clock within ms of every wake.
    if (topicTime.length()) mqtt.subscribe(topicTime.c_str(), 1);
    if (isLocalMode() && !enrolled) mqtt.subscribe(topicPairConfirm.c_str(), 1);
    // 1.96 — clear our own will, and any stale retained factory_reset from a
    // previous life, the moment we are up. Until this lands the retained value
    // on this topic is whatever we died saying last time: for a lock that was
    // reset and then re-paired, that is "offline / factory_reset" describing a
    // lock which is demonstrably alive and talking. Same call bridge32 makes
    // immediately after its own connect, for the same reason.
    {
      const String online = ozBuildLockPresence(deviceId, true, OZ_PRESENCE_ONLINE,
                                                String(""), FW_VERSION);
      mqtt.publish(topicPresence.c_str(), online.c_str(), true /*retain*/);
    }
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
  } else {
    // Backoff doubles on EVERY failure, not just during ST_JOINING — an
    // operational lock that loses its broker is exactly the one that must not
    // spend the rest of its life re-entering a blocking connect.
    mqttRetryMs *= 2;
    if (mqttRetryMs > OZ_MQTT_RETRY_MAX_MS) mqttRetryMs = OZ_MQTT_RETRY_MAX_MS;
    Serial.printf("[MQTT] connect failed (rc=%d) — next attempt in %lus\n",
                  mqtt.state(), mqttRetryMs / 1000);
    if (state == ST_JOINING) {
      notifyStatus("BROKER_FAIL");
      joinLine2 = "Server: KHONG TOI DUOC";
      screenDirty = true;
    }
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

  // ── ozkey-21 — timezone and time from the app, BOTH transport arms ───────
  //
  // Read here, ABOVE the transport branch, for exactly the reason `app_id` is
  // (XF-47 §11.5): a field parsed inside the Wi-Fi arm silently does not exist
  // on Thread, and that class of bug does not surface until much later.
  //
  // WHY THE LOCK NEEDS ITS OWN COPY even though the bridge sends tz on every
  // beacon: a WI-FI LOCK HAS NO BRIDGE. For it, the app at provisioning is the
  // only timezone source that will ever exist. For a Thread lock this is a
  // head start — correct local time from the first second rather than from the
  // first beacon.
  //
  // `utc` likewise: at a site with NTP blocked (measured — see ozkey-21 §3.4
  // rule 5) and a cold server, the phone in the installer's hand is the only
  // clock in the room. Both fields are optional; an older app simply omits
  // them and nothing changes.
  {
    const bool haveTz = doc["tz"].is<int>() || doc["tz_offset_min"].is<int>();
    if (haveTz) {
      cfgTzMin = (int16_t)(doc["tz"] | doc["tz_offset_min"] | 0);
      prefs.begin("blelock", false);
      prefs.putShort("tzmin", cfgTzMin);
      prefs.end();
      Serial.printf("[TIME] timezone from app: %+d min\n", (int)cfgTzMin);
      screenDirty = true;
    }
    const uint32_t provUtc = doc["utc"] | 0UL;
    if (provUtc >= OZ_TIME_FLOOR) {
      // Through ozClockSet() like every other source — monotonic-forward and
      // the 400-day cap apply to the app exactly as they do to the bridge.
      if (ozClockSet(ozclock, provUtc, millis())) {
        g_clockLive = true; // 1.74 — the app is a real source, not a snapshot
        Serial.printf("[TIME] clock from app at provisioning: %lu\n",
                      (unsigned long)provUtc);
        ozClockPersist(true);
        // Also set the system clock so isoNow() (log timestamps) agrees.
        struct timeval tv = { .tv_sec = (time_t)provUtc, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
      }
    }
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
  cfgHeartbeatS = clampHeartbeatS(doc["heartbeat_s"] | 300);

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

// ─────────────────────────────────────────────────────────────────────────────
// M3 — member enrolment (CONTRACT.md `member_enroll` …0007)
//
// Payload: plaintext JSON `{"app_id":"<member pubkey hex>","invite":"OZINV1:…"}`.
// Unsealed by design: the member holds no bond yet, so there is no key to seal
// under. The INVITE is the authenticator — an HMAC only bond #0's pairing
// secret could have produced, and only this lock can verify.
//
// Everything unauthentic collapses to MEMBER_FAIL. That is deliberate: a
// caller who cannot produce a valid MAC learns only "no", never which field
// they got wrong, which is the difference between a locked door and an oracle.
// The three specific outcomes below are all reachable ONLY after the MAC has
// already verified, so they leak nothing to an attacker.
// ─────────────────────────────────────────────────────────────────────────────

// UTF-8-safe truncation. A label cut mid-sequence is invalid UTF-8 that the app
// would later fail to decode out of a bond listing — the bond would be real and
// unusable, which is worse than a shortened name.
static void copyLabelUtf8(const char *src, char *dst, size_t cap) {
  size_t n = strlen(src);
  if (n > cap - 1) {
    n = cap - 1;
    while (n > 0 && (src[n] & 0xC0) == 0x80) n--; // back off continuation bytes
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

void handleMemberEnroll(JsonDocument &doc) {
  const char *appIdHex = doc["app_id"] | "";
  const char *inviteQr = doc["invite"] | "";

  if (!ozBond0Present()) {
    // An unowned lock has no issuer to verify against, so no invite can be
    // authentic. Refuse rather than invent a trust root.
    Serial.println("[MEMBER] no bond #0 — nothing can authorise a member");
    notifyStatus("MEMBER_FAIL");
    return;
  }
  uint8_t memberPub[32];
  if (!ozIsHex(appIdHex, 32)) {
    Serial.println("[MEMBER] app_id not 64 hex chars");
    notifyStatus("MEMBER_FAIL");
    return;
  }
  ozFromHex(appIdHex, memberPub, 32);
  // The owner scanning their own invite. Harmless in authority terms — bond #0
  // already outranks any member — but it would land on slot 0 and rewrite the
  // OWNER record's label with a member label, and burn a nonce doing it. Name
  // it instead of quietly half-applying it.
  if (memcmp(memberPub, g_bonds[0].pub, 32) == 0) {
    Serial.println("[MEMBER] that is the owner's own key — already bond #0");
    notifyStatus("MEMBER_FAIL");
    return;
  }

  const char *prefix = "OZINV1:";
  const size_t plen = strlen(prefix);
  if (strncmp(inviteQr, prefix, plen) != 0) {
    Serial.println("[MEMBER] invite is not an OZINV1 string");
    notifyStatus("MEMBER_FAIL");
    return;
  }

  // static, not stack: this runs on the BLE task, whose stack is modest, and a
  // long member label pushes the decoded invite past 300 bytes. Safe because
  // exactly one connection can exist at a time (the SCAN_IND busy rule).
  static uint8_t body[512];
  const int bodyLen = ozB64UrlDecode(inviteQr + plen, strlen(inviteQr) - plen,
                                     body, sizeof(body) - 1);
  if (bodyLen <= 0) {
    Serial.println("[MEMBER] invite body is not base64url");
    notifyStatus("MEMBER_FAIL");
    return;
  }
  body[bodyLen] = 0;

  JsonDocument inv;
  if (deserializeJson(inv, (const char *)body) != DeserializationError::Ok) {
    Serial.println("[MEMBER] invite body is not JSON");
    notifyStatus("MEMBER_FAIL");
    return;
  }

  const int         ver     = inv["v"] | 0;
  const String      invDev  = inv["d"] | "";
  const String      issuer  = inv["i"] | "";
  const String      roleStr = inv["r"] | "";
  const String      label   = inv["l"] | "";
  const String      nonceHx = inv["n"] | "";
  const uint32_t    expires = inv["e"] | 0u;
  const String      macHex  = inv["m"] | "";
  // XF-87 'me' — the MEMBERSHIP's own expiry, distinct from 'e' (which is only
  // how long the QR stays redeemable). 0 = permanent. At v2 this is INSIDE the
  // MAC, so unlike 'e' it cannot be altered by whoever holds the QR.
  const uint32_t    memExp  = inv["me"] | 0u;

  // XF-87: accept v1 AND v2. v2 carries the membership expiry inside the MAC.
  // v1 invites minted before the app update must keep verifying byte-identically
  // — anyone holding one in flight would otherwise be locked out of enrolling.
  if ((ver != 1 && ver != 2) || nonceHx.length() != 32 || macHex.length() != 64) {
    Serial.printf("[MEMBER] invite shape rejected (v=%d n=%u m=%u)\n", ver,
                  nonceHx.length(), macHex.length());
    notifyStatus("MEMBER_FAIL");
    return;
  }
  // The invite names the lock it opens. Without this check, an invite minted
  // for the neighbour's lock by an issuer who owns BOTH would enrol here.
  if (invDev != deviceId) {
    Serial.printf("[MEMBER] invite is for '%s', not this lock\n", invDev.c_str());
    notifyStatus("MEMBER_FAIL");
    return;
  }
  // The issuer must be OUR bond #0. The MAC would fail anyway (we key it off
  // our own bond #0 secret regardless of what `i` claims), but checking the
  // claim explicitly makes the serial log diagnosable instead of a bare MAC
  // mismatch — the commonest real cause is a phone that lost its keyring.
  if (issuer != ozBond0PubHex()) {
    Serial.println("[MEMBER] invite issuer is not this lock's owner");
    notifyStatus("MEMBER_FAIL");
    return;
  }
  // v1 has exactly one admin and no bond #0 transfer (CONTRACT.md "Deferred
  // (v2)"). BANOI never sets role, so refusing costs nothing today — and
  // accepting would create a second admin with no revoke story behind it.
  if (roleStr != "member") {
    Serial.printf("[MEMBER] role '%s' refused — v1 issues members only\n",
                  roleStr.c_str());
    notifyStatus("MEMBER_FAIL");
    return;
  }

  uint8_t s0[32];
  if (!ozBond0Secret(s0)) {
    Serial.println("[MEMBER] could not derive bond #0 pairing secret");
    notifyStatus("MEMBER_FAIL");
    return;
  }
  uint8_t want[32], got[32];
  const bool macOk =
      ozInviteMac(s0, 32, invDev, issuer, roleStr, label, nonceHx, expires, want,
                  ver, memExp);
  ozFromHex(macHex.c_str(), got, 32);
  memset(s0, 0, sizeof(s0));
  if (!macOk || !ozCtEq(want, got, 32)) {
    Serial.println("[MEMBER] invite MAC does not verify — refused");
    notifyStatus("MEMBER_FAIL");
    return;
  }

  // `expires` is parse-and-ignore (XF-47): the lock has no clock yet, so
  // enforcing it would be theatre. MEMBER_EXPIRED is reserved and NEVER
  // emitted. The nonce is the hard guarantee; DPID 102 is the kill switch.
  //
  // XF-87/ozkey-21: at v2 `me` is SIGNED, so it is trustworthy — and as of
  // T4/T5 (1.58) we now have both halves it was waiting on: a clock (T1/T2/T3)
  // and a field to keep it in (OzBond.expiresAt). It is STORED and ENFORCED
  // from here; ozBondExpirySweep() deletes the bond when it comes due.
  Serial.printf("[MEMBER] invite v%d VERIFIED label='%s' qr_expires=%u", ver,
                label.c_str(), (unsigned)expires);
  if (ver >= 2)
    Serial.printf(" membership_expires=%u%s", (unsigned)memExp,
                  memExp ? " (SIGNED, ENFORCED)" : " (permanent)");
  Serial.println();

  // A membership that is ALREADY past its expiry must never enrol. Without this
  // an old QR would mint a bond that the sweep then removes seconds later —
  // brief real access, and a confusing "member appeared then vanished" for the
  // owner. Refuse up front instead. Only checkable when the clock is known;
  // with no clock we fall through and the sweep catches it once time arrives.
  if (memExp && ozClockKnown(ozclock)) {
    const uint32_t nowUtc = ozClockNow(ozclock, millis());
    if (nowUtc && nowUtc >= memExp) {
      Serial.printf("[MEMBER] invite membership already expired (%u <= %u) — refused\n",
                    (unsigned)memExp, (unsigned)nowUtc);
      notifyStatus("MEMBER_EXPIRED");
      return;
    }
  }

  uint8_t nonce[16];
  ozFromHex(nonceHx.c_str(), nonce, 16);
  const OzNonceState ns = ozNonceCheck(nonce, memberPub);
  const int existing = ozBondFind(memberPub);

  if (ns == OZ_NONCE_REPLAY) {
    Serial.println("[MEMBER] nonce already burned by a DIFFERENT key — replay");
    notifyStatus("MEMBER_REPLAY");
    return;
  }
  if (ns == OZ_NONCE_SAME_PUB) {
    if (existing >= 0) {
      // The enrolment already happened and our notify was lost. Re-answer OK
      // and touch NOTHING — especially not counter_floor, which would re-open
      // every frame this member has already sent.
      Serial.printf("[MEMBER] idempotent retry — bond %d unchanged\n", existing);
      notifyStatus("MEMBER_OK");
      return;
    }
    // Burned by THIS key, but the bond is gone: it was revoked after redemption.
    // Re-admitting here would make a spent invite resurrect a revoked member and
    // defeat revocation entirely. XF-47's idempotent-retry rule assumes the bond
    // still exists; this is the branch where it does not.
    Serial.println("[MEMBER] nonce spent and the bond was revoked — refused");
    notifyStatus("MEMBER_REPLAY");
    return;
  }

  int slot = existing;
  if (slot < 0) {
    for (int i = 1; i < OZ_BOND_MAX; i++) { // slot 0 is the owner, never reused
      if (!g_bonds[i].present) { slot = i; break; }
    }
  }
  if (slot < 0) {
    Serial.printf("[MEMBER] no free bond slot (%d/%d used)\n", ozBondCount(),
                  OZ_BOND_MAX);
    notifyStatus("MEMBER_FULL");
    return;
  }

  if (existing >= 0) {
    // Re-invite of a key that already holds a bond (BANOI's "gửi lại QR" —
    // doorlock_service.reinviteMember). Refresh the label, KEEP the floor: a
    // floor reset here would re-open every frame this member ever sent.
    Serial.printf("[MEMBER] bond %d already exists — refreshing label, floor kept\n",
                  slot);
  } else {
    g_bonds[slot].present = true;
    g_bonds[slot].role    = OZ_ROLE_MEMBER;
    g_bonds[slot].floor   = 0;
    memcpy(g_bonds[slot].pub, memberPub, 32);
  }
  copyLabelUtf8(label.c_str(), g_bonds[slot].label, OZ_LABEL_MAX);
  // T4: keep the signed `me`. Applies on the re-invite path too — a re-issued
  // QR carrying a NEW window is how an owner extends or shortens an existing
  // member, and it is signed, so honouring it is the correct behaviour. v1
  // invites carry no `me`, so memExp is 0 = permanent, unchanged from before.
  g_bonds[slot].expiresAt = memExp;
  ozBondsSave();
  ozNonceBurn(nonce, memberPub); // ONLY on success — XF-47 §"Nonce replay cache"

  Serial.printf("[MEMBER] bond %d ADDED role=member label='%s' pub=%.16s… (%d/%d)\n",
                slot, g_bonds[slot].label, appIdHex, ozBondCount(), OZ_BOND_MAX);
  screenDirty = true;
  // ozkey-17 U1: the admin phone that issued this invite is usually nowhere
  // near the door when it is redeemed — that gap is why its roster goes stale
  // and why XF-75/78's "cancel invite" taps kept landing on a row that had
  // already become a live bond.
  ozNotifyRosterChanged("member_enrolled");
  notifyStatus("MEMBER_OK");
}

class MemberCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String chunk = String(c->getValue().c_str());
    if (!chunk.length()) return;
    if (chunk[0] == '{') memberBuf = chunk; else memberBuf += chunk;
    JsonDocument doc;
    if (deserializeJson(doc, memberBuf) == DeserializationError::Ok) {
      Serial.printf("[MEMBER] enrol payload complete (%u B)\n", memberBuf.length());
      memberBuf = "";
      handleMemberEnroll(doc);
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// M4 — `control` …0006 (CONTRACT.md "Operational / member profile")
//
// Wire: utf8(app_id_hex, 64) ‖ OzkeyEnvelope, envelope plaintext =
// challenge(16) ‖ DPID frame. Four gates, in this order, ALL of them mandatory:
//
//   1. the sender names a bond            — else there is no key to open with
//   2. the envelope opens under that bond — AEAD authenticity
//   3. the challenge matches the one THIS connection was just issued
//   4. the counter is above that bond's floor
//
// Gate 3 is unconditional across every verb (XF-47), and the reason is not
// obvious: when a revoked pubkey is later re-invited its bond is re-created with
// counter_floor = 0, while frames captured from its previous life carry counters
// 1..N — every one of which clears that floor. Gate 4 is blind to them. The
// stale challenge is the ONLY thing standing between a captured frame and a door
// that opens. Do not "optimise" it away for the non-unlock verbs.
// ─────────────────────────────────────────────────────────────────────────────

// Structural validation of a Tuya DP-write frame, checksum included. The
// envelope already proved WHO sent these bytes; this proves they are a frame at
// all, before any field is read out of them. Length is checked twice over
// (frame-level `len` and the DP's own `vlen`) because they are independent
// fields that a malformed-but-authentic sender could disagree on, and every
// later read indexes off one or the other.
static bool ozTuyaFrameOk(const uint8_t *f, size_t n) {
  if (n < 11 || f[0] != 0x55 || f[1] != 0xAA || f[3] != 0x06) return false;
  const size_t dlen = ((size_t)f[4] << 8) | f[5];
  if (6 + dlen + 1 != n) return false;
  const size_t vlen = ((size_t)f[8] << 8) | f[9];
  if (10 + vlen + 1 != n) return false;
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += f[i];
  return sum == f[n - 1];
}

// Build a DP-write frame in place: 55 AA 00 06 <len:2> <dp> <type> <vlen:2>
// <value> <ck>. Returns the frame length. Used by the self-test below and
// nowhere else — the lock never mints 101/102, it only receives them — but
// generating the vectors instead of transcribing them is the 1.6 lesson: leg 9
// failed on its first flash because a hand-wrapped C literal had silently lost
// 16 characters out of the middle.
static size_t ozBuildDpFrame(uint8_t dp, uint8_t type, const uint8_t *val,
                             size_t vlen, uint8_t *out) {
  const size_t dlen = 4 + vlen;
  out[0] = 0x55; out[1] = 0xAA; out[2] = 0x00; out[3] = 0x06;
  out[4] = (uint8_t)(dlen >> 8); out[5] = (uint8_t)(dlen & 0xFF);
  out[6] = dp; out[7] = type;
  out[8] = (uint8_t)(vlen >> 8); out[9] = (uint8_t)(vlen & 0xFF);
  memcpy(out + 10, val, vlen);
  const size_t n = 6 + dlen + 1;
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < n; i++) sum += out[i];
  out[n - 1] = sum;
  return n;
}

// ── M4 boot self-test: the DP dispatch split ─────────────────────────────────
//
// The M4 property that matters is a NEGATIVE one — 101 and 102 must never reach
// the Tuya MCU — and a negative is exactly what a bench capture is worst at
// proving: "I did not see the frame" and "I was not looking properly" produce
// identical hex. LockSim still shows the wire, but this checks the predicate
// that decides it, on every unit, on every boot, in a line the operator can read
// without a second terminal. Cheap: no I/O, no crypto, runs in microseconds.
static bool ozM4SelfTest() {
  bool ok = true;

  {
    // The forward allow-list. 101/102 are called out separately from the other
    // rejects because they are the ones that would be MEANINGFUL to the MCU's
    // parser if they ever arrived — the rest are merely unrecognised.
    const uint8_t allow[] = {1, 21, 22, 23, 24};
    const uint8_t deny[]  = {101, 102};
    const uint8_t junk[]  = {0, 2, 5, 8, 20, 25, 100, 103, 200, 255};
    bool pass = true;
    for (uint8_t d : allow) if (!ozDpForwardable(d)) pass = false;
    for (uint8_t d : deny)  if (ozDpForwardable(d))  pass = false;
    for (uint8_t d : junk)  if (ozDpForwardable(d))  pass = false;
    ok &= pass;
    // Name the profile: it is what decides the allow-list now, so a PASS is
    // only meaningful alongside which map produced it.
    Serial.printf("[CRYPTO] selftest dp-allow-list %s under profile '%s'%s "
                  "(101/102 forwardable=%d/%d)\n",
                  pass ? "PASS" : "FAIL — 101/102 COULD REACH THE MCU",
                  ozProfileId(), ozProfile()->deprecated ? " [INVENTED MAP]" : "",
                  (int)ozDpForwardable(101), (int)ozDpForwardable(102));
  }

  {
    // The CONTRACT.md frames, byte-for-byte, through the structural validator.
    uint8_t pub[32], nonce[16], f[64];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(0x10 + i);

    const size_t n101 = ozBuildDpFrame(101, 0x00, pub, 32, f);
    bool pass = (n101 == 43) && ozTuyaFrameOk(f, n101) && f[4] == 0x00 &&
                f[5] == 0x24 && f[6] == 0x65;
    // A flipped checksum must fail. This is the branch that stops a corrupted
    // relay frame being executed as a revoke.
    f[n101 - 1] ^= 0xFF;
    pass &= !ozTuyaFrameOk(f, n101);
    f[n101 - 1] ^= 0xFF;
    // So must a frame whose two length fields disagree — they are independent,
    // and every field read downstream indexes off one or the other.
    f[9] = 0x1F;
    pass &= !ozTuyaFrameOk(f, n101);

    const size_t n102 = ozBuildDpFrame(102, 0x00, nonce, 16, f);
    pass &= (n102 == 27) && ozTuyaFrameOk(f, n102) && f[4] == 0x00 &&
            f[5] == 0x14 && f[6] == 0x66;
    ok &= pass;
    Serial.printf("[CRYPTO] selftest dp-frame-101/102 %s (%u B / %u B)\n",
                  pass ? "PASS" : "FAIL", (unsigned)n101, (unsigned)n102);
  }

  Serial.printf("[CRYPTO] selftest M4 %s\n",
                ok ? "PASS (dispatch split holds)" : "FAIL — do not ship this");
  return ok;
}

// DPID 101 — bond_revoke. Value = the target's 32-byte pubkey, raw.
static void handleBondRevoke(int senderSlot, const uint8_t *v, size_t vlen) {
  if (vlen != 32) {
    Serial.printf("[REVOKE] 101 value is %u B, expected 32\n", (unsigned)vlen);
    notifyStatus("REVOKE_DENIED");
    return;
  }
  const bool admin = (g_bonds[senderSlot].role == OZ_ROLE_ADMIN);
  const int target = ozBondFind(v);

  // Bond #0 is never revocable, by anyone, including itself. Checked FIRST so
  // the answer is the same whether or not the caller is the owner.
  if (target == 0) {
    Serial.println("[REVOKE] target is bond #0 — never revocable");
    notifyStatus("REVOKE_DENIED");
    return;
  }
  // A member may revoke exactly one bond: its own ("Rời khỏi cửa này"). This is
  // checked BEFORE the not-found branch on purpose — otherwise a member could
  // walk the table and learn which pubkeys hold a bond on this lock by the
  // difference between DENIED and NOT_FOUND.
  if (!admin && memcmp(v, g_bonds[senderSlot].pub, 32) != 0) {
    Serial.println("[REVOKE] a member may only revoke itself");
    notifyStatus("REVOKE_DENIED");
    return;
  }
  if (target < 0) {
    Serial.println("[REVOKE] no bond holds that pubkey");
    notifyStatus("REVOKE_NOT_FOUND");
    return;
  }

  char label[OZ_LABEL_MAX];
  copyLabelUtf8(g_bonds[target].label, label, sizeof(label));

  // REVOKE FIRST, ANSWER SECOND — including on self-revoke.
  //
  // This was the other way round until 2026-08-04, on the reasoning XF-47 wrote
  // down and I implemented without checking: "emit REVOKE_OK and let it flush
  // BEFORE the bond becomes unusable, or a member can never confirm their own
  // removal." **That premise is false.** `notifyStatus()` writes a plaintext
  // string to …0003 and gates only on the LINK COUNT — it is not sealed under
  // the bond and knows nothing about one. Revoking does not drop the BLE link
  // and cannot stop the answer being delivered. The member confirms their own
  // removal perfectly well from the far side of it.
  //
  // What the old order did buy was a 300 ms window in which the app had been
  // told REVOKE_OK — and would drop the row — while the bond still sat in NVS.
  // A reset or a flat battery inside that window left the member believing they
  // had left a door that still trusted their key. Fail-OPEN, on the one verb
  // whose entire job is withdrawal, in the case that matters most (a cleaner
  // finishing a job, someone leaving a relationship).
  //
  // ftpos independently described the correct model in their member-management
  // report — "the lock forgets first, then the app drops the row" — which is
  // what caught this.
  ozBondRevoke(target); // memsets the slot and commits NVS before we answer
  Serial.printf("[REVOKE] bond %d ('%s') revoked by %s bond %d (%d/%d remain)\n",
                target, label, admin ? "admin" : "member", senderSlot,
                ozBondCount(), OZ_BOND_MAX);
  screenDirty = true;
  notifyStatus("REVOKE_OK"); // the user's answer first…
  // senderSlot, not -1: the actor is the bond that ASKED for the revoke, which
  // we authenticated. Safe on self-revoke even though ozBondRevoke() has already
  // memset the slot — actorSlot is only ever mapped to owner/member/keypad by
  // ozEvtPush(), never used to index g_bonds[].
  publishLog("bond_revoked", label, senderSlot); // …then the housekeeping
  // ozkey-17 U1: and tell every OTHER admin, unprompted. This is the event
  // whose absence produced XF-77 — a revoke that genuinely happened, which the
  // other side could not observe and therefore diagnosed as bond-table
  // corruption. The lock knows; it should say so rather than wait to be asked
  // by whoever next happens to stand in front of it.
  ozNotifyRosterChanged("bond_revoked");
}

// ─────────────────────────────────────────────────────────────────────────────
// ozkey-21 T5 — MEMBERSHIP EXPIRY IS ENFORCED HERE
//
// Until 1.58 nothing in this firmware compared a bond against the clock. The
// invite's `me` was signed (XF-87 v2), verified by us, and then discarded,
// because there was no field to keep it in. ozkey-21 §9 recorded the result on
// real hardware: a membership that expired at 12:38 opened the door at 12:39,
// and would have done so forever.
//
// The operator's ruling (§8, via XF-87 §12) is to DELETE the bond outright
// rather than park it inactive: the window was granted knowingly and signed
// into the invite, so expiry is a pre-authorised outcome, not a decision that
// needs review. Reinstating early is what T7 (amend-by-command) is for.
//
// Reuses ozBondRevoke() + ozNotifyRosterChanged() deliberately — a bond that
// disappears on expiry should be indistinguishable, to every layer above, from
// one an admin revoked. A second removal path would be a second thing to keep
// correct.
//
// 🔴 THREE SAFETY RULES, each of which would be a serious bug to get wrong:
//
//  1. NEVER expire bond #0. The owner has no expiry and must never acquire one
//     — a lock that expired its own owner is unrecoverable without a factory
//     reset. Enforced here even though no invite can set it, because "no path
//     reaches this" is exactly the assumption that stops being true later.
//
//  2. NEVER expire while the clock is unknown. ozClockKnown() gates the whole
//     sweep. A lock that boots with no time would otherwise read now=0, and
//     `0 >= expiresAt` is false so nothing would happen — but relying on that
//     arithmetic accident is not a safety property. Say it out loud instead.
//     Being an hour stale is harmless in the other direction: a late clock only
//     DELAYS an expiry, it never un-expires anything (see ozClockPersist).
//
//  3. expiresAt == 0 means PERMANENT, and that is what every pre-T4 bond and
//     every v1 NVS record reads back as. Nobody already enrolled is affected.
//
// Cadence: cheap (16 slots, integer compares) but not free — it touches NVS
// only when something actually expires. Called on a timer AND before dispatch,
// so a member cannot beat the sweep by unlocking between ticks.
// ─────────────────────────────────────────────────────────────────────────────
#define OZ_EXPIRY_SWEEP_MS 15000UL

static void ozBondExpirySweep() {
  if (!ozClockKnown(ozclock)) return;            // rule 2
  const uint32_t now = ozClockNow(ozclock, millis());
  if (now == 0) return;

  int expired = 0;
  char lastLabel[OZ_LABEL_MAX] = {0};
  for (int i = 1; i < OZ_BOND_MAX; i++) {        // rule 1 — start at 1, never 0
    if (!g_bonds[i].present) continue;
    const uint32_t exp = g_bonds[i].expiresAt;
    if (exp == 0) continue;                      // rule 3 — permanent
    if (now < exp) continue;
    copyLabelUtf8(g_bonds[i].label, lastLabel, sizeof(lastLabel));
    Serial.printf("[EXPIRY] bond %d ('%s') expired at %u, now %u — deleting\n",
                  i, lastLabel, (unsigned)exp, (unsigned)now);
    ozBondRevoke(i); // memsets the slot and commits NVS, same as a revoke
    publishLog("bond_expired", lastLabel);
    expired++;
  }
  if (expired) {
    screenDirty = true;
    // One notify for the sweep, not one per bond: an app resyncing on the epoch
    // wants to know the roster moved, and it re-reads the whole roster anyway.
    ozNotifyRosterChanged("bond_expired");
    Serial.printf("[EXPIRY] %d bond(s) removed, %d remain\n", expired,
                  ozBondCount());
  }
}

// DPID 102 — invite_cancel. Value = the 16-byte invite nonce. Kills a QR that
// was photographed but never redeemed; this is the real expiry control, since
// `expires` is parse-and-ignore on a lock with no clock (XF-47).
static void handleInviteCancel(int senderSlot, const uint8_t *v, size_t vlen) {
  if (vlen != 16) {
    Serial.printf("[CANCEL] 102 value is %u B, expected 16\n", (unsigned)vlen);
    notifyStatus("REVOKE_DENIED");
    return;
  }
  if (g_bonds[senderSlot].role != OZ_ROLE_ADMIN) {
    Serial.println("[CANCEL] 102 is admin-only");
    notifyStatus("REVOKE_DENIED");
    return;
  }

  const OzNonceState ns = ozNonceCheck(v, OZ_NONCE_CANCELLED);
  if (ns == OZ_NONCE_SAME_PUB) {
    // Already burned against the cancel marker. Idempotent: the invite is dead,
    // which is what was asked for, so answering OK is the honest result.
    Serial.println("[CANCEL] invite was already cancelled — idempotent OK");
    notifyStatus("REVOKE_OK");
    return;
  }
  if (ns == OZ_NONCE_REPLAY) {
    // Burned against a REAL pubkey: the invite was redeemed and is now a bond.
    // Cancelling it would do nothing — 101 is the verb for that. Note this is
    // also where a heap failure lands (ozNonceCheck fails closed), which for a
    // CANCEL is the unsafe direction: the invite stays alive. Saying DENIED is
    // what makes that visible enough to retry.
    Serial.println("[CANCEL] nonce already redeemed (or cache unreadable) — "
                   "use 101 bond_revoke on the resulting bond");
    notifyStatus("REVOKE_DENIED");
    return;
  }

  ozNonceBurn(v, OZ_NONCE_CANCELLED);
  Serial.println("[CANCEL] invite nonce burned — that QR can no longer enrol");
  publishLog("invite_cancelled", "admin cancelled an unredeemed invite", senderSlot);
  notifyStatus("REVOKE_OK");
}

// Send a JSON string out over chrMember in MTU-sized pieces, NOTIFY per piece.
// Mirrors MemberCB::onWrite's reassembly rule in reverse: the receiver treats
// a chunk starting with '[' as the start of a fresh buffer and re-tries
// parsing after every piece, exactly like memberBuf does for an inbound
// member_enroll payload. No length prefix, no end marker — "it parses" IS
// the end marker, same convention both directions.
//
// 180 B keeps every piece well under the 244 B payload the app's MTU-247
// request leaves (XF-65 §6) without needing to read back the negotiated MTU.
static void ozNotifyChunked(const String &json) {
  static const size_t CHUNK = 180;
  for (size_t off = 0; off < json.length(); off += CHUNK) {
    size_t remain = (size_t)json.length() - off;
    size_t n = remain < CHUNK ? remain : CHUNK;
    chrMember->setValue((uint8_t *)json.c_str() + off, n);
    chrMember->notify();
    delay(30); // let each notification actually transmit before the next
  }
}

// DPID 103 — list_bonds. Value = empty (request only). Admin-only, same
// leak-prevention reasoning as 101's ordering: a member enumerating every
// other member's pubkey is the thing being prevented, not just self-revoke
// abuse (doorlock.ino:2204-2212 above). Reply is the same {slot, label,
// floor, pub} shape the boot serial dump already prints (line ~3001) —
// reused, not reinvented — delivered via ozNotifyChunked() over chrMember.
static void handleListBonds(int senderSlot, const uint8_t *v, size_t vlen) {
  (void)v; (void)vlen; // request carries no payload
  if (g_bonds[senderSlot].role != OZ_ROLE_ADMIN) {
    Serial.println("[LIST] 103 is admin-only");
    notifyStatus("LIST_DENIED");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 1; i < OZ_BOND_MAX; i++) {
    if (!g_bonds[i].present) continue;
    JsonObject o = arr.add<JsonObject>();
    char h[65];
    ozHex(g_bonds[i].pub, 32, h);
    o["slot"] = i;
    o["label"] = g_bonds[i].label;
    o["floor"] = (unsigned long long)g_bonds[i].floor;
    o["pub"] = h;
  }
  String out;
  serializeJson(doc, out);
  Serial.printf("[LIST] 103 — %d bond(s), %u B JSON, sending chunked\n",
                (int)arr.size(), (unsigned)out.length());
  ozNotifyChunked(out);
  String detail = String(arr.size()) + " member(s)";
  publishLog("bonds_listed", detail.c_str(), senderSlot);
}

static void ctlReset() {
  portENTER_CRITICAL(&ctlMux);
  ctlLen = 0;
  portEXIT_CRITICAL(&ctlMux);
  ctlLastChunkAt = 0;
  ctlNewBytes = false;
  memset(ctlBuf, 0, sizeof(ctlBuf));
}

// Drop the [n] bytes we just processed, KEEPING anything that arrived while we
// were processing them.
//
// This replaced a flat ctlReset() after a bench failure on 2026-08-04, and the
// reasoning that produced the bug is worth keeping written down. I decided a
// write could not land mid-processing "because the app waits for the status
// before sending anything else." That is exactly backwards: notifyStatus()
// holds a 150 ms settle AFTER notify(), so the client receives its answer while
// the lock is still inside that delay and writes immediately — landing the next
// message in the window, every time, rather than rarely. The BLE task appended
// it to the not-yet-cleared buffer and ctlReset() then wiped both.
//
// The failure mode was the worst available: ctlLen went to zero, so the idle
// backstop had nothing left to time out and never fired. A write that produces
// NO status at all — the precise XF-53 hang the backstop exists to prevent,
// reconstructed inside the fix for it. Bench evidence: probe 2 of three
// produced no `[CTL]` line whatsoever while probes 1 and 3, spaced 5 s apart,
// both answered.
static void ctlConsume(size_t n) {
  portENTER_CRITICAL(&ctlMux);
  if (n >= ctlLen) {
    ctlLen = 0;
  } else {
    memmove(ctlBuf, ctlBuf + n, ctlLen - n);
    ctlLen -= n;
  }
  const bool more = (ctlLen > 0);
  portEXIT_CRITICAL(&ctlMux);
  // Anything left is the head of the NEXT message and deserves its own idle
  // budget — otherwise it inherits a stale timestamp and is closed out early.
  ctlLastChunkAt = millis();
  ctlNewBytes = more;
}

// Execute an opened, challenged (or MQTT counter-only, see
// ozControlVerifyAndDispatch), floor-cleared frame.
static void ozControlDispatch(int slot, const uint8_t *frame, size_t flen) {
  const uint8_t dp = frame[6];
  const size_t vlen = ((size_t)frame[8] << 8) | frame[9];
  const uint8_t *v = frame + 10;

  if (dp == 101) { handleBondRevoke(slot, v, vlen); return; }
  if (dp == 102) { handleInviteCancel(slot, v, vlen); return; }
  if (dp == 103) { handleListBonds(slot, v, vlen); return; }

  // ozkey-13 F3: DP 21-24 (temp PIN/RFID add/delete) join DP 1 on the sealed
  // dispatch — reusing ozDpForwardable(), the SAME allow-list the legacy
  // plaintext MQTT path already forwards through, rather than maintaining a
  // second competing list. This supersedes the old comment here (through
  // doorlock-1.21): credential frames used to be refused on this path because
  // the BLE-only `control` channel had no server-side record of what it
  // issued. That reasoning doesn't apply once issuance is sealed-and-relayed
  // over MQTT (ozkey-13 §3-4) — the server still keeps the record (B1, §4),
  // it just never sees the credential value. Anything not in the allow-list
  // is unknown and is rejected, never forwarded.
  if (!ozDpForwardable(dp)) {
    Serial.printf("[CTL] DP %u is not a v1 `control` verb — rejected, "
                  "NOT forwarded\n", dp);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  // ozkey-13 F4: role-gate 21-24 to bond #0. A member may unlock (DP 1, the
  // door they're standing at) but must never issue or delete a credential —
  // same admin-only bar as 101/102/103, checked the same way.
  if (dp != 1 && g_bonds[slot].role != OZ_ROLE_ADMIN) {
    Serial.printf("[CTL] DP %u role-gated to bond #0 — bond %d ('%s', member) "
                  "denied\n", dp, slot, g_bonds[slot].label);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  Serial.printf("[CTL] %s authorised by bond %d ('%s', %s)\n",
                dp == 1 ? "unlock" : "credential frame", slot,
                g_bonds[slot].label,
                g_bonds[slot].role == OZ_ROLE_ADMIN ? "admin" : "member");
  forwardFrameToMcu(frame, flen);
  // Status/log strings deliberately reused rather than inventing GRANT_OK/
  // GRANT_DENIED: this tail was already the generic "sealed control verb
  // forwarded to the MCU" path for DP 1, and 21-24 are the same shape of
  // operation (build frame, forward, done) — one pair of wire strings for
  // "forwarded verb succeeded/failed", not a growing set of near-duplicates.
  // ftpos flagged in XF-69; no objection raised, but call it out to them
  // explicitly since their app code has to match it.
  publishLog("granted", slot == 0
                             ? (dp == 1 ? "BLE unlock (owner)" : "credential (owner)")
                             : "BLE unlock (member)",
             slot);
  notifyStatus("UNLOCK_OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// F8 (ozkey-17 §4): OZKIE semantic dispatch — the network speaks JSON, and the
// Tuya frame is born HERE.
//
// ozkey-13 moved frame COMPOSITION off the server and encrypted it, but a Tuya
// 55 AA frame still crossed three network hops — as ciphertext. Sealing a
// proprietary wire format is not the same as not using one: it left every
// command shaped by Tuya's single-byte DP id and fixed value layout, which is
// exactly the cage ozkey-17 exists to leave. Encryption without a protocol of
// our own is a locked door on a rented house.
//
// So the plaintext inside a sealed envelope is OZKIE JSON, and the 55 AA frame
// is built in this function, microseconds before tuyaWireSend() pushes it at
// the strike MCU. That isolates the proprietary dependency to one replaceable
// sub-board rather than spreading it across the app, the server and the mesh.
//
// CONTRACT (normative — ozkey-17 §6c. The lock is the reference implementation;
// server and app align to THIS, not the reverse):
//
//   {"kind":"unlock"}                                            any bond
//   {"kind":"grant_pin",  "slot":N,"cred":hex,"from":ts,"to":ts}  bond #0 only
//   {"kind":"delete_pin", "slot":N}                               bond #0 only
//   {"kind":"grant_rfid", "slot":N,"cred":hex,"from":ts,"to":ts}  bond #0 only
//   {"kind":"delete_rfid","slot":N}                               bond #0 only
//   {"kind":"bond_revoke","pub":hex(64 chars)}                    bond #0 only
//   {"kind":"invite_cancel","nonce":hex(32 chars)}                bond #0 only
//   {"kind":"list_bonds"}                                         bond #0 only
//
// An unrecognised `kind` is REJECTED — never guessed at, never forwarded. Same
// allow-list discipline ozDpForwardable() enforces on the legacy path, and for
// the same reason: blind forwarding of an authenticated-but-unrecognised verb
// is not a property worth keeping.
// ─────────────────────────────────────────────────────────────────────────────

#define OZ_SEM_VAL_MAX 128 // largest DP value we will build (RFID cred + slot + window)

// DP 21/23 RAW value: slot(2 BE) ‖ credential ‖ from(4 BE) ‖ to(4 BE) — the
// layout ozctl.py's dp_grant() mirrors and the MCU already parses. Returns the
// value length, or 0 if any field is missing/oversized (0 is never valid: a
// grant always carries at least a slot and a window).
// ── 🔴 1.60 — `cred` ENCODING IS PER-KIND. It used to be hex for both. ──────
//
// This silently broke every PIN grant ever issued, and did it in the most
// expensive possible way: the lock said UNLOCK_OK and the MCU threw the frame
// away without a word.
//
// The old code ran ozHexDecode() on `cred` unconditionally. For an RFID UID
// that is right — a UID *is* hex, so "7B3F91D2" -> 7B 3F 91 D2 round-trips. For
// a PIN it is a disaster: the app sends the digits the user was shown,
// "482915", which ozHexDecode reads as HEX and turns into three bytes
// 48 29 15. The MCU decodes a PIN as ASCII, gets "H)<0x15>", fails its
// all-digits check and discards it. Reproduced end to end against LockSim's own
// parser before writing this fix.
//
// The supplier settles which side was wrong: DS013-T3 §16/§18 specify password
// data as 密码数据传输字符的ASCII码 — ASCII. So the wire carries ASCII digits
// for a PIN, and the hex-decode was our defect.
//
// Branching on the VERB, not on the content: "482915" is valid hex *and* valid
// decimal, so sniffing the string could never disambiguate. grant_pin means
// ASCII, grant_rfid means hex, and there is nothing to guess.
static size_t ozSemGrantValue(JsonDocument &doc, bool isPin, uint8_t *out, size_t cap) {
  const uint32_t slotNo = doc["slot"] | 0xFFFFFFFFu;
  const uint32_t from   = doc["from"] | 0u;
  const uint32_t to     = doc["to"]   | 0u;
  if (slotNo > 0xFFFF) return 0;

  uint8_t cred[72];
  size_t clen = 0;
  const char *credStr = doc["cred"] | "";

  if (isPin) {
    // ASCII digits, verbatim. Rejecting a non-digit here rather than passing it
    // on is deliberate: the MCU would drop it silently, which is exactly the
    // failure mode this whole change exists to remove.
    const size_t n = strlen(credStr);
    if (n == 0 || n > sizeof(cred)) return 0;
    for (size_t i = 0; i < n; i++) {
      if (credStr[i] < '0' || credStr[i] > '9') {
        Serial.printf("[OZKIE] grant_pin: `cred` must be ASCII DIGITS, got '%s'\n", credStr);
        return 0;
      }
      cred[i] = (uint8_t)credStr[i];
    }
    clen = n;
  } else {
    clen = ozHexDecode(String(credStr), cred, sizeof(cred));
  }
  if (clen == 0 || 2 + clen + 8 > cap) return 0;

  size_t n = 0;
  out[n++] = (uint8_t)(slotNo >> 8);
  out[n++] = (uint8_t)(slotNo & 0xFF);
  memcpy(out + n, cred, clen);
  n += clen;
  for (int i = 3; i >= 0; i--) out[n++] = (uint8_t)(from >> (8 * i));
  for (int i = 3; i >= 0; i--) out[n++] = (uint8_t)(to >> (8 * i));
  return n;
}

// ── Q1 (ozkey-17 §6b): query rate limiting ───────────────────────────────────
// Sized against the threat that actually exists. A "rogue app" must already
// hold a bond to send a sealed query at all, so that is the narrow case. The
// observed case is our own software: XF-72/74/76 were all BANOI retry-looping
// against a lock, and XF-81 caught it polling list_bonds 8 times in 90 seconds.
// On a 4xAA battery lock the cost of a query storm is BATTERY, not CPU.
//
// So: generous enough never to interfere with legitimate admin use, firm
// enough to stop a runaway loop, with a longer cooldown once a caller has
// demonstrated it is looping rather than asking.
#define OZ_QUERY_MIN_GAP_MS  2000UL   // per bond, between accepted queries
#define OZ_QUERY_STRIKES_MAX 5        // rapid-fire attempts before cooldown
#define OZ_QUERY_COOLDOWN_MS 30000UL  // enforced quiet period after that

static uint32_t g_qLastMs[OZ_BOND_MAX];
static uint8_t  g_qStrikes[OZ_BOND_MAX];

// RAM-only by design: a rate limit that survived reboot would let a lock that
// brownouts mid-storm come back still refusing its owner.
static bool ozQueryRateOk(int slot) {
  const uint32_t now = millis() ? millis() : 1; // 0 means "never queried"
  const uint32_t last = g_qLastMs[slot];
  if (last == 0) { g_qLastMs[slot] = now; g_qStrikes[slot] = 0; return true; }

  // Unsigned subtraction is millis()-wrap safe; do not compare timestamps.
  const uint32_t gap = now - last;
  const uint32_t need = (g_qStrikes[slot] >= OZ_QUERY_STRIKES_MAX)
                            ? OZ_QUERY_COOLDOWN_MS
                            : OZ_QUERY_MIN_GAP_MS;
  if (gap < need) {
    if (g_qStrikes[slot] < 255) g_qStrikes[slot]++;
    Serial.printf("[OZKIE] query from bond %d rate-limited (%lums < %lums, "
                  "%u strikes)\n", slot, (unsigned long)gap,
                  (unsigned long)need, g_qStrikes[slot]);
    return false;
  }
  g_qLastMs[slot] = now;
  g_qStrikes[slot] = 0; // asked politely — forgiven
  return true;
}

// Budget for a query response's PLAINTEXT, before sealing.
//
// A Thread datagram is bounded by the IPv6 minimum MTU (1280 B). Our wire path
// costs roughly: plaintext -> +37 B envelope -> x2 for hex -> +~130 B of
// {from,to,envelope_hex} wrapper. Working backwards from ~1200 B of usable UDP
// payload leaves about 500 B of plaintext. A full 15-member roster at ~106 B
// per entry would be ~1600 B and would simply vanish — silently, since a
// too-large sendto fails at a layer nothing here logs.
//
// So responses are BUDGETED and say so when truncated, rather than being
// quietly lost. Full enumeration when standing at the door is what BLE
// `list_bonds` is still for; pagination is future work if it is ever needed.
#define OZ_QUERY_PLAINTEXT_BUDGET 480

static void ozSemanticDispatch(int slot, const char *json, size_t len, bool viaBle) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    Serial.println("[OZKIE] plaintext is authentic but not valid JSON — rejected");
    notifyStatus("UNLOCK_DENIED");
    return;
  }
  const char *kind = doc["kind"] | (const char *)nullptr;
  if (!kind) {
    Serial.println("[OZKIE] no `kind` field — rejected");
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  // ── in-lock verbs — these never reach the MCU, which has no concept of a
  // bond. Their own handlers own the role check (and 101's is subtler than
  // "admin only": bond #0 is never revocable and a member may revoke itself),
  // so they are called directly with the same value bytes the DP path fed them
  // rather than being re-gated here.
  // ── ozkey-21/ozkey-23 — SEALED remote factory reset ─────────────────────
  //
  // WHY THIS EXISTS. "Remove lock from the app" published
  // {"op":"factory_reset"} to `locks/<id>/command` — the LOCK's own MQTT topic.
  // A Thread lock has no Wi-Fi and never subscribes to MQTT, so that message
  // could never arrive; verified on the broker 2026-08-11 and confirmed on the
  // bench (the lock did not reset). It works on a Wi-Fi lock, which is why it
  // looked like a regression rather than a routing gap.
  //
  // WHY SEALED rather than teaching the bridge to relay a bare `op` verb:
  // "wipe this lock" is the most destructive command we have. Unauthenticated,
  // on a broker that today enforces NO credentials at all (ozkey-13 S8/S9 —
  // a fabricated username still publishes), it is a one-line site-wide DoS.
  // Remote revoke already rides the sealed envelope for exactly this reason;
  // this is the same verb class and gets the same treatment.
  //
  // OWNER ONLY. `slot` is the bond the envelope authenticated against, and
  // bond #0 is the owner. A member must never be able to factory-reset the
  // lock they were invited to — that is an escalation from "I have access" to
  // "nobody has access", including the owner.
  if (strcmp(kind, "factory_reset") == 0 || strcmp(kind, "unpair") == 0) {
    if (slot != 0) {
      Serial.printf("[OZKIE] %s REFUSED — bond %d is not the owner\n", kind, slot);
      notifyStatus("REVOKE_DENIED");
      // XF-114 §13.4 — and say it on the wire too. A member's refused reset
      // used to exist only as a BLE notify and a Serial line, so over MQTT it
      // was indistinguishable from the lock never having heard the request.
      // NOT retained: a refusal is an event, not a liveness state.
      ozPublishResetOutcome("factory_reset_denied", false);
      return;
    }
    Serial.printf("[OZKIE] %s authorised by owner — wiping\n", kind);
    // Tell the app before we go: factoryReset() ends in a platform reset and
    // never returns, so anything sent after it is never sent. Same ordering
    // rule as the ozkey-22 R1 MCU reset ack.
    //
    // 🔴 BOTH transports, deliberately. notifyStatus() reaches a BLE client if
    // one is connected; ozPublishResetOutcome() reaches the network. The bench
    // failure (XF-114 §9.3) was a command delivered over MQTT whose only
    // acknowledgement was the BLE half — `links=0`, nothing sent, app waited
    // forever. Neither call knows which transport carried the request, and
    // neither needs to: whichever one the app is listening on, it hears.
    notifyStatus("FACTORY_RESET");
    ozPublishResetOutcome("factory_reset", true);
    factoryReset(); // never returns
    return;
  }

  // ── set_name — the lock learns what the user called it (operator, 2026-08-14)
  //
  // WHY THIS HAD TO EXIST. cfgName was writable from exactly two places: the
  // BLE provision payload, and enrollment_ack's `label` — and the latter only
  // filled an EMPTY name and only exists over MQTT. So:
  //   • an app that does not send `name` at pairing left the lock permanently
  //     nameless, and the panel fell back to the device id forever;
  //   • renaming the lock in the app afterwards never reached the lock at all;
  //   • a THREAD lock has no MQTT, so it had no naming path whatsoever.
  // The operator's report was "it still shows device_id instead of the doorlock
  // name after pairing" — the panel was right, it had nothing else to show.
  //
  // Sealed like every other settings verb, so it works on all three transports
  // (BLE control, MQTT envelope_hex, Thread UDP) through this one dispatch.
  //
  // OWNER ONLY. The displayed name is what a person at the door reads to decide
  // which lock they are standing at, so it is a trust surface: a member who
  // could rename it could make one door impersonate another. Naming is cheap to
  // do through the owner and not worth the ambiguity.
  // ── ozkey-34 F-9 — provision_key: NEXUS-issued identity, DEVELOPMENT ONLY ──
  //
  // {"kind":"provision_key","pub":"<64 hex>","priv":"<64 hex>"}
  //
  // Three refusals, in order of how badly they would end:
  //
  // 1. PRODUCTION REFUSES OUTRIGHT. A production lock's identity lives in
  //    eFuse and is not writable over the air by anyone, ever. If this verb
  //    could overwrite it, the entire point of burning silicon would be a
  //    60-second BLE window away from being undone.
  // 2. OWNER ONLY. Same bar as every other identity-affecting verb.
  // 3. The pair must AGREE — pub must be the X25519 base multiplication of
  //    priv. NEXUS sends both, so a mismatch means corruption or a hostile
  //    sender, and installing a pub the lock cannot prove would produce a lock
  //    that is unreachable in a way nothing on the wire could diagnose.
  //
  // 🔴 INSTALLING THIS DESTROYS EVERY EXISTING BOND — ozkey-34 §6. Bond
  // secrets are X25519(lock_priv, member_pub), so replacing lock_priv silently
  // changes every one of them. We therefore WIPE THE BOND TABLE rather than
  // leave a lock that reports bonds=1 and refuses every envelope. The app MUST
  // re-pair afterwards; PROVISION_KEY_OK means "new identity, start over".
  if (strcmp(kind, "provision_key") == 0) {
    if (g_modeProduction) {
      Serial.println("[OZKIE] provision_key REFUSED — this lock is production "
                     "(identity is in eFuse and is not writable)");
      notifyStatus("PROVISION_KEY_DENIED");
      return;
    }
    if (slot != 0) {
      Serial.printf("[OZKIE] provision_key REFUSED — bond %d is not the owner\n", slot);
      notifyStatus("PROVISION_KEY_DENIED");
      return;
    }
    uint8_t np[32], nb[32], check[32];
    if (ozHexDecode(String((const char *)(doc["priv"] | "")), np, sizeof(np)) != 32 ||
        ozHexDecode(String((const char *)(doc["pub"] | "")), nb, sizeof(nb)) != 32) {
      Serial.println("[OZKIE] provision_key: pub/priv must both be 64 hex chars");
      notifyStatus("PROVISION_KEY_DENIED");
      return;
    }
    ozClamp(np);
    if (!ozX25519Base(np, check) || memcmp(check, nb, 32) != 0) {
      Serial.println("[OZKIE] provision_key: pub does NOT match priv — refusing "
                     "an identity this lock could not prove");
      memset(np, 0, sizeof(np));
      notifyStatus("PROVISION_KEY_DENIED");
      return;
    }

    memcpy(g_lockPriv, np, 32);
    memcpy(g_lockPub, check, 32);
    memset(np, 0, sizeof(np));
    g_lockKeyReady = true;
    prefs.begin("blelock", false);
    prefs.putBytes("xpriv", g_lockPriv, 32);
    prefs.putBytes("xpub", g_lockPub, 32);
    prefs.end();

    Serial.printf("[OZKIE] provision_key ACCEPTED — new info.pub=%s\n",
                  ozLockPubHex().c_str());
    ozRefreshInfoChar(); // the whole point: info.pub must report the NEW key
    Serial.println("[OZKIE] 🔴 all bonds invalidated by the key change — wiping "
                   "the bond table; the app must re-pair");
    notifyStatus("PROVISION_KEY_OK"); // answer BEFORE the bond goes away
    delay(150);                        // let the notify flush (see notifyStatus)
    // Every slot, INCLUDING bond #0 — the owner's secret was derived from the
    // old private key too, so it is just as dead as the members'. Starting at
    // 1 here (the usual "never touch slot 0" rule) would leave exactly the bond
    // the app is holding, and it would fail on the next envelope.
    for (int i = 0; i < OZ_BOND_MAX; i++)
      if (g_bonds[i].present) ozBondRevoke(i);
    screenDirty = true;
    return;
  }

  if (strcmp(kind, "set_name") == 0) {
    if (slot != 0) {
      Serial.printf("[OZKIE] set_name REFUSED — bond %d is not the owner\n", slot);
      notifyStatus("SETTING_DENIED");
      return;
    }
    // asciiOnly() because the panel font has no glyphs beyond ASCII and a
    // multi-byte name would render as mojibake on the one screen this exists
    // to serve. Clamped so a hostile or careless name cannot fill NVS.
    String n = asciiOnly(String((const char *)(doc["name"] | "")));
    n.trim();
    if (n.length() > 24) n = n.substring(0, 24);
    if (!n.length()) {
      Serial.println("[OZKIE] set_name: empty name — refused");
      notifyStatus("SETTING_DENIED");
      return;
    }
    cfgName = n;
    saveConfig();
    Serial.printf("[OZKIE] set_name -> '%s' (persisted)\n", cfgName.c_str());
    screenDirty = true; // the whole point: the panel changes now, not at reboot
    // …and so does info.name. The rename epic reported through a field that
    // could not change until reboot; an app re-reading INFO after a successful
    // rename would have been told the OLD name and had no way to know why.
    ozRefreshInfoChar();
    notifyStatus("SETTING_OK");
    return;
  }

  if (strcmp(kind, "bond_revoke") == 0) {
    uint8_t pub[32];
    if (ozHexDecode(String((const char *)(doc["pub"] | "")), pub, sizeof(pub)) != 32) {
      Serial.println("[OZKIE] bond_revoke: `pub` is not 32 bytes of hex");
      notifyStatus("REVOKE_DENIED");
      return;
    }
    handleBondRevoke(slot, pub, 32);
    return;
  }
  if (strcmp(kind, "invite_cancel") == 0) {
    uint8_t nonce[16];
    if (ozHexDecode(String((const char *)(doc["nonce"] | "")), nonce, sizeof(nonce)) != 16) {
      Serial.println("[OZKIE] invite_cancel: `nonce` is not 16 bytes of hex");
      notifyStatus("REVOKE_DENIED");
      return;
    }
    handleInviteCancel(slot, nonce, 16);
    return;
  }
  if (strcmp(kind, "list_bonds") == 0) {
    handleListBonds(slot, nullptr, 0);
    return;
  }

  // ── Q1 queries — answered over the UPLINK, not over BLE ────────────────────
  // Deliberate: the entire point of a query is to ask WITHOUT being present.
  // A caller standing at the door already has BLE `list_bonds`. Answering
  // remote questions on a transport that requires proximity would make the
  // feature useless for the case it exists to serve (an admin phone that is
  // nowhere near the lock — the exact situation that produced XF-75/77/78).
  //
  // `msg_id` is echoed INSIDE the seal. Putting a correlator in the clear
  // would let anyone watching the broker link "admin asked" to "lock
  // answered" and infer who administers which door and when — traffic
  // analysis, without decrypting anything. The app can decrypt, so it
  // correlates perfectly well from inside; no middle hop needs it.
  // ── ozkey-29 §11.4 — query_events: pull the sealed audit backlog ──────────
  //
  // Deliberately shaped as a CURSOR, not "send me everything": an app that has
  // been away for a month must not need one 10,000-record response, and a
  // repeated request with the same `since_seq` must return the same records
  // (ozkey-27 §4.4 R1 — idempotent verbs, so a lost reply is free to retry).
  //
  // `dropped_before_seq` is the honest half and the reason this verb exists at
  // all rather than the app just reading a log. Rotation destroys 5,000 records
  // at a time; without this the app cannot distinguish a complete history from
  // one with a hole, and ozkey-29's whole claim is that the APP holds the
  // complete history (§10.5). A log that quietly forgets cannot back that
  // claim, so we state the gap rather than paper over it.
  //
  // 🔴 CORRECTED 2026-08-15. This comment used to claim the response "rides the
  // existing sealed notify path ... That is §5.1's 'sealed before it ever
  // leaves'." IT DID NOT — it called ozNotifyChunked() on the raw JSON, putting
  // the entire door history in clear over BLE. It now genuinely seals, via
  // ozNotifySealedTo(); see that function's header for why the claim being
  // WRITTEN DOWN made it worse than an open gap.
  if (strcmp(kind, "query_events") == 0) {
    if (!ozQueryRateOk(slot)) { notifyStatus("QUERY_THROTTLED"); return; }
    if (g_bonds[slot].role != OZ_ROLE_ADMIN) {
      Serial.printf("[OZKIE] query_events is admin-only — bond %d denied\n", slot);
      notifyStatus("QUERY_DENIED");
      return;
    }
    const uint32_t sinceSeq = doc["since_seq"] | 0u;
    JsonDocument rsp;
    rsp["kind"] = "events_response";
    rsp["msg_id"] = (const char *)(doc["msg_id"] | "");
    rsp["seq_to"] = g_evtSeq;
    // Present ONLY when records are genuinely gone. Absent means "the history
    // you are reading is complete from since_seq" — a positive statement the
    // app can rely on, which is only safe because rotation records the truth.
    if (g_evtDroppedBefore) rsp["dropped_before_seq"] = g_evtDroppedBefore;

    JsonArray arr = rsp["events"].to<JsonArray>();
    uint32_t first = 0, last = 0;
    bool truncated = false;
    if (fsUp) {
      for (const char *path : {"/txlog.1", "/txlog.0"}) {
        if (truncated || !LittleFS.exists(path)) continue;
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        while (f.available()) {
          String line = f.readStringUntil('\n');
          if (!line.length()) continue;
          JsonDocument rec;
          if (deserializeJson(rec, line) != DeserializationError::Ok) continue;
          const uint32_t sq = rec["seq"] | 0u;
          if (sq <= sinceSeq) continue; // already delivered — the cursor
          // Budget-bounded, same discipline as query_roster: a BLE notify has a
          // finite MTU and an oversized reply is worse than a short one the app
          // can simply ask again from.
          if (measureJson(rsp) > OZ_QUERY_PLAINTEXT_BUDGET) { truncated = true; break; }
          arr.add(rec);
          if (!first) first = sq;
          last = sq;
        }
        f.close();
      }
    }
    if (first) rsp["seq_from"] = first;
    if (last) rsp["seq_to"] = last;   // what THIS response actually carries
    if (truncated) rsp["more"] = true; // ask again with since_seq = seq_to
    rsp["seq_head"] = g_evtSeq;        // how far the lock has got overall

    Serial.printf("[EVT] query_events since=%lu -> %u records (%lu..%lu)%s\n",
                  (unsigned long)sinceSeq, arr.size(),
                  (unsigned long)first, (unsigned long)last,
                  truncated ? " MORE" : "");
    String out;
    serializeJson(rsp, out);
    // Fails CLOSED: if it cannot be sealed it is not sent. An audit history is
    // exactly the payload where "send it anyway, unencrypted" is the wrong
    // fallback — the app can retry, a leaked history cannot be recalled.
    if (!ozNotifySealedTo(slot, out)) {
      Serial.println("[EVT] events_response could NOT be sealed — refusing to "
                     "send it in the clear");
      notifyStatus("QUERY_DENIED");
    }
    return;
  }

  if (strcmp(kind, "query_roster") == 0 || strcmp(kind, "query_bond_state") == 0) {
    if (!ozQueryRateOk(slot)) { notifyStatus("QUERY_THROTTLED"); return; }
    if (g_bonds[slot].role != OZ_ROLE_ADMIN) {
      Serial.printf("[OZKIE] %s is admin-only — bond %d denied\n", kind, slot);
      notifyStatus("QUERY_DENIED");
      return;
    }
    const char *msgId = doc["msg_id"] | "";

    JsonDocument rsp;
    if (strcmp(kind, "query_roster") == 0) {
      rsp["kind"] = "roster_response";
      rsp["msg_id"] = msgId;
      rsp["bonds"] = ozBondCount();
      // R5: the pull path must answer with the epoch too, or an app that
      // resyncs after a missed push has no way to record what it just caught up
      // TO — and would resync forever.
      rsp["roster_epoch"] = g_rosterEpoch;
      JsonArray arr = rsp["members"].to<JsonArray>();
      bool truncated = false;
      for (int i = 1; i < OZ_BOND_MAX; i++) {
        if (!g_bonds[i].present) continue;
        if (measureJson(rsp) > OZ_QUERY_PLAINTEXT_BUDGET) { truncated = true; break; }
        JsonObject o = arr.add<JsonObject>();
        char h[65];
        ozHex(g_bonds[i].pub, 32, h);
        h[16] = 0; // 8-byte prefix — enough to match a row the app already has,
                   // and the full value is available over BLE when present
        o["slot"] = i;
        o["label"] = g_bonds[i].label;
        o["pub8"] = h;
      }
      if (truncated) rsp["truncated"] = true;
    } else {
      uint8_t pub[32];
      if (ozHexDecode(String((const char *)(doc["pub"] | "")), pub, sizeof(pub)) != 32) {
        Serial.println("[OZKIE] query_bond_state: `pub` is not 32 bytes of hex");
        notifyStatus("QUERY_DENIED");
        return;
      }
      const int target = ozBondFind(pub);
      rsp["kind"] = "bond_state_response";
      rsp["msg_id"] = msgId;
      rsp["present"] = (target >= 0);
      if (target >= 0) {
        rsp["slot"] = target;
        rsp["label"] = g_bonds[target].label;
        rsp["role"] = (g_bonds[target].role == OZ_ROLE_ADMIN) ? "admin" : "member";
      }
    }

    String out;
    serializeJson(rsp, out);
    Serial.printf("[OZKIE] %s -> %u B response to bond %d\n", kind,
                  (unsigned)out.length(), slot);
    if (!ozUplinkSend(slot, out)) notifyStatus("QUERY_UNDELIVERABLE");
    return;
  }

  // ── MCU verbs — decide the DP, build the value, then build the frame.
  uint8_t dp = 0, type = 0;
  uint8_t val[OZ_SEM_VAL_MAX];
  size_t vlen = 0;

  // ── "is this an unlock" is a SEMANTIC question, not a DP number ──────────
  //
  // The gates below used to ask `dp != 1`, which was only ever right because
  // unlock happened to be DP 1 — our own fiction. The moment unlock can also
  // be DP 76, that test silently starts denying MEMBERS the one verb they are
  // allowed, and makes the latency-critical verb wait for an MCU ack. Ask the
  // real question instead.
  const bool isUnlock = (strcmp(kind, "unlock") == 0);

  if (isUnlock) {
    // ── DP 76 `unlock_ble` — the REAL command, when this is a BLE unlock ────
    //
    // Supplier's instruction table: DP 76 = 0x4c, issuable by the MODULE under
    // 0x06, type 0x04 VALUE, 4 bytes, range 0..99999 — a FULLY SPECIFIED
    // payload, unlike DP 10 `remote_unlock` whose layout is "0x00-0xff" and
    // therefore unimplementable (ozkey-27 Q2). The T3 module doc names the
    // intent outright: "To enable Bluetooth lock control when the device is
    // offline, select DP76 - unlock_ble."
    //
    // 🔴 NO LONGER GATED ON viaBle — operator's call, 2026-08-20.
    //
    // It used to be `viaBle && ozDpFind(76)`, so a network unlock fell through
    // to fiction DP 1. The reasoning was audit honesty: DP 76 asserts the door
    // was opened over Bluetooth, and a lock's record should not claim BLE for a
    // command that arrived over Thread.
    //
    // That reasoning was right about the audit and wrong about the priority.
    // Measured on the bench today: with a REAL supplier profile
    // (`tuya-ds013-t3`) loaded, DP 1 is not in the product at all, so a network
    // unlock is silently discarded — server reports `delivered`, app reports
    // success, door stays shut. We were protecting the accuracy of a log entry
    // for an event that could no longer happen.
    //
    // 🔴 THE TRANSPORT WAS NEVER THE PROBLEM. The door opens on the last hop —
    // module -> DL-MCU over the Tuya UART — and BLE/Thread/MQTT only decide how
    // the request reached US. Making the DP depend on the transport was our own
    // invention; the supplier's table says DP 76 is issuable by the module and
    // says nothing about how the request arrived. DP 10 `remote_unlock` is the
    // nominal network equivalent and is unimplementable (`0x00-0xff`, no
    // layout — ozkey-42 §2.2).
    //
    // WHAT WE GIVE UP: the MCU's OWN access record may now say "ble" for a
    // network-originated unlock. We keep the truth where it matters — our
    // txlog and `event.access` record the real actor, bond slot and transport,
    // and those are the records the app and server actually read. The MCU's
    // internal log is one we neither own nor consume.
    //
    // 🔴 RESOLVED FROM DATA, NOT BRANCHED IN C (PM directive 2026-08-20).
    //
    // This used to be `if (ozDpFind(76)) dp = 76; else dp = 1;` — two DP
    // numbers compiled into logic. A supplier whose unlock DP was neither would
    // have needed a firmware change, which made "profile-driven" aspirational,
    // and it is how DP 1 survived long enough to reach real hardware.
    //
    // No field is named, so the resolver returns the best-status candidate for
    // `lock.unlock`: DP 76 on a real profile, DP 10 never (RESERVED, sorted
    // after), DP 1 on the legacy map where it is genuinely correct.
    //
    // cred_id = the bond slot that authorised it, so the MCU's own access
    // record names WHICH credential opened the door. DP 1 could never say.
    const OzVerbMap *m = ozResolveVerb("lock.unlock", nullptr, OZ_DIR_DOWN);
    if (!m) {
      // This product has no unlock command at all. Say so — never substitute
      // something that looks close, which is precisely how DP 1 happened.
      Serial.printf("[OZKIE] unlock: profile '%s' has no lock.unlock command "
                    "— refusing\n", ozProfileId());
      ozReportOutcome(slot, "UNLOCK_UNSUPPORTED",
                      String("profile ") + ozProfileId() + " has no unlock DP");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    if (!ozVerbUsable(m)) {
      // Known DP, unusable payload — DP 10's layout was never supplied. That is
      // a different answer from "unknown", and the app deserves to hear which.
      Serial.printf("[OZKIE] unlock: DP %u is not usable (status %u) on '%s' "
                    "— refusing\n", (unsigned)m->dp, (unsigned)m->status,
                    ozProfileId());
      ozReportOutcome(slot, "UNLOCK_UNSUPPORTED",
                      String("DP ") + m->dp + " payload layout not supplied");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    dp = m->dp;
    type = m->type;
    if (type == 0x02 /* VALUE */) {
      const uint32_t credId = (uint32_t)slot;
      val[0] = (uint8_t)(credId >> 24); val[1] = (uint8_t)(credId >> 16);
      val[2] = (uint8_t)(credId >> 8);  val[3] = (uint8_t)(credId & 0xFF);
      vlen = 4;
    } else {
      // BOOL — the legacy map's DP 1, whose value is simply "open". `dp` and
      // `type` already came from the resolver; do NOT reassign them here, or
      // the table stops being the source of truth for the one case it still
      // governs.
      val[0] = 0x01; vlen = 1;
    }
  } else if (strcmp(kind, "grant_pin") == 0 || strcmp(kind, "grant_rfid") == 0) {
    // 🔴 RESOLVED, NOT HARDCODED — and this one is a safety fix, not tidiness.
    //
    // This used to be `dp = grant_pin ? 21 : 23`, unconditionally. On the real
    // supplier map DP 21 is `navigation_volume` and DP 23 is `auto_lock`, and
    // both ARE selected by the profile — so nothing rejected them. Issuing a
    // PIN against a real DS013-T3 wrote the credential onto the VOLUME
    // CONTROL, with both ends reporting success (ozkey-42 §2.4.1, demonstrated
    // on the bench 2026-08-20).
    //
    // The resolver returns the real credential DPs (16 for a PIN, 13 for RFID),
    // both RESERVED because the supplier has never supplied their payload
    // layout — so the grant is now REFUSED with a reason instead of silently
    // misfiring. Refusing is the correct behaviour until ozkey-42 P0 lands.
    const bool isPin = (strcmp(kind, "grant_pin") == 0);
    const OzVerbMap *g = ozResolveVerb("cred.put", isPin ? "pin" : "rfid",
                                       OZ_DIR_DOWN);
    if (!g) {
      Serial.printf("[OZKIE] %s: profile '%s' has no cred.put — refusing\n",
                    kind, ozProfileId());
      ozReportOutcome(slot, "CRED_UNSUPPORTED",
                      String("profile ") + ozProfileId() + " cannot store credentials");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    if (!ozVerbUsable(g)) {
      Serial.printf("[OZKIE] %s: DP %u payload layout not supplied "
                    "(ozkey-42 P0) — refusing rather than misfiring\n",
                    kind, (unsigned)g->dp);
      ozReportOutcome(slot, "CRED_UNSUPPORTED",
                      String("DP ") + g->dp + " payload layout not supplied");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    dp = g->dp;
    type = g->type;
    vlen = ozSemGrantValue(doc, isPin, val, sizeof(val));
    if (vlen == 0) {
      Serial.printf("[OZKIE] %s: bad or missing slot/cred/from/to\n", kind);
      notifyStatus("UNLOCK_DENIED");
      return;
    }
  } else if (strcmp(kind, "delete_pin") == 0 || strcmp(kind, "delete_rfid") == 0) {
    dp = (strcmp(kind, "delete_pin") == 0) ? 22 : 24;
    type = 0x00 /* RAW */;
    const uint32_t slotNo = doc["slot"] | 0xFFFFFFFFu;
    if (slotNo > 0xFFFF) {
      Serial.printf("[OZKIE] %s: bad or missing `slot`\n", kind);
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    val[0] = (uint8_t)(slotNo >> 8); val[1] = (uint8_t)(slotNo & 0xFF); vlen = 2;
  } else {
    Serial.printf("[OZKIE] unknown kind '%s' — rejected, NOT forwarded\n", kind);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  // Same admin bar the DP path applies (ozkey-13 F4): a member may unlock the
  // door they're standing at, but must never issue or delete a credential.
  if (!isUnlock && g_bonds[slot].role != OZ_ROLE_ADMIN) {
    Serial.printf("[OZKIE] %s is role-gated to bond #0 — bond %d ('%s', member) "
                  "denied\n", kind, slot, g_bonds[slot].label);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  uint8_t frame[OZ_SEM_VAL_MAX + 16];
  const size_t flen = ozBuildDpFrame(dp, type, val, vlen, frame);

  // Validate our OWN output with the same gate inbound frames pass. If this
  // ever trips it is a bug in the builder above, not in anything the sender
  // did — and a malformed frame must never reach the MCU regardless of which
  // side authored it.
  if (!ozTuyaFrameOk(frame, flen)) {
    Serial.printf("[OZKIE] BUG: built an invalid DP %u frame (%u B) — not sent\n",
                  dp, (unsigned)flen);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  Serial.printf("[OZKIE] %s -> DP %u authorised by bond %d ('%s', %s)\n", kind, dp,
                slot, g_bonds[slot].label,
                g_bonds[slot].role == OZ_ROLE_ADMIN ? "admin" : "member");
  forwardFrameToMcu(frame, flen);

  // 1.61 — a CREDENTIAL operation must be confirmed by the MCU before we call
  // it a success. An unlock is exempt: its proof is the bolt, and it is the one
  // latency-critical verb here (see ozAwaitMcuAck's header).
  if (!isUnlock && !ozAwaitMcuAck(dp)) {
    // Distinct from UNLOCK_DENIED, which means "I refused you". This means "I
    // accepted you and cannot confirm the lock stored it" — a different thing
    // for an app to show a user, and previously indistinguishable from success.
    publishLog("mcu_timeout", "OZKIE credential — MCU did not confirm");
    ozReportOutcome(slot, "MCU_TIMEOUT",
                    String("DP ") + dp + " written, MCU did not echo");
    return;
  }

  // 🔴 `isUnlock`, NOT `dp == 1` (2026-08-20). The old test was the same trap
  // this function's own header warns about 20 lines up: it read correctly only
  // while unlock WAS DP 1. Now that an unlock on a real profile is DP 76, that
  // test would have logged every owner unlock as "credential" — a mislabelled
  // audit line, which is worse than a missing one because it is believed.
  //
  // `via` is recorded because we no longer choose the DP by transport: DP 76
  // asserts "opened over BLE" to the MCU whatever the truth was, so OUR record
  // is now the only place the real transport survives. This is the log the app
  // and server actually read; the MCU's internal one we neither own nor
  // consume. See the DP-selection note above.
  char gdetail[72];
  snprintf(gdetail, sizeof(gdetail), "OZKIE %s (%s, via %s)",
           isUnlock ? "unlock" : "credential", slot == 0 ? "owner" : "member",
           viaBle ? "ble" : "net");
  publishLog("granted", gdetail, slot);
  notifyStatus("UNLOCK_OK");
}

// ── F1 (ozkey-13 §8): the open/verify core, shared between BLE `control` and
// the MQTT `envelope_hex` path added in F2. Everything through "envelope
// opened, here is the plaintext + counter" is identical for both transports —
// same bond lookup, same key derivation, same AES-GCM open. What differs is
// what happens AFTER: BLE requires a live per-connection challenge prefix
// (Gate 3, XF-47, unconditional on that transport); a queued/remote MQTT
// command has no live connection to have issued one over, so its freshness is
// counter-only (ozkey-13 §5 — confirmed acceptable, documented as honestly
// weaker than BLE's, not hidden). `ozControlOpen` stays transport-agnostic;
// `ozControlVerifyAndDispatch` takes `hasChallenge` to select the wire shape.

// Try to open `app_id_hex(64) ‖ envelope` into a plaintext frame. Does not
// consume any buffer or notify status itself — the caller does both once it
// knows the full verdict, since only the caller knows whether "envelope did
// not open" means forged (MQTT: always) or possibly-still-arriving (BLE only,
// mid-chunk).
static OzCtlOpen ozControlOpen(const uint8_t *buf, size_t n, int *outSlot,
                                uint8_t *pt, size_t ptCap, size_t *outPtLen,
                                uint64_t *outCounter) {
  // ── 🔴 1.65 — BARE ENVELOPE (the server path). ROOT CAUSE OF A FULL DAY. ──
  //
  // This function was written for the BLE `control` characteristic, where the
  // app writes `appIdHex(64 ASCII) ‖ envelope` — the prefix is how we learn
  // WHICH bond to derive the key from. The length gate below therefore demands
  // 64 + OZ_ENV_MIN bytes.
  //
  // Nothing on the SERVER path ever carries that prefix. `Keyring._seal()`
  // returns the bare envelope; the app prepends the app_id only at the BLE
  // write site; `ozlockserv` relays `envelope_hex` verbatim. So a sealed
  // command arriving over MQTT/Thread is ~62 bytes, fails `n < 101` on the
  // first line, and is refused before a single crypto operation runs.
  //
  // Every remote grant this lock has ever been sent failed here. It was
  // invisible because the refusal went to notifyStatus(), which is BLE-only,
  // on a path that by definition has no BLE client (fixed in 1.63/1.64 — that
  // is how we finally read the reason instead of inferring it from blob sizes).
  //
  // WHY TRY-ALL-BONDS RATHER THAN ADD A PREFIX OR A FIELD. Requiring the server
  // or the app to send the app_id would work, but it means a contract change
  // agreed across three teams to carry a value the lock can simply discover.
  // There are at most OZ_BOND_MAX bonds; AES-GCM's tag makes a wrong key fail
  // safely and unambiguously, so "try each" is a search, not a weakening — the
  // authentication is identical either way. It also fixes every sealed verb on
  // every remote transport at once, with no app release and no server release.
  //
  // The two forms cannot be confused: a bare envelope begins with OZ_ENV_VER
  // (0x02), which is not a printable character and therefore can never be the
  // first byte of a 64-char ASCII hex app_id.
  if (n >= OZ_ENV_MIN && buf[0] == OZ_ENV_VER) {
    char selfHex[65];
    for (int i = 0; i < OZ_BOND_MAX; i++) {
      if (!g_bonds[i].present) continue;
      ozHex(g_bonds[i].pub, 32, selfHex);
      uint8_t ps2[32], k2[32];
      const bool have = ozBondSecret(i, ps2) &&
                        ozEnvKey(ps2, 32, deviceId, String(selfHex), true, k2);
      memset(ps2, 0, sizeof(ps2));
      if (!have) { memset(k2, 0, sizeof(k2)); continue; }
      uint64_t c2 = 0;
      const int len2 = ozEnvOpen(k2, deviceId, buf, n, pt, ptCap, &c2);
      memset(k2, 0, sizeof(k2));
      if (len2 > 0) {
        *outSlot = i;
        *outPtLen = (size_t)len2;
        *outCounter = c2;
        Serial.printf("[CTL] bare envelope opened under bond %d (%.16s…)\n", i, selfHex);
        return OZCTL_OPENED;
      }
    }
    Serial.printf("[CTL] bare envelope (%u B) opened by NO bond of %d\n",
                  (unsigned)n, ozBondCount());
    return OZCTL_FAILED_DEFINITE;
  }

  if (n < 64 + OZ_ENV_MIN) return OZCTL_FAILED_DEFINITE;

  char appIdHex[65];
  memcpy(appIdHex, buf, 64);
  appIdHex[64] = 0;
  if (!ozIsHex(appIdHex, 32)) {
    Serial.println("[CTL] leading 64 bytes are not an app_id hex string");
    return OZCTL_FAILED_DEFINITE;
  }
  uint8_t senderPub[32];
  ozFromHex(appIdHex, senderPub, 32);
  const int slot = ozBondFind(senderPub);
  if (slot < 0) {
    Serial.printf("[CTL] %.16s… holds no bond on this lock\n", appIdHex);
    // XF-114 §10.3 — distinct from a malformed/forged frame. See the OzCtlOpen
    // enum: for a REMOVE this is not a refusal, it is "already done".
    return OZCTL_FAILED_NO_BOND;
  }

  uint8_t ps[32], key[32];
  const bool haveKey =
      ozBondSecret(slot, ps) &&
      ozEnvKey(ps, 32, deviceId, String(appIdHex), true /*app->lock*/, key);
  memset(ps, 0, sizeof(ps));
  if (!haveKey) {
    Serial.println("[CTL] could not derive the bond's app->lock key");
    memset(key, 0, sizeof(key));
    return OZCTL_FAILED_DEFINITE;
  }

  uint64_t counter = 0;
  const int ptLen = ozEnvOpen(key, deviceId, buf + 64, n - 64, pt, ptCap, &counter);
  memset(key, 0, sizeof(key));
  if (ptLen < 0) {
    Serial.printf("[CTL] envelope did not open (%u B, bond %d)\n",
                  (unsigned)(n - 64), slot);
    return OZCTL_FAILED_MAYBE_INCOMPLETE;
  }

  *outSlot = slot;
  *outPtLen = (size_t)ptLen;
  *outCounter = counter;
  return OZCTL_OPENED;
}

// Given an OPENED plaintext (past ozControlOpen), verify freshness and
// execute. `hasChallenge` selects whether the first 16 bytes of `pt` are a
// live challenge to check-and-strip (BLE) or the frame starts at byte 0
// (MQTT — see the block comment above).
static void ozControlVerifyAndDispatch(int slot, uint8_t *pt, size_t ptLen,
                                        uint64_t counter, bool hasChallenge,
                                        bool viaBle) {
  // ── ozkey-21 T5: expiry is checked ON THE COMMAND PATH, not only on a timer.
  //
  // The 15 s sweep alone would leave a window in which an expired member could
  // still be served — small, but this is the verb that opens a door, and "we
  // would have removed you 14 seconds from now" is not an access control. Sweep
  // first, then confirm the caller's own bond survived it.
  //
  // ozControlOpen() resolved `slot` by matching the sender's pubkey BEFORE this
  // ran, so the slot it handed us may be the very bond the sweep just deleted.
  // Re-checking `present` is what makes an expired member's own unlock fail
  // rather than execute against a memset slot.
  ozBondExpirySweep();
  if (slot < 0 || slot >= OZ_BOND_MAX || !g_bonds[slot].present) {
    Serial.printf("[CTL] bond %d no longer present (expired or revoked) — denied\n",
                  slot);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  const size_t need = (hasChallenge ? 16u : 0u) + 11u; // 11 = smallest legal DP frame
  if (ptLen < need) {
    Serial.printf("[CTL] plaintext is %u B — too short for %s\n",
                  (unsigned)ptLen,
                  hasChallenge ? "challenge + frame" : "a frame");
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  if (hasChallenge) {
    // Gate 3 — UNCONDITIONAL, every verb, no exceptions (XF-47).
    if (!bleChallengeValid || !ozCtEq(pt, bleChallenge, 16)) {
      Serial.printf("[CTL] challenge %s — denied\n",
                    bleChallengeValid ? "does not match the one issued on this "
                                        "connection"
                                      : "was never read on this connection");
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    // Burn it. The app reads …0005 before every control write, so requiring a
    // fresh one costs nothing — and without this, one challenge read would
    // authorise every frame sent for the rest of the connection.
    bleChallengeValid = false;
  }

  // Gate 4 — strictly greater. Equal is a replay of the frame we just accepted.
  if (counter <= g_bonds[slot].floor) {
    Serial.printf("[CTL] counter %llu is not above bond %d's floor %llu — replay\n",
                  (unsigned long long)counter, slot,
                  (unsigned long long)g_bonds[slot].floor);
    // Carry BOTH numbers. A replay refusal is indistinguishable from a lost
    // datagram unless the sender can see that its counter is behind — and if
    // the app has been reinstalled its counter legitimately restarts below a
    // floor the lock still remembers, which is a recoverable condition nobody
    // can diagnose from silence.
    char why[80];
    snprintf(why, sizeof(why), "counter %llu <= bond %d floor %llu (replay)",
             (unsigned long long)counter, slot,
             (unsigned long long)g_bonds[slot].floor);
    ozReportOutcome(slot, "COUNTER_REPLAY", why);
    return;
  }

  const uint8_t *body = pt + (hasChallenge ? 16 : 0);
  const size_t blen = ptLen - (hasChallenge ? 16 : 0);

  // ozkey-17 F8: the plaintext is either OZKIE semantic JSON (the protocol from
  // here on) or a legacy Tuya DP frame (pre-F8 senders). They are unambiguous
  // on the first byte — '{' vs 0x55 — so no version field is needed to tell
  // them apart, and neither can be mistaken for the other by accident.
  //
  // Dual-accept is deliberate and mirrors the discipline the server team
  // applied to buildUnlockFrame(): keep the legacy path working until the app's
  // sealed semantic sender is confirmed shipped, THEN delete it. Cutting the
  // old path before the new one is live just breaks unlock for everyone.
  const bool semantic = (body[0] == '{');
  if (!semantic && !ozTuyaFrameOk(body, blen)) {
    Serial.printf("[CTL] plaintext is authentic but is neither OZKIE JSON nor a "
                  "valid DP frame (%u B)\n", (unsigned)blen);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  // Advance the floor BEFORE executing. If execution reboots the lock (or the
  // MCU write hangs), the frame must not become replayable on the way back up —
  // an unlock that maybe happened is a far better outcome than one that can be
  // repeated by anyone who captured it.
  g_bonds[slot].floor = counter;
  ozBondsSave();

  if (semantic) {
    Serial.printf("[CTL] OPENED — bond %d, counter %llu, OZKIE (%u B)\n", slot,
                  (unsigned long long)counter, (unsigned)blen);
    ozSemanticDispatch(slot, (const char *)body, blen, viaBle);
  } else {
    Serial.printf("[CTL] OPENED — bond %d, counter %llu, DP %u (legacy frame)\n",
                  slot, (unsigned long long)counter, body[6]);
    ozControlDispatch(slot, body, blen);
  }
}

// Try to open whatever is buffered on the BLE `control` characteristic.
// [final] means the idle timer fired and no more bytes are coming, so an
// unopenable buffer must be answered rather than left to accumulate. Returns
// true when the buffer was consumed either way.
static bool ozControlTry(bool final) {
  // Snapshot the length once. The BLE task may append while we work; taking the
  // value once means we open a consistent prefix rather than a buffer that grew
  // underneath the AAD. A late chunk simply means this attempt fails to open and
  // the next pass tries again with the longer buffer.
  size_t n;
  portENTER_CRITICAL(&ctlMux);
  n = ctlLen;
  portEXIT_CRITICAL(&ctlMux);

  if (n < 64 + OZ_ENV_MIN) {
    if (!final) return false;
    Serial.printf("[CTL] %u B is too short to be a control message\n",
                  (unsigned)n);
    ctlConsume(n);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  int slot = -1;
  uint8_t pt[OZ_CTL_MAX];
  size_t ptLen = 0;
  uint64_t counter = 0;
  const OzCtlOpen r =
      ozControlOpen(ctlBuf, n, &slot, pt, sizeof(pt), &ptLen, &counter);

  // Indistinguishable from here: still arriving, or forged. Keep waiting
  // unless the idle timer already gave up on it.
  if (r == OZCTL_FAILED_MAYBE_INCOMPLETE && !final) return false;

  // Opened or definitively failed either way: the buffer is spent.
  ctlConsume(n);

  if (r != OZCTL_OPENED) {
    // XF-114 §10.3 — "I do not know you" is not "I refuse you". The app needs
    // to tell an already-unowned lock (remove locally, you have won) from a
    // rejected frame (stop), and until 1.95 both arrived as UNLOCK_DENIED.
    notifyStatus(r == OZCTL_FAILED_NO_BOND ? "NO_BOND" : "UNLOCK_DENIED");
    return true;
  }

  ozControlVerifyAndDispatch(slot, pt, ptLen, counter, true /*hasChallenge*/,
                             true /*viaBle — the BLE control characteristic*/);
  return true;
}

// APPEND ONLY. Everything that interprets these bytes runs in loop() — see the
// ctlBuf comment for why. getData()/getLength(), never getValue().c_str(): the
// payload is binary and a c_str() copy would truncate the whole message at the
// first zero byte in the ciphertext, which one in 256 of them starts with.
class ControlCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const uint8_t *data = c->getData();
    const size_t n = c->getLength();
    if (!data || !n) return;

    bool overflow = false;
    portENTER_CRITICAL(&ctlMux);
    if (ctlLen + n > sizeof(ctlBuf)) {
      overflow = true;
      ctlLen = 0;
    } else {
      memcpy(ctlBuf + ctlLen, data, n);
      ctlLen += n;
    }
    portEXIT_CRITICAL(&ctlMux);

    if (overflow) {
      Serial.printf("[CTL] message exceeds %u B — dropped\n",
                    (unsigned)sizeof(ctlBuf));
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    ctlLastChunkAt = millis();
    ctlNewBytes = true;
  }
};

// M3 `challenge` …0005 — fresh 16 bytes on EVERY read. Generating in the read
// callback rather than at connect time is what makes it fresh: two unlocks on
// one connection must not share a challenge, or the second is replayable.
class ChallengeCB : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    esp_fill_random(bleChallenge, sizeof(bleChallenge));
    bleChallengeValid = true;
    c->setValue(bleChallenge, sizeof(bleChallenge));
    Serial.println("[CHAL] issued a fresh 16-byte challenge");
  }
};

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

// ── M3 busy flag (XF-47 "Single connection, and the busy flag") ──────────────
//
// One byte of service data in the SCAN RESPONSE, bit 0 = a connection is up.
// Scan response, not the ADV packet: ADV is already at 29 of 31 bytes (flags 3
// + complete-128-bit-UUID-list 18 + name 8), and BANOI's Android `withServices`
// filter keys off that 0x07 UUID list — displacing it would break discovery
// before their listener ever ran. Costs an active scan on the app side.
//
// While connected we keep advertising but switch to SCAN_IND (scannable,
// NON-connectable). That is what enforces "max 1 connection, the in-progress
// one is kept": a second phone can still SEE the lock and read busy=1, but the
// link layer refuses it, so a half-finished enrolment can never be aborted by
// someone else connecting. Refusing at the link layer is indistinguishable from
// out-of-range, which is exactly why the flag has to be observable.
// ⚠ THIS FUNCTION RESTARTS ADVERTISING. It does not look like it does, and that
// cost an entire bench session (2026-08-04/05) — the lock accepted exactly ONE
// BLE connection per boot and was discoverable-but-unconnectable ever after.
//
// setScanResponseData() calls esp_ble_gap_config_scan_rsp_data_raw(), which is
// ASYNCHRONOUS. Its completion event is handled inside the Arduino BLE library
// (BLEAdvertising.cpp, ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT) and that
// handler does:
//
//     m_advertisingPending = true;
//     esp_ble_gap_start_advertising(&m_advParams);
//
// So it re-starts advertising using whatever `m_advParams.adv_type` holds AT
// COMPLETION TIME — not at call time. The old comment here claimed the byte
// could be updated "without a stop/start cycle", which is exactly backwards.
//
// CALLERS MUST SET THE ADVERTISEMENT TYPE FIRST — which is why the only caller
// is bleRearmAdvertising() below. Reversed, the queued restart fires with the
// PREVIOUS type and the later setAdvertisementType() only edits a struct field
// that nothing re-applies, because advertising is already running.
void bleSetBusy(bool busy) {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (adv == nullptr) return;
  BLEAdvertisementData scanResp;
  String sd;
  sd += (char)(busy ? 0x01 : 0x00);

  // ── WHICH LOCK IS THIS? (operator, 2026-08-12) ──────────────────────────
  //
  // Every lock advertises the identical local name "OZLOCK", so an app
  // scanning three locks shows three indistinguishable rows and can only tell
  // them apart AFTER connecting and reading info.device_id — by which point it
  // has already picked one.
  //
  // ⚠ THE ONE THING THAT CANNOT WORK: matching on the identifier iOS shows
  // (e.g. "EBD686A3-17DC-…"). That is a CoreBluetooth peripheral UUID —
  // generated by the PHONE, different on every phone, never transmitted over
  // the air. The lock cannot know it, so no LCD change could ever display it.
  // The identifier has to be one the LOCK owns and broadcasts. That is this.
  //
  // 4 bytes of MAC = the same 8 hex characters the panel prints via
  // ozShortId() ("ozk-acebe639"), so what is on the screen and what is in the
  // scan response are the same string by construction, not by convention.
  //
  // Goes in the SCAN RESPONSE, not the advertisement: the ADV is already at
  // 29 of 31 bytes (flags 3 + 128-bit UUID list 18 + name 8) and the 128-bit
  // UUID must stay there because BANOI's Android `withServices` filter keys off
  // it — displacing it breaks discovery before their listener runs. Scan
  // response is 19/31 used, so this fits at 23/31.
  //
  // Requires the app to scan ACTIVELY (scan response is only returned to a
  // scan request) — the same requirement the busy byte above already has.
  // Derived from deviceId's own hex rather than re-reading the MAC, so the
  // broadcast bytes and the printed string cannot drift apart.
  const int dash = deviceId.indexOf('-');
  for (int i = 0; i < 4 && dash >= 0; i++) {
    const int hi = hexNibble(deviceId.charAt(dash + 1 + i * 2));
    const int lo = hexNibble(deviceId.charAt(dash + 2 + i * 2));
    if (hi < 0 || lo < 0) break;
    sd += (char)((hi << 4) | lo);
  }

  scanResp.setServiceData(BLEUUID(SVC_UUID), sd);
  adv->setScanResponseData(scanResp);
}

// The one place advertising is (re)armed, so the ordering above can only be got
// right once. `connectable` false = SCAN_IND, the M3 busy state: still
// scannable so the busy byte is readable, but the link layer refuses — which IS
// the single-connection enforcement (CONTRACT.md M3 realization note 5).
//
// 2026-08-06 CORRECTION — the real 1.10 bug was never an async race. That
// theory (a first draft of this comment, and of XFtposDecisions-62.md §9)
// was written reading BLEAdvertising.cpp's *Bluedroid* implementation
// (BLEAdvertising.cpp:639-1434) — code this board never runs.
// `arduino-cli --verbose` + a #ifdef probe sketch confirmed this exact
// ESP32-C6 build compiles NimBLE (CONFIG_NIMBLE_ENABLED), whose start()/stop()
// (BLEAdvertising.cpp:1440+) call ble_gap_adv_start()/ble_gap_adv_stop()
// directly — synchronous host-stack calls, no async completion events for
// configuration at all. There is no race to synchronize.
//
// The actual bug: OZ_ADV_TYPE_IND/OZ_ADV_TYPE_SCAN_IND (below) are Bluedroid's
// esp_ble_adv_type_t numeric values (IND=0x00, SCAN_IND=0x02).
// BLEAdvertising::setAdvertisementType()'s NimBLE branch
// (BLEAdvertising.cpp:145-147) writes that raw byte straight into
// m_advParams.conn_mode with NO translation — and NimBLE's conn_mode enum
// (ble_gap.h:2156-2158) is BLE_GAP_CONN_MODE_NON=0 / _DIR=1 / _UND=2, a
// DIFFERENT meaning at the same numeric values. So `connectable=true` (value
// 0x00) was setting conn_mode=BLE_GAP_CONN_MODE_NON — genuinely
// NON-connectable — and the busy state (0x02) was setting
// BLE_GAP_CONN_MODE_UND, genuinely connectable: exactly backwards. This
// explains every symptom without needing any race: the lock always advertised
// (conn_mode doesn't affect basic scannability) but never accepted a
// connection when it believed it was available, on any BLE stack, from the
// very first attempt of every boot — matching XF-62 (BA)'s `trace=[]` finding
// (the link layer never even reaches `connected`, because a non-connectable
// PDU cannot be connected to by protocol, not because of any timing).
//
// FIX: define the two constants per-backend, the same discriminator pattern
// the library itself uses throughout BLEAdvertising.cpp.
void bleRearmAdvertising(bool connectable, const char *why) {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (adv == nullptr) return;
  BLEDevice::stopAdvertising();
  delay(40);                       // let the stop land
  adv->setAdvertisementType(connectable ? OZ_ADV_TYPE_IND : OZ_ADV_TYPE_SCAN_IND);
  bleSetBusy(!connectable);        // AFTER the type
  delay(40);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] advertising %s (busy=%d) — %s\n",
                connectable ? "IND/connectable" : "SCAN_IND/busy",
                connectable ? 0 : 1, why);
}


class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleLinkCount++;
    bleClientConnected = (bleLinkCount > 0);
    Serial.printf("[BLE] connect  — links=%d\n", bleLinkCount);
    screenDirty = true;
    // Keep advertising, but non-connectably, so the busy flag is observable.
    // Only where we were supposed to be discoverable in the first place —
    // a commissioned lock outside its window stays dark, exactly as before.
    if (bleAdvertisingAllowed()) {
      bleRearmAdvertising(false, "client connected");
    }
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
    if (bleLinkCount == 0) {
      // M3: the challenge never outlives its connection. This is not hygiene —
      // M4 compares `control` frames against it, and a challenge that survived
      // a disconnect would let a captured frame be replayed on a fresh link.
      memset(bleChallenge, 0, sizeof(bleChallenge));
      bleChallengeValid = false;
      memberBuf = ""; // a half-written enrolment must not fuse with the next one
      ctlReset();     // M4: likewise, and it matters more here — a half-written
                      // control message fusing with the next connection's would
                      // put one bond's bytes in front of another's app_id
    }
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
    //
    // ⚠ Must go through bleRearmAdvertising(), NOT a bare startAdvertising().
    // The link that just closed left adv_type at SCAN_IND; a bare start would
    // either be a no-op (advertising already running, Bluedroid ignores param
    // changes) or restart non-connectably. Either way the lock stays visible and
    // refuses every subsequent connection until reboot — one connect per boot,
    // which is precisely the bug this call fixes.
    if (bleAdvertisingAllowed()) {
      bleRearmAdvertising(true, "link closed, back to discoverable");
    } else if (bleWindowUntil) {
      closeBleWindow("expired during session");
    } else {
      // Not supposed to be discoverable. Make sure the async scan-response
      // restart inside bleSetBusy() cannot leave us advertising anyway.
      BLEDevice::stopAdvertising();
    }
  }
};

// ── The INFO characteristic is REBUILT, not snapshotted (2026-08-16) ────────
//
// It used to be filled once inside startBle(), and startBle() only ever runs
// when bleServer == nullptr — which is once per boot, because bleServer is
// never nulled. So every field in here froze at GATT-build time.
//
// That is a real defect and it was found the expensive way: after
// `provision_key` replaced LockA's identity, the app read `info.pub` and got
// the PRE-provision key back (XF-106 §17 run 1). ftpos generously wrote it up
// as transient BLE staleness; a phone-side GATT cache may well have played a
// part, but ours was serving a stale value regardless.
//
// `pub` is the one that bites hardest, but it is not alone: `name` goes stale
// after every set_name (the whole rename epic reported through a field that
// could not change), and `transport` after a Wi-Fi/Thread conversion.
//
// Same fault class as the LCD showing "Thread: JOINED" from a latch that was
// set once and cleared nowhere: a value that was true when it was written and
// is never asked again.
static void ozRefreshInfoChar() {
  if (!chrInfo) return;
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  doc["name"] = cfgName;
  doc["pub"] = ozLockPubHex(); // X25519 ceremony pubkey (XF-46 §7.1)
  // XF-107 §3.1 — so BANOI can branch its at-the-door instruction instead of
  // promising a doorbell every lock may not have. See ozHasDoorbell().
  doc["has_doorbell"] = ozHasDoorbell();
  doc["profile"] = ozProfileId();
  if (cfgMcuPid.length()) doc["tuya_pid"] = cfgMcuPid;
  if (cfgMcuVer.length()) doc["mcu_fw"] = cfgMcuVer;
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
}

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

  // M3 …0005 challenge — READ only. Its value is (re)generated in the read
  // callback; the seed below exists so a read that somehow races the callback
  // still returns bytes rather than an empty attribute.
  BLECharacteristic *chal = svc->createCharacteristic(CHR_CHALLENGE, BLECharacteristic::PROPERTY_READ);
  chal->setCallbacks(new ChallengeCB());
  esp_fill_random(bleChallenge, sizeof(bleChallenge));
  chal->setValue(bleChallenge, sizeof(bleChallenge));

  // M4 …0006 control — WRITE, binary, reassembled by GCM-tag completeness with
  // an idle-timer backstop (see ctlBuf). Creating this characteristic is itself
  // the capability signal the app probes for: its absence means a lock that
  // cannot authorise an unlock, and the app says so in Vietnamese rather than
  // writing into the void.
  BLECharacteristic *ctl = svc->createCharacteristic(CHR_CONTROL, BLECharacteristic::PROPERTY_WRITE);
  ctl->setCallbacks(new ControlCB());

  // M3 …0007 member_enroll — WRITE, chunked JSON (the invite QR alone is ~270 B,
  // well past one MTU). Also carries M4 …103 list_bonds' reply: NOTIFY added
  // 2026-08-06 so the same chunked-JSON convention runs in both directions on
  // one characteristic rather than inventing a second one (XF-65 §6).
  chrMember = svc->createCharacteristic(CHR_MEMBER,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  chrMember->addDescriptor(new BLE2902());
  chrMember->setCallbacks(new MemberCB());

  chrInfo = svc->createCharacteristic(CHR_INFO, BLECharacteristic::PROPERTY_READ);
  ozRefreshInfoChar();

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  // Same path as every other advertise, so the ordering rule has exactly one
  // implementation. A fresh stack defaults to ADV_TYPE_IND, but relying on that
  // default is how the ordering bug hid for so long.
  bleRearmAdvertising(true, "startBle");
}

void buildTopics() {
  // S16 (operator directive 2026-08-10): the topic root is `ozkie/`, not
  // `ozkey/` — OZKEY is already another company's product, so this is
  // trademark exposure rather than naming taste. NOTE the boundary: the
  // HKDF info strings in ozcrypto.h ("ozkey/app->lock", "ozkey/lock->app",
  // "ozkey/invite-v1") are key-derivation INPUTS and must NEVER be renamed —
  // changing one character breaks every bond and every outstanding invite.
  // They are invisible on every product surface, so they carry no exposure.
  String base = "ozkie/" + cfgSiteId + "/locks/" + deviceId + "/";
  String legacyBase = "ozkey/" + cfgSiteId + "/locks/" + deviceId + "/";
  topicCommand = base + "command";
  topicCommandLegacy = legacyBase + "command"; // S16 transition — subscribe both
  topicEnroll = base + "enroll";
  topicHeartbeat = base + "heartbeat";
  topicLog = base + "log";
  topicPresence = base + "presence"; // XF-114 §9 — see the declaration

  // ozkey-17 §6a: a SEPARATE topic from heartbeat/log on purpose. Those two are
  // operational metadata the server legitimately reads (presence, fw, transport,
  // and the wake that flushes its queue). This one carries sealed content the
  // server must never parse. Splitting them at the topic level means the rule is
  // enforced by routing rather than by everyone remembering it.
  topicUplink = base + "uplink";
  // ozkey-33 — SITE-WIDE, RETAINED time. Not under this lock's own subtree on
  // purpose: one retained message serves every Wi-Fi lock on the site, and a
  // retained payload must never sit on a `command` topic (the broker would
  // redeliver it as a replayed command on every reconnect).
  //
  // This is what makes the 1.74 MQTT clock work actually run. A Wi-Fi lock
  // already reconnects and resubscribes on every keep-alive wake (60-600 s), and
  // a retained message is delivered the instant a client subscribes — so the
  // lock gets an authoritative clock within milliseconds of every wake, with no
  // request, no round trip, and no NTP (which this network blocks).
  topicTime = "ozkie/" + cfgSiteId + "/time";
  topicPairConfirm = "hotel/locks/" + deviceId.substring(4) + "/pair/confirm";
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch — kept ONLY for the factory-reset ceremony ('*' zone then '5' zone,
// same invisible grid as blelock so the operator muscle-memory transfers).
// ─────────────────────────────────────────────────────────────────────────────
// touchInit() — board-specific (RST/INT sequence vs pure I2C polling),
// defined before this #include.

static bool touchReadRegs(uint8_t *buf) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, 7) < 7) return false;
  for (int i = 0; i < 7; i++) buf[i] = Wire.read();
  return true;
}

int lastTapX = 0, lastTapY = 0;
uint8_t tapSamples = 0;

// 1.70 — THREE WAYS A TAP USED TO VANISH WITHOUT A TRACE
//
// The operator reported taps taking 10-20 s to register after power-up, on both
// boards. [MON]'s cadence (measured 2026-08-14: 10.003-10.016 s against a 10 s
// tick) proves the loop is NOT starved — touch is polled at ~65 Hz — so the
// taps were being lost inside this function or before it, and every one of the
// three ways that can happen was SILENT:
//
//   1. touchReadRegs() returns false (I2C NACK). A controller that has gone to
//      sleep, or a bus glitch, looked exactly like a finger that was never
//      there. This is the one that would confirm or kill the auto-sleep theory,
//      and it printed nothing at all.
//   2. The controller reports count=0 forever. Same silence.
//   3. OUR OWN two-sample rule threw the tap away — see below.
//
// [[silent-failures-rule]]: a diagnostic you cannot read is not a diagnostic.
// All three now speak, on CHANGE only so an idle panel stays quiet.
bool touchRead(int &tx, int &ty) {
  uint8_t buf[7];

  // (1) Does the digitizer answer at all?
  static bool lastRegsOk = true;
  const bool regsOk = touchReadRegs(buf);
  if (regsOk != lastRegsOk) {
    lastRegsOk = regsOk;
    Serial.printf("[TOUCH] i2c %s\n",
                  regsOk ? "answering again"
                         : "NO ACK — controller not responding (asleep? bus?)");
  }
  if (!regsOk) return false;

  // (2) What is it actually reporting? Edge-triggered: two lines per tap.
  uint8_t count = buf[2];
  static uint8_t lastCount = 0xFF;
  if (count != lastCount) {
    lastCount = count;
    Serial.printf("[TOUCH] count=%u\n", count);
  }

  // (2b) …and a 5 s heartbeat, because edge-triggered ALONE is unreadable.
  //
  // 1.70 printed only on change, so the single most important failure state —
  // the controller ACKing happily while reporting count=0 through a real
  // finger press — produced NO output whatsoever. A 90 s capture during the
  // operator's "3-5 s to respond" was therefore empty, and empty could mean
  // either "no taps happened" or "every tap was invisible". That is the same
  // silent-instrument trap as [[serial-capture-dead-use-ble]], rebuilt by hand.
  //
  // With a heartbeat, silence has exactly one meaning: this function is not
  // running. Anything else prints a live count and the poll rate, so a capture
  // is interpretable without knowing precisely when a finger landed.
  static unsigned long lastTouchBeat = 0;
  static uint32_t pollsSinceBeat = 0;
  pollsSinceBeat++;
  if (millis() - lastTouchBeat > 5000) {
    lastTouchBeat = millis();
    Serial.printf("[TOUCH] alive: count=%u polls=%lu in 5s (%lu Hz) down=%d\n",
                  count, (unsigned long)pollsSinceBeat,
                  (unsigned long)(pollsSinceBeat / 5), touchWasDown ? 1 : 0);
    pollsSinceBeat = 0;
  }

  bool down = (count > 0 && count <= 5);
  if (down) {
    lastActivityAt = millis();
    // Stamped on TOUCH-DOWN, not on the completed tap, so the clock starts the
    // instant the visitor's finger lands — the wake, Wi-Fi reassociation and
    // broker dial that follow all happen inside the window they just opened.
    lastTouchAt = lastActivityAt;
    // (3) SAMPLE ON THE FIRST DOWN POLL, not the second.
    //
    // This read used to sit behind `if (touchWasDown)`, so the first poll that
    // saw a finger only armed the flag and discarded its coordinates. A tap had
    // to span TWO consecutive polls to exist at all; one that spanned a single
    // poll fell out of the `n == 0` return below having logged nothing.
    //
    // At 65 Hz a human finger clears two polls easily, so this is not the whole
    // story — but it is a pure amplifier of anything upstream that makes the
    // controller report only intermittently. If the digitizer emits exactly one
    // frame on waking, the old code guaranteed that frame was thrown away, and
    // the user's real tap became the SECOND one they made.
    int rawX = ((buf[3] & 0x0F) << 8) | buf[4];
    int rawY = ((buf[5] & 0x0F) << 8) | buf[6];
    int x, y;
    mapTouchRaw(rawX, rawY, x, y); // board-specific transform + clamp
    lastTapX = x;
    lastTapY = y;
    if (tapSamples < 255) tapSamples++;
    touchWasDown = true;
    return false;
  }
  if (!touchWasDown) return false;
  touchWasDown = false;
  uint8_t n = tapSamples;
  tapSamples = 0;
  if (n == 0) {
    // Unreachable via the path above now that one down poll always samples —
    // kept as a live assertion rather than deleted. If this ever prints again
    // it means a touch-down was seen with no usable coordinate read, which is a
    // driver fault worth knowing about, not a user tapping too fast.
    Serial.println("[TOUCH] down->up with NO samples — tap discarded");
    return false;
  }
  tx = lastTapX;
  ty = lastTapY;
  return true;
}

// blelock's keypad grid — layout bands defined earlier in this file, right
// before drawOperational() (which needs them first). row 2 col 0 = '*', row
// 2 col 1 = '9' … we only care about '*' (bottom-left) and '5' (mid) for the
// factory-reset gesture.
// 4x2, not 4x3 (operator 2026-08-07): these taps only exercise the touch
// zones (any tap opens the BLE window; '*' then '5' arms/fires factory
// reset) — there's no real PIN-entry backend behind them, so a full 0-9
// digit set was never needed. Fewer rows frees real vertical space, given
// to the hex readout below instead (drawHexReadout() now uses two lines).
const char KP_KEYS[2][4] = {
  {'1','2','3','4'},
  {'*','5','6','#'},
};

char keyAt(int tx, int ty, int &r, int &c) {
  r = ty <= KP_TOP ? 0 : (ty - KP_TOP) / KEY_H;
  if (r > 1) r = 1;
  if (r < 0) r = 0;
  c = tx * 4 / PANEL_W;
  if (c > 3) c = 3;
  if (c < 0) c = 0;
  return KP_KEYS[r][c];
}

// Currently-highlighted key (tap feedback) — drawn by drawKeypad() itself
// (a full redraw already happens on every tap via openBleWindow()'s
// screenDirty, so the highlight lives here rather than a separate partial-
// redraw path) and cleared by the loop() timer check below.
int litKeyR = -1, litKeyC = -1;
unsigned long litKeySince = 0;
#define KEY_LIGHT_MS 400

void drawKeypad() {
  // Filled blue keys (operator, 2026-08-07: "employ blue amber etc. ...to
  // improve your UI") — reads as an actual button instead of an outline on
  // black, same idea as the LOCKED/UNLOCKED block always had before it
  // became a thin bar. Green fill + black text on tap, same as before.
  bool litActive = litKeyR >= 0 && millis() - litKeySince < KEY_LIGHT_MS;
  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 4; c++) {
      int x = c * KEY_W, y = KP_TOP + r * KEY_H;
      bool lit = litActive && r == litKeyR && c == litKeyC;
      // 1.70: the amber '#' is GONE, and that is not cosmetic. It existed to
      // say "this key, and only this key, opens the pairing window" — true from
      // 2026-08-11 until this version, false the moment any key started opening
      // it. A key coloured differently from its neighbours is a claim that it
      // does something different; leaving it would be the same class of defect
      // as 1.67's ADVERTISING screen, which went on saying a thing that had
      // stopped being true. All keys behave alike, so all keys look alike.
      gfx->fillRect(x + 2, y + 2, KEY_W - 4, KEY_H - 4, lit ? C_GREEN : C_BLUE);
      gfx->drawRect(x, y, KEY_W, KEY_H, C_GREY);
      gfx->setTextColor(lit ? C_BLACK : C_WHITE);
      gfx->setTextSize(3);
      char label[2] = {KP_KEYS[r][c], 0};
      gfx->setCursor(x + KEY_W / 2 - 9, y + KEY_H / 2 - 12);
      gfx->print(label);
    }
  }
}

// lastMcuHex is set by tuyaWireSend() (defined earlier — set by the operator
// directive, "no need to use locksim... display hex cmd instead").
// Line 2, final layout (operator, 2026-08-07): a 40px LOCKED/UNLOCKED color
// bar + the hex-command readout, in that order. The lock name moved OFF
// this line onto line 1 (drawStatusLine()) — line 1 was rebuilt to fit it
// there instead, so showing it twice would just be wasted width again.
void drawHexReadout() {
  // Bug fixed 2026-08-07: this was still drawing at y=STATUS_H — the SAME y
  // the color bar used — so this row's black background painted directly
  // over the bar every redraw (operator-caught: "big red line... overlay
  // and disappear"). Must be HEX_TOP, not STATUS_H.
  // FLICKER FIX 2026-08-11 (operator: "keep flickering every 3-5s"). This used
  // to blank the whole row to black and repaint it every redraw. At a 1 s clock
  // tick that is a visible strobe.
  //
  // Instead: paint the bar only when the bolt actually changes, and draw the
  // stamp as OPAQUE text (foreground + background), so each glyph overwrites
  // exactly its own pixels. No clear step, nothing to flicker. The stamp is
  // fixed-width by construction, so there is never a leftover tail to erase.
  bool open = doorStatus == "UNLOCKED";
  const int barW = 40;
  static int lastBar = -1;
  static uint32_t barGen = (uint32_t)-1;
  if (barGen != panelGen) { barGen = panelGen; lastBar = -1; } // screen was cleared
  const int barState = open ? 1 : 0;
  if (barState != lastBar) {
    lastBar = barState;
    gfx->fillRect(0, HEX_TOP + 2, barW, HEX_H - 4, open ? C_GREEN : C_RED);
    // 🟡 TEMPORARY, operator 2026-08-20: label the bar in words.
    //
    // The colour alone is easy to miss — during the first successful DP 76
    // unlock the operator was watching this panel and saw nothing, because a
    // 40 px block changing hue is not what the eye catches. It also auto-relocks
    // after 5 s, so the window to notice is short.
    //
    // REMOVE BEFORE THE REAL PCB SHIPS. On production the bolt state belongs to
    // the DL MCU and its own indicator; this is a bench affordance, not product
    // UI, and a lock that reports its own door state from the module's MIRROR of
    // MCU traffic would be asserting something it does not actually own.
    //
    // Drawn inside the existing bar so no other element moves: size 1 is 6 px
    // per glyph, so "OPEN"/"LOCK" is 24 px in a 40 px bar. Painted only on a
    // state CHANGE, preserving the 2026-08-11 flicker fix — repainting this row
    // every tick was a visible strobe.
    gfx->setTextSize(1);
    gfx->setTextColor(C_BLACK, open ? C_GREEN : C_RED); // opaque, no clear step
    gfx->setCursor(8, HEX_TOP + 6);
    gfx->print(open ? "OPEN" : "LOCK");
  }

  // ── LEFT: the clock, size 2 ────────────────────────────────────────────
  // Narrow form (16 chars, "11/08/26 09:17AM") is what allows size 2 here:
  // 16 x 12 = 192 px, which fits beside the door name on a 320 px panel.
  // Same format as the bridge, deliberately — comparing a lock screen against
  // the bridge screen should not require converting between two date formats.
  char stamp[17];
  ozFormatStampNarrow(stamp, sizeof(stamp), ozClockNow(ozclock, millis()), cfgTzMin);
  gfx->setTextSize(2);
  gfx->setCursor(barW + 6, HEX_TOP + 3);
  gfx->setTextColor(ozClockKnown(ozclock) ? C_GREEN : C_AMBER, C_BLACK); // opaque
  gfx->print(stamp);

  // ── RIGHT: the door name (operator, 2026-08-11) ────────────────────────
  //
  // Moved off line 1 and changed from device_id to NAME. On a wall the useful
  // identifier is "Front Door", not "ozk-acebe639" — the id matters when you
  // are typing an MQTT topic, and it is still on the advertising screen and in
  // every log line for that. An unnamed lock falls back to the short id so the
  // field is never blank.
  //
  // size 1 and right-aligned: 10 chars x 6 px = 60 px, which is what is left
  // after the 192 px clock. Padded to a fixed width so a shorter name cannot
  // leave the tail of a longer previous one on screen — there is no fillRect
  // on this row any more, opaque text is the only eraser.
  String dname = cfgName.length() ? asciiOnly(cfgName) : ozShortId(deviceId);
  if (dname.length() > 10) dname = dname.substring(0, 10);
  while (dname.length() < 10) dname += ' ';
  gfx->setTextSize(1);
  gfx->setCursor(PANEL_W - 4 - 60, HEX_TOP + 7);
  gfx->setTextColor(C_WHITE, C_BLACK); // opaque
  gfx->print(dname);
}



// ─────────────────────────────────────────────────────────────────────────────
// §0.2/§0.3 keep-alive nap (wake_sim=false only). Persistent power — this is
// light sleep, not rail-off: association state is in RAM, the module owns
// its cadence. Wake sources: SRDY low (MCU wants us) · heartbeat_s timer
// (the §0.3 proactive pull — THE credential-delivery guarantee) · screen
// touch (operator door-knock; also wakes a board before flashing).
// ─────────────────────────────────────────────────────────────────────────────
void enterKeepAliveSleep() {
  // Touch dropped as a wake source on this board — no confirmed INT line
  // (see header §4). SRDY + the heartbeat timer only.
  Serial.printf("[PWR] idle %lus — light sleep (wake: SRDY / %us timer"
#ifdef HAS_TOUCH_INT
                " / touch"
#endif
                ")\n",
                SLEEP_IDLE_MS / 1000, cfgHeartbeatS);
  Serial.flush(); // USB serial goes quiet during the nap — expected
  mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(LCD_BL, LCD_BL_OFF); // dark panel = the visible "napping" cue

  gpio_wakeup_enable((gpio_num_t)SRDY_PIN, GPIO_INTR_LOW_LEVEL);
#ifdef HAS_TOUCH_INT
  gpio_wakeup_enable((gpio_num_t)TOUCH_INT, GPIO_INTR_LOW_LEVEL);
#endif
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)cfgHeartbeatS * 1000000ULL);
  esp_light_sleep_start();

  sleepWakeCount++;
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  digitalWrite(LCD_BL, LCD_BL_ON);
  lastActivityAt = millis();
  screenDirty = true;
  Serial.printf("[PWR] wake #%u by %s — rejoin + heartbeat pull\n",
                (unsigned)sleepWakeCount,
                timerWake ? "timer (proactive pull)" :
#ifdef HAS_TOUCH_INT
                "GPIO (SRDY/touch)"
#else
                "GPIO (SRDY)"
#endif
                );
  if (!timerWake) mrdySet(true); // answer the MCU's SRDY immediately
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  lastMqttAttempt = 0; // dial the broker on the next loop pass
  // ...and dial it NOW, not on whatever backoff the pre-nap failures had grown
  // to. Waking from sleep is a fresh network situation: the radio was off, so
  // the last failure said nothing about whether the broker is reachable today.
  mqttRetryMs = OZ_MQTT_RETRY_MIN_MS;
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────

// 1.70 — HOW LONG IS THE DEAD ZONE AFTER POWER-UP? (operator, 2026-08-14:
// "after it is powered up it stuck for a while before become responsive")
//
// The panel cannot respond during setup() at all: touch is polled from loop(),
// and loop() does not run until setup() returns. So "unresponsive after boot"
// is not a touch bug — it is however long setup() takes, and NOTHING measured
// that. The stages here are not equally cheap (a 1.2 s splash delay, a
// LittleFS mount that may format, two X25519 self-tests, an OpenThread bring-up,
// a BLE stack allocation), and guessing which one dominates is exactly the kind
// of theory that has cost this project sessions. Print the number instead.
static unsigned long g_bootT0 = 0;
static unsigned long g_bootLast = 0;
static void bootMark(const char *stage) {
  const unsigned long now = millis();
  Serial.printf("[BOOT+%lums] %s (+%lu)\n", now - g_bootT0, stage,
                now - g_bootLast);
  g_bootLast = now;
}

void setup() {
  Serial.begin(115200);
  // 🔴 1.72 — NEVER LET A LOG LINE BLOCK THE DOOR.
  //
  // On this board Serial is the native USB CDC. When the port is enumerated by
  // a host but nothing is DRAINING it — a lock plugged into a laptop or a
  // charger with no terminal open, which is the bench's normal state — the TX
  // FIFO fills and Serial.print() blocks until someone reads. Every one of this
  // firmware's jobs runs on the loop task, so a blocked print freezes touch,
  // the panel, the MCU wire pump and the factory-reset button, exactly like the
  // 1.66 MQTT dial did.
  //
  // Measured 2026-08-14, first boot after flashing, with no reader attached:
  // worst single loop iteration was 20271 ms on DoorA, 8149 ms on LockC,
  // 6021 ms on DoorB — and all three ended the instant a reader attached. That
  // is the operator's "10-20 s before a key registers", and it is also why
  // every previous measurement looked healthy: those were taken with a capture
  // running, i.e. with the buffer being drained.
  //
  // Timeout 0 = drop when full, never wait. Diagnostics are worth having, but
  // not at the price of the lock stopping. A dropped log line costs a line; a
  // blocked one costs the door.
  Serial.setTxTimeoutMs(0);
  delay(300);
  g_bootT0 = g_bootLast = millis();
  Serial.println("\n*** OZLOCK COMM MODULE — unified doorlock (MCU = LockSim on UART) ***");
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

  // Which supplier DP map this unit dispatches under. Printed at boot because
  // it now decides what reaches the MCU, and because the default is our INVENTED
  // map — a fact that should be visible on every start, not buried in a header.
  ozProfileBegin();
  Serial.printf("[PROFILE] %s%s (%u DPs)\n", ozProfileId(),
                ozProfile()->deprecated ? " — INVENTED MAP, collides with the real"
                                          " Tuya catalogue (ozkey-27 §2.1)" : "",
                (unsigned)ozProfile()->count);

  // WHY DID WE JUST BOOT? Never logged until 2026-08-05, and its absence cost a
  // whole bench session: the board was resetting itself about every 10 minutes,
  // which silently killed the serial capture (USB re-enumerates, the reader's fd
  // goes stale) AND invalidated test results, because "it worked" sometimes just
  // meant "a reset had happened moments earlier". A reboot with no stated cause
  // is indistinguishable from a reboot someone asked for.
  const esp_reset_reason_t rr = esp_reset_reason();
  const char *rrName =
      rr == ESP_RST_POWERON   ? "POWERON"  : rr == ESP_RST_EXT      ? "EXT/RESET-PIN"
    : rr == ESP_RST_SW        ? "SW (ESP.restart)" : rr == ESP_RST_PANIC ? "PANIC/EXCEPTION"
    : rr == ESP_RST_INT_WDT   ? "INT WATCHDOG" : rr == ESP_RST_TASK_WDT ? "TASK WATCHDOG"
    : rr == ESP_RST_WDT       ? "OTHER WATCHDOG" : rr == ESP_RST_DEEPSLEEP ? "DEEPSLEEP WAKE"
    : rr == ESP_RST_BROWNOUT  ? "BROWNOUT (power!)" : rr == ESP_RST_SDIO ? "SDIO" : "UNKNOWN";
  Serial.printf("[BOOT] reset reason: %s (%d)\n", rrName, (int)rr);

  // Tuya MCU bus → LockSim Mode B (raw 55 AA frames, wire-tested 2026-07-19)
  // 🔴 RX BUFFER BEFORE begin() — the 0x08 census overflows the default.
  //
  // Measured on the bench 2026-08-20: LockSim answered a status query with all
  // 34 DPs of `tuya-ds013-t3` in one burst — its console confirmed "reported 34
  // DPs" — and firmware recorded only 8. The other 26 frames were dropped in
  // the UART driver, silently, before any of our code saw them.
  //
  // 34 frames x ~12 bytes = ~408 bytes, against the ESP32's default 256-byte
  // RX ring. A real MCU answering 0x08 will burst exactly the same way — Tuya
  // explicitly allows "all at one time" — so this is not a simulator artefact.
  //
  // The failure mode is the dangerous kind: a partial census looks like a
  // successful one. Firmware would have concluded the hardware lacks DP 76 and
  // refused to unlock, blaming the lock for our own dropped bytes.
  //
  // Must precede begin(); setRxBufferSize() is ignored once the driver is up.
  Serial1.setRxBufferSize(2048);
  Serial1.begin(9600, SERIAL_8N1, TUYA_RX_PIN, TUYA_TX_PIN);
  Serial.println("[TUYA] Serial1 up @ 9600 8N1 GPIO16(TX)/GPIO17(RX)");
  bootMark("banner + profile + tuya wire");

  // §0.2 wake lines — MRDY idles HIGH (also satisfies the GPIO8 strap)
  pinMode(SRDY_PIN, INPUT_PULLUP);
  pinMode(MRDY_PIN, OUTPUT);
  digitalWrite(MRDY_PIN, HIGH);

  // Transaction buffer (LittleFS, format on first mount)
  fsUp = LittleFS.begin(true);
  Serial.printf("[FS] LittleFS %s\n", fsUp ? "mounted" : "FAILED — txlog disabled");
  txlogCount0 = txlogCountLines("/txlog.0");
  txlogCount1 = txlogCountLines("/txlog.1");
  ozEvtSeqRestore(); // ozkey-29 §11.4 — resume the audit cursor across reboots
  Serial.printf("[FS] txlog %u event(s) buffered\n", (unsigned)txlogTotal());
  bootMark("LittleFS mount + txlog scan");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LCD_BL_ON);
  gfx->begin();
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(C_BLACK);
  bootMark("LCD begin + fillScreen");
  drawSplash();
  bootMark("splash (includes its own delay)");

  touchInit();
  bootMark("touch controller init");
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
  bootMark("OpenThread early begin()");

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
  bootMark("WiFi.mode(STA) + MAC");

  loadConfig();
  ozUplinkLoadPeer(); // ozkey-19 v2 R2 — restore the unicast target BEFORE any
                      // uplink can fire, so a reboot does not silently demote
                      // this lock to unacknowledged multicast.
  ozRosterEpochLoad(); // R5 — must survive the reboot that lost the push.

  // ozkey-21 — timezone survives reboot. Without this a rebooted lock shows
  // UTC on its panel until the next daily beacon, which reads as a wrong clock
  // rather than a missing one.
  prefs.begin("blelock", true);
  cfgTzMin = prefs.getShort("tzmin", 0);
  prefs.end();
  if (cfgTzMin) Serial.printf("[TIME] timezone restored: %+d min\n", (int)cfgTzMin);
  ozClockRestore();
  buildTopics();
  bootMark("config + peer + epoch + clock restore");

  // Ceremony identity (RF is up → TRNG seeded) + boot known-answer self-test.
  ozLockKeyInit();
  Serial.printf("[CRYPTO] info.pub=%s\n", ozLockPubHex().c_str());
  // ozkey-34 F-8. The AUTHORITATIVE mode is derived in ozLockKeyInit() from
  // whether eFuse holds a key; this NVS copy is written for diagnostics and so
  // a support dump can say which posture the unit came up in. It is never read
  // back to decide the mode — see ozOperationalMode()'s header for why a
  // writable mode key would be a downgrade vector.
  prefs.begin("blelock", false);
  prefs.putString("opmode", ozOperationalMode());
  prefs.end();
  Serial.printf("[CRYPTO] operational_mode=%s (derived from hardware)\n",
                ozOperationalMode());
  bootMark("lock keypair init");
  // M2: ownership state, printed every boot. This line is the Ask 6 factory-reset
  // evidence — after a reset both info.pub AND this must change (pub re-minted,
  // owner back to "none"), since prefs.clear() wipes the whole "blelock"
  // namespace that holds the keypair and the bond together.
  ozBondsLoad();
  Serial.printf("[BOND] bond #0: %s\n",
                ozBond0Present() ? ozBond0PubHex().c_str() : "none (unowned)");
  // M3: name every member at boot. A bond the operator cannot enumerate is a
  // bond they cannot audit, and this is the only surface that lists them until
  // the M4 uplink exists.
  for (int i = 1; i < OZ_BOND_MAX; i++) {
    if (!g_bonds[i].present) continue;
    char h[65];
    ozHex(g_bonds[i].pub, 32, h);
    Serial.printf("[BOND] member %d: label='%s' floor=%llu pub=%.16s…\n", i,
                  g_bonds[i].label, (unsigned long long)g_bonds[i].floor, h);
  }
  bootMark("bond load + enumerate");
  ozCryptoSelfTest();
  ozM4SelfTest();
  bootMark("crypto + M4 self-tests");

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
    // Post-boot grace: discoverable for BLE_BOOT_ADV_MS without any gesture,
    // because you have just reset/powered this lock and are standing at it.
    // After that it goes dark until a short BOOT press. See bleAdvertisingAllowed().
    bleBootAdvUntil = millis() + BLE_BOOT_ADV_MS;
    startBle();
    Serial.printf("[BLE] discoverable for %lus after boot, then press BOOT to pair\n",
                  BLE_BOOT_ADV_MS / 1000);
  }
  Serial.printf("[WAKE] wake_sim=%s hb=%us (SRDY=GPIO%d MRDY=GPIO%d)\n",
                wakeSim ? "ON (bench: SRDY assumed, no sleep)" : "OFF (honest)",
                cfgHeartbeatS, SRDY_PIN, MRDY_PIN);
  lastActivityAt = millis();
  screenDirty = true;
  bootMark("transport start");
  // THE number the operator asked about: until this line prints, the panel is
  // deaf — no touch poll has run even once.
  Serial.printf("[BOOT] setup() done in %lu ms — loop() starts now, panel is "
                "live from here\n",
                millis() - g_bootT0);
}

// 1.71 — WORST-ITERATION TIMER (the instrument that should have come first)
//
// Everything this firmware does runs on the loop task, so any single blocking
// call makes the panel deaf for exactly as long as it blocks. Two operator
// reports — "5-10 s for the panel to answer a touch" and "10-20 s before a key
// registers" — have outlived several theories because nothing ever measured
// the one number that matters.
//
// 🔴 WHY [MON] WAS THE WRONG METER, and I used it as one on 2026-08-14:
// [MON]'s own `lastMon = millis()` is stamped BEFORE its print and before the
// drawOperational() repaint it triggers, so a stall AFTER the tick does not
// push the next tick out. Measuring [MON] at 10.003-10.016 s therefore proves
// only that nothing blocks for longer than the interval itself. A 2 s repaint
// every 10 s is completely invisible to it — and the repaint runs in precisely
// that blind spot. This timer has no such hole: it measures top-of-loop to
// top-of-loop, so whatever ran in between is inside the number.
#define OZ_LOOP_LAG_WARN_MS 250UL
unsigned long g_loopMaxMs = 0;    // worst iteration since the last [MON]
const char *g_loopMaxWhat = "?";  // coarse attribution, set by the slow paths
static unsigned long g_loopLastTop = 0;

void loop() {
  {
    const unsigned long top = millis();
    if (g_loopLastTop) {
      const unsigned long dt = top - g_loopLastTop;
      if (dt > g_loopMaxMs) g_loopMaxMs = dt;
      // Printed the moment it happens, not just aggregated: an operator standing
      // at the bench watching a dead panel needs the console to say so WHILE it
      // is dead, not 10 s later in a summary.
      if (dt > OZ_LOOP_LAG_WARN_MS)
        Serial.printf("[LAG] loop iteration took %lu ms (last slow path: %s)\n",
                      dt, g_loopMaxWhat);
    }
    g_loopLastTop = top;
  }

  checkFactoryResetButton();

  // XF-52 (R): close the window on expiry — but never mid-session. A client
  // connected at the deadline keeps its link; onDisconnect() closes it then.
  // Cutting a live commissioning off at 60 s would produce exactly the
  // dropped-ladder "unknown" state XF-48 §21.3 exists to handle, self-inflicted.
  // XF-58: an armed assisted unlock that nobody came for. Dropping it is the
  // correct outcome of "no one was at the door", and saying so on the console is
  // what makes a door that did not open diagnosable instead of mysterious.
  if (assistArmedUntil && (long)(millis() - assistArmedUntil) >= 0) {
    assistArmedUntil = 0;
    assistArmedPayload = "";
    Serial.printf("[ASSIST] armed window expired after %lus — nobody touched, "
                  "command dropped\n",
                  ASSISTED_ARM_MS / 1000);
    txlogAppend("expired", "assisted unlock — nobody came to the door");
  }

  // M4: the whole `control` path runs here, on this task — the BLE callback only
  // appends. Try to open on every new chunk (at the MTU 247 the app requests
  // that is the first and only one, so this is a single pass), and when the
  // chunks stop, close it out.
  //
  // The idle backstop exists because the GCM tag cannot distinguish "still
  // coming" from "forged" — both are just a failure to open. Without it a
  // corrupted write would get silence, and the app would wait for a status that
  // is never coming: the XF-53 hang, rebuilt in a new place.
  if (ctlLen) {
    const bool idle = (millis() - ctlLastChunkAt) >= OZ_CTL_IDLE_MS;
    if (ctlNewBytes || idle) {
      ctlNewBytes = false;
      if (idle)
        Serial.printf("[CTL] no chunk for %lums — closing out %u buffered B\n",
                      OZ_CTL_IDLE_MS, (unsigned)ctlLen);
      // NOT ctlReset() on success — ozControlTry() consumes exactly the bytes it
      // processed via ctlConsume(), so a message that arrived while it was
      // working survives. Wiping wholesale here is what silently ate one.
      ozControlTry(idle);
    }
  }

  if (bleWindowUntil && !bleWindowOpen() && !bleClientConnected) {
    // DERIVED, not a literal (1.90). This said "60s elapsed" for every close,
    // frozen since before 1.85 shortened the window to 30 s — so the log
    // confidently reported double the real duration, and it is exactly the line
    // someone reads to VERIFY the window length. closeBleWindow() does no
    // timing of its own; it prints whatever string it is handed. Same rule as
    // FW_DISPLAY_VERSION and the "[BLE] window OPEN %lus" line above: derive it
    // from BLE_WINDOW_MS so there is nothing left to keep in step by hand.
    char why[24];
    snprintf(why, sizeof(why), "%lus elapsed", BLE_WINDOW_MS / 1000);
    closeBleWindow(why);
  }
  tuyaWirePump(); // MCU (LockSim) → module frames off the wire

  // Ask the MCU what it is, once the wire is alive and it has said something.
  // Retried a few times because a cold MCU can miss the first frame, then
  // dropped: an MCU that never answers 0x01 is an older one that does not
  // implement it, and pestering it forever would be noise on a shared UART.
  {
    // Asked even when a PID is already restored from NVS: the stored value is
    // a memory of the last MCU, and this board may have been moved to another
    // lock. `g_pidAsks` stops after a real answer, so a confirmed lock asks
    // once per boot, not forever.
    if (g_pidAsks < OZ_PID_MAX_ASKS && mcuLinkUp() &&
        (!g_pidAskedAt || millis() - g_pidAskedAt > OZ_PID_RETRY_MS)) {
      ozAskMcuProductInfo();
      if (g_pidAsks == OZ_PID_MAX_ASKS)
        Serial.println("[PID] MCU never answered 0x01 — staying on the "
                       "compiled-in profile. It may predate product info.");
    }

    // ── 0x08 DP census, once, after identity has settled ──────────────────
    //
    // Ordered AFTER 0x01 deliberately: the answer is only interesting once we
    // know which profile we are comparing against, and asking both at once
    // makes the two conversations interleave on one UART for no gain.
    //
    // Fire-and-forget with a settle window rather than a reply count, because
    // the MCU may answer as one grouped 0x07 or as N separate ones and the
    // protocol does not say which — so counting replies would be guessing.
    // 🔴 RE-ASK WHEN THE MCU COMES BACK. Tuya specifies the status query is
    // sent at TWO points: first power-on, AND whenever the module detects the
    // MCU has rebooted or gone offline and returned. A first version of this
    // asked once per boot, which would have missed exactly the case that
    // matters on a bench — the MCU being re-plugged, or LockSim reconnecting
    // its Web Serial — and left the census describing a device that had since
    // been swapped.
    static bool lastMcuUp = false;
    const bool up = mcuLinkUp();
    if (up && !lastMcuUp && g_dpListAsked) {
      Serial.println("[DPQ] MCU link returned — re-querying its DP list");
      memset(g_dpSeen, 0, sizeof(g_dpSeen)); // stale census describes the OLD device
      g_dpListAsked = false;
      g_dpListReported = false;
    }
    lastMcuUp = up;

    // 🔴 NOT gated on g_pidLatched. A first version was, on the reasoning that
    // the census is "only interesting once we know which profile to compare
    // against" — which is wrong twice. The profile is pinned at BUILD time, so
    // we always know it; and an MCU that never answers 0x01 (an older one, or a
    // simulator that stays silent) would then never be asked 0x08 either,
    // losing the more valuable of the two answers. Identity and capability are
    // independent questions and neither should gate the other.
    if (!g_dpListAsked && up) {
      ozAskMcuDpList();
    } else if (g_dpListAsked && !g_dpListReported &&
               millis() - g_dpListAskedAt > OZ_DPQ_IDLE_MS) {
      g_dpListReported = true;
      ozReportDpListComparison();
    }
  }

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
      // Fresh link, fresh chance: earlier broker failures were very likely
      // "no network" wearing a broker failure's clothes, so do not make the
      // lock serve out a 60 s backoff it earned while it had no IP at all.
      mqttRetryMs = OZ_MQTT_RETRY_MIN_MS;
      lastMqttAttempt = 0;
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
  if (isThread()) { pollThreadUdp(); ozUplinkRetryTick(); }

  // ── ozkey-20 R3 — Thread presence beacon ─────────────────────────────
  //
  // A Thread lock has no MQTT session, so publishHeartbeat() can never run
  // for it (there is a second gate inside the function itself). Consequences
  // we hit tonight, all from this one hole:
  //
  //  • The server can never mark a Thread lock reachable, so after a bridge
  //    restart every lock stays unreachable forever and the app falls back
  //    to BLE.
  //  • The bridge only learns a lock's identity from an uplink, and uplinks
  //    only happen on roster changes — so the liveness report could not name
  //    anyone until somebody happened to revoke a member.
  //  • ftpos built epoch reconciliation (XF-89 §7.1) against a heartbeat we
  //    do not send, so their correct code does nothing on our main transport.
  //
  // Deliberately UNSEALED and deliberately minimal: device_id, Thread
  // identity, firmware, roster epoch. No credentials, no door state, nothing
  // that would breach XF-47 if read in transit. It says "I exist, I am this
  // lock, my roster is at N" — facts already public in the topic name.
  //
  // Fire-and-forget by design (ozkey-20 §7.2): a lost beacon is replaced by
  // the next one, and retransmitting stale liveness is worse than useless.
  if (isThread() && threadUdpReady &&
      (g_threadBeaconDue ||
       millis() - lastThreadBeaconAt > cfgHeartbeatS * 1000UL)) {
    // XF-116. An explicit flag rather than back-dating lastThreadBeaconAt:
    // that trick relies on unsigned wraparound when millis() is still smaller
    // than the interval, which is correct and unreadable. This says what it
    // means, and clearing it here means an armed beacon fires exactly once.
    g_threadBeaconDue = false;
    lastThreadBeaconAt = millis();

    char extHex[17] = {0};
    otInstance *inst = esp_openthread_get_instance();
    if (inst && esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
      const otExtAddress *ea = otLinkGetExtendedAddress(inst);
      if (ea) ozHex(ea->m8, 8, extHex);
      esp_openthread_lock_release();
    }

    JsonDocument hb;
    hb["from"] = deviceId;
    if (extHex[0]) hb["ext"] = extHex;
    hb["kind"] = "presence";
    hb["fw"] = FW_VERSION;
    hb["roster_epoch"] = g_rosterEpoch;
    // ozkey-32 §5 Option A — see publishHeartbeat() for the full reasoning.
    // It matters MORE on this path than on MQTT: a Thread lock has no MQTT
    // session, so this beacon is the only unprompted thing it ever says. And
    // `set_name` is the only naming path a Thread lock has ever had, driven
    // over BLE — i.e. the rename and the reporting of it travel by completely
    // different routes, and without this field the server can never learn that
    // one happened.
    hb["name"] = cfgName;
    hb["bonds"] = ozBondCount();
    // XF-108 §5 — the PID rides the heartbeat too, not just BLE `info`.
    //
    // ftpos capture `tuya_pid` where a BLE session already reads `info`:
    // commissioning and member enrolment. That leaves every ALREADY-COMMISSIONED
    // lock with no PID until someone re-runs one of those — which for an
    // installed door is approximately never. The heartbeat reaches the server
    // unprompted, so the app can learn a lock's identity without anyone standing
    // at it. Same argument the `name` field above is here for.
    if (cfgMcuPid.length()) hb["tuya_pid"] = cfgMcuPid;
    if (g_profileMismatch) hb["profile_mismatch"] = true;
    // Which DP map this lock is ACTUALLY running. Fleet-visible so a lock
    // still on the invented default is findable without a BLE session.
    hb["profile"] = ozProfileId();
    hb["has_doorbell"] = ozHasDoorbell();
    hb["mcu_link_up"] = mcuLinkUp();          // ozkey-20 §5a
    hb["uptime_s"] = (uint32_t)(millis() / 1000);
    // ── ozkey-21 — PULL, don't sit waiting (operator, 2026-08-11) ─────────
    //
    // The bridge's time beacon is multicast, so it has no link-layer ACK: if
    // it is lost, a clock-less lock waits up to 24 h for the next one with
    // every temporary credential unenforceable meanwhile. Push alone is not
    // self-healing.
    //
    // So the lock ASKS. This rides the presence beacon it already sends — no
    // new message type, no new socket, and it inherits that beacon's retry
    // cadence for free, which is exactly the property that was missing.
    // The flag clears itself the moment we have a clock.
    // 1.74: ask while the clock is UNKNOWN **or merely RESTORED** — see
    // g_clockLive. The old test was ozClockKnown() alone, so a lock that booted
    // from an NVS snapshot never asked at all and sat on a stale clock until the
    // bridge's next 24 h multicast. Now it asks every beacon (60 s) until a real
    // source answers, and stops the moment one does.
    if (!ozClockKnown(ozclock) || !g_clockLive) hb["need_time"] = true;
    String out;
    serializeJson(hb, out);

    // Same dual path as a real uplink (ozkey-19 R7) — unicast for the MAC
    // ACK, ff03::1 because it needs no address.
    if (g_haveDownlinkPeer)
      ozThreadUdpSendOnce(out, (const uint8_t *)&g_lastDownlinkPeer.sin6_addr,
                          "beacon unicast");
    ozThreadUdpSendOnce(out, OZ_ALLNODES_BYTES, "beacon ff03::1");
    Serial.printf("[BEACON] presence epoch=%lu bonds=%d mcu=%s\n",
                  (unsigned long)g_rosterEpoch, ozBondCount(),
                  mcuLinkUp() ? "up" : "down");
  }

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

    // ── heartbeat ────────────────────────────────────────────────────────
    // The old comment here said "Wi-Fi/MQTT only — Thread has no uplink yet".
    // That premise died when ozkey-17 U1 shipped the uplink, and the note
    // outlived it by weeks — which is why Thread locks have NEVER reported
    // liveness (ozkey-20 §2.1).
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

  // ── touch: opens the BLE window (M3), and the factory-reset ceremony ─────
  //
  // ANY tap opens the window, not a designated zone. The keypad grid is a
  // hit-test with nothing drawn on it, so "tap the invisible # key" is not an
  // instruction a user at a door can follow — and the app's copy is already
  // *"chạm/nhấn nút trên khoá"* (XF-55 §13.1), which promises exactly this.
  //
  // This is the M3 PREREQUISITE, not a convenience. BOOT is on the board, i.e.
  // INSIDE the door; the keypad is outside. Without a touch path a member
  // standing at a commissioned lock has no way to make it advertise, so
  // member_enroll — and Wi-Fi/ECO owner unlock, and member unlock — are
  // unreachable no matter how correct the ceremony is.
  //
  // Physical presence is the whole security property here, and a tap is exactly
  // as physical as the BOOT press: nothing remote can open this window, and per
  // XF-52 §4 there must never be an MQTT or DPID verb that does.
  {
    int tx, ty;
    if (touchRead(tx, ty)) {
      int kr, kc;
      char k = keyAt(tx, ty, kr, kc);
      Serial.printf("[TOUCH] %d,%d -> key '%c'\n", tx, ty, k ? k : '-');
      litKeyR = kr; litKeyC = kc; litKeySince = millis(); // visual tap feedback

      // XF-58: somebody just arrived, and the owner already authorised. Fire the
      // held command and CONSUME it — single-shot, exactly as in the immediate
      // path, so one touch can never satisfy a second press.
      //
      // FIRST, before openBleWindow(). 1.7 had these the other way round, which
      // put the ~43 KB Bluedroid allocation of a cold startBle() — ~230 ms on the
      // bench — in front of the one action here that is time-critical and already
      // authorised. Opening the door does not depend on BLE being up, and if
      // startBle() ever stalled or failed outright it would have delayed or lost
      // an unlock the owner had already granted. The window is the thing that can
      // afford to wait.
      if (assistArmedUntil && (long)(millis() - assistArmedUntil) < 0) {
        Serial.println("[ASSIST] touch received — armed assisted unlock ALLOWED");
        const String payload = assistArmedPayload;
        assistArmedUntil = 0;
        assistArmedPayload = "";
        lastTouchAt = 0; // consumed here too; do not also satisfy a later command
        forwardHexToMcu(payload);
      }

      // ── BLE window: ANY key (operator, 2026-08-14) ─────────────────────
      //
      // WHY THIS PANEL IS NOT BENCH SCAFFOLDING — corrected after the operator
      // pointed it out: this board is intended to work as a SELF-CONTAINED
      // doorlock with no DL MCU and no LockSim attached. For that variant the
      // LCD keypad is the product's real keypad, not a stand-in. So there are
      // two legitimate pairing gestures, not one deprecated and one real:
      //
      //   • self-contained lock  -> the LCD keypad here
      //   • lock with a DL MCU   -> the DL MCU's keypad -> DP 60 (ozkey-22 §7)
      //
      // 🔴 THIS REVERSES THE 2026-08-11 DECISION, deliberately and on the
      // operator's instruction ("allow touching any key on the lcd will turn on
      // BLE, not just the '#' key"). The reversed rule and its reasoning are
      // kept here because the trade-off did not disappear, it was overruled:
      //
      //   2026-08-11, '#' only — "any tap re-arms a 60 s advertising window, so
      //   a sleeve brushing the panel leaves the lock discoverable. The window
      //   is a physical-presence CLAIM (XF-52 §4); it should cost a deliberate
      //   act, not an accident."
      //
      // What changed is evidence about the other failure: the operator found
      // the panel taking 10-20 s to respond after power-up, and a user who taps
      // a key and gets nothing has no way to tell "this is the wrong key" from
      // "this lock is broken". A designated key only works if the user knows
      // which one, and at a door nobody does. Making every key work removes an
      // entire class of "the lock ignored me" that we cannot otherwise
      // distinguish from a real fault.
      //
      // The accidental-advertising cost is REAL and unmitigated by this change.
      // What bounds it: the window is still 60 s and still self-closes, it is
      // still local-radio only, and pairing still requires the bond ceremony —
      // an open window is discoverability, not access. If accidental windows
      // become a nuisance in the field, the fix is a deliberate gesture (two
      // taps, or a long press), NOT a return to a secret key.
      // ── 🔴 THE LCD KEYPAD NO LONGER TOUCHES BLE OR RESET ─────────────────
      // (operator, 2026-08-16: "ignore doorlock esp32 1.9 lcd keypad.. we only
      // use in the lab. 5s hold on boot is good enough")
      //
      // Two triggers used to fire from this panel and both fired by ACCIDENT:
      //
      //   • ANY key opened a 60 s BLE window. The 2026-08-14 comment above
      //     justified that partly on the panel's 10-20 s lag making "wrong key"
      //     indistinguishable from "broken lock" — but that lag was the
      //     Serial.print() stall, fixed in 1.72 (worst case now 95 ms). The
      //     premise expired; the cost did not. A sleeve on the panel left the
      //     lock advertising, and an advertising window is a PRIVACY surface
      //     readable from the footpath (same channel the DP 8 handler above
      //     closed on the successful-unlock path) — and, once C9's Sleepy End
      //     Device lands, one of the most expensive things the radio does.
      //   • `*` armed a factory reset that any following `5` completed. On this
      //     layout `5` sits directly under `*`: two adjacent taps wiped the
      //     lock's identity and every bond. Far too cheap for an irreversible
      //     action.
      //
      // Both are simply GONE rather than replaced with a keypad gesture. This
      // panel is bench hardware, and the BOOT button already covers both jobs
      // deliberately and physically: a short press opens the window, a 5 s hold
      // factory-resets. A gesture on a lab-only panel would be machinery to
      // maintain for no product surface.
      //
      // The keypad still lights and logs — it remains useful for exercising
      // touch zones, which is what it is for.
      if (k) Serial.printf("[KEY] '%c' (no BLE/reset side effect)\n", k);
    }
  }

  // Clear the tap-highlight once its window elapses, even with no other
  // activity to trigger a redraw — otherwise a lit key can stay lit
  // indefinitely if nothing else marks the screen dirty in the meantime.
  if (litKeyR >= 0 && millis() - litKeySince >= KEY_LIGHT_MS) {
    litKeyR = litKeyC = -1;
    screenDirty = true;
  }

  // ── status-line-only refresh, every 3s (operator spec, 2026-08-07) —
  // calls drawStatusLine() DIRECTLY rather than setting screenDirty, so this
  // never triggers drawOperational()'s full fillScreen(). That was the
  // actual source of the flicker the operator flagged on real hardware
  // ("REMOVE THE 3S screen update..it causes bad flicker..can u update a
  // word rather than a whole screen") — a full-screen wipe+redraw every 3s
  // is a lot more visible than repainting one thin row. Real state changes
  // (door open/close, BLE window open/close, a tap) still go through the
  // normal screenDirty -> drawOperational() path elsewhere in this file.
  // ONE 1 s tick for both live rows. 1 s rather than the old 3 s because the
  // BLE countdown and the clock both tick in seconds — and it is affordable
  // now only because neither call repaints unconditionally any more:
  // drawStatusLine() returns early unless its content actually changed, and
  // drawHexReadout() writes opaque text over itself instead of clearing.
  // At 3 s the countdown visibly stuttered; at 1 s with unconditional repaints
  // the panel strobed. Gated 1 s is both correct and still.
  static unsigned long lastScreenTick = 0;
  if (millis() - lastScreenTick > 1000) {
    lastScreenTick = millis();
    if (state == ST_OPERATIONAL) {
      drawStatusLine();
      drawHexReadout();
    }
  }

  // ── ozkey-21 T1/T2 — keep our clock fed, and push if the MCU subscribed ──
  // ── Close the post-boot pairing grace once it lapses ────────────────────
  // Without this the lock would keep advertising until something else re-armed
  // it — the grace has to actually END, or it is just the old always-on
  // behaviour with extra steps.
  {
    static bool bootAdvClosed = false;
    if (!bootAdvClosed && bleBootAdvUntil && millis() >= bleBootAdvUntil) {
      bootAdvClosed = true;
      if (!bleAdvertisingAllowed() && !bleClientConnected) {
        BLEDevice::stopAdvertising();
        Serial.println("[BLE] post-boot pairing window lapsed — now dark until "
                       "a short BOOT press (operator, 2026-08-12)");
        screenDirty = true;
      }
    }
  }

  ozClockRefreshFromSystem();
  ozClockPersist(false);
  if (mcuWantsTimePush && ozClockKnown(ozclock) &&
      millis() - lastTimePushAt > MCU_TIME_PUSH_MS)
    serveMcuTimePush();

  // ozkey-21 T5: expire memberships on a timer as well as on the command path.
  // The timer is what makes an expiry OBSERVABLE — the roster_changed notify
  // and the heartbeat's bond count move on their own, so an admin's app learns
  // the member is gone without anyone touching the door. Without it, a bond
  // would linger in the roster until the next command happened to sweep it.
  {
    static unsigned long lastExpirySweep = 0;
    if (millis() - lastExpirySweep > OZ_EXPIRY_SWEEP_MS) {
      lastExpirySweep = millis();
      ozBondExpirySweep();
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
    // name= added 2026-08-14. The panel renders cfgName and falls back to the
    // device id when it is empty — so "the screen shows an id" is ambiguous
    // between "the name never arrived" and "the panel prefers the id". Nothing
    // printed cfgName anywhere, so the two could not be told apart from
    // outside. Quoted, because an empty name and a name of spaces look
    // identical unquoted, and the empty case is the one that matters.
    Serial.printf("[MON] %s name='%s' xport=%s mode=%s wifi=%s ip=%s mqtt=%s thread=%s "
                  "udp=%s mcu=%s tx=%u rx=%u wake=%s mrdy=%s srdy=%s hb=%us "
                  "radio=%s naps=%u heap=%u clock=%s treq=%u tserved=%u\n",
                  st, cfgName.c_str(),
                  provisioned ? cfgTransport.c_str() : "unset", modeInfo.c_str(),
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
                  cfgHeartbeatS,
                  // C9: which radio duty cycle this lock is ACTUALLY running.
                  // Printed for the same reason the link-mode read-back exists
                  // — "we set SED at boot" is an intention, not an observation,
                  // and the current-draw measurement is meaningless without
                  // knowing which mode produced it.
                  (!isThread() ? "n/a"
                               : (cfgThreadSed ? (bleWindowOpen() ? "SED/1s"
                                                                  : "SED")
                                               : "rx-on")),
                  (unsigned)sleepWakeCount,
                  (unsigned)ESP.getFreeHeap(),
                  // ozkey-21: "unknown" here means every temporary PIN/RFID
                  // window on this lock is unenforceable. It is the headline
                  // number, not a footnote.
                  // 1.74: three states, not two. "NVS-only" is the one that
                  // used to be indistinguishable from "known" and is exactly
                  // the state in which every temporary credential window is
                  // being judged against a guess.
                  !ozClockKnown(ozclock) ? "UNKNOWN"
                                         : (g_clockLive ? "live" : "NVS-only"),
                  (unsigned)mcuTimeRequests, (unsigned)mcuTimeServed);
    // The worst iteration in the last 10 s, then reset. A healthy lock reads
    // ~15-30 ms (the delay(15) plus a poll pass). Anything in the hundreds is
    // the panel being deaf for that long, and loopmax is the only number that
    // has ever been able to say so — see the timer at the top of loop().
    Serial.printf("[MON] loopmax=%lu ms (worst iteration since last MON, was: %s)\n",
                  g_loopMaxMs, g_loopMaxWhat);
    g_loopMaxMs = 0;
    g_loopMaxWhat = "-";
    if (state == ST_OPERATIONAL) screenDirty = true; // age/link refresh
  }

  // ── screen ────────────────────────────────────────────────────────────────
  // TIMED, and it is the prime suspect: a full drawOperational() is a
  // fillScreen() plus a keypad redraw over SPI, and [MON] sets screenDirty
  // every 10 s — inside [MON]'s own measurement blind spot (see the worst-
  // iteration timer at the top of loop()). If the panel is deaf for seconds at
  // a time on a 10 s rhythm, this is where it would be hiding.
  if (screenDirty) {
    screenDirty = false;
    const unsigned long t0 = millis();
    if (state == ST_ADVERTISING) drawAdvertising();
    else if (state == ST_JOINING) drawJoining();
    else drawOperational();
    const unsigned long dt = millis() - t0;
    g_loopMaxWhat = "screen repaint";
    if (dt > OZ_LOOP_LAG_WARN_MS)
      Serial.printf("[LAG] screen repaint took %lu ms\n", dt);
  }

  // ── §0.2/§0.3 keep-alive nap (Wi-Fi transport, honest mode only; bench
  // wake_sim skips). Thread SED polling as a wake source is still an open
  // decision (ozkey-10 §7 Q1) — Thread-transport locks never nap in this
  // pass rather than guess at an interaction that hasn't been decided.
  //
  // M3 BATTERY FIX: this used to require `bleServer == nullptr`, and bleServer
  // is never set back to null once startBle() has run. So the first BLE window
  // a lock ever opened stopped it napping again — permanently, until a reboot.
  // Harmless while the window was a rare BOOT press; with M3 a touch opens one,
  // so it would have become "any passer-by ends this lock's battery life".
  // What actually blocks a nap is an OPEN window or a LIVE link, so test those.
  // (The stack stays initialised across the nap: it is idle, not advertising.
  // Reclaiming its ~40 KB would need BLEDevice::deinit() and a rebuild of the
  // GATT server on the next window — a bigger change than M3 should carry.)
  if (!isThread() && !wakeSim && state == ST_OPERATIONAL && enrolled &&
      !bleWindowOpen() && !bleClientConnected &&
      doorStatus == "LOCKED" && !touchWasDown && !mrdyAsserted &&
      millis() - lastActivityAt > SLEEP_IDLE_MS) {
    enterKeepAliveSleep();
  }

  delay(15);
}
