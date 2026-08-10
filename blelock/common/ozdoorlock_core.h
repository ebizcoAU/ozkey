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
bool touchRead(int &tx, int &ty);
char keyAt(int tx, int ty, int &r, int &c);
void drawKeypad();
void drawHexReadout();
int hexNibble(char c);
static bool ozControlTry(bool final);
enum OzCtlOpen { OZCTL_OPENED, OZCTL_FAILED_DEFINITE, OZCTL_FAILED_MAYBE_INCOMPLETE };
static OzCtlOpen ozControlOpen(const uint8_t *buf, size_t n, int *outSlot,
                                uint8_t *pt, size_t ptCap, size_t *outPtLen,
                                uint64_t *outCounter);
static void ozControlVerifyAndDispatch(int slot, uint8_t *pt, size_t ptLen,
                                        uint64_t counter, bool hasChallenge);
static size_t ozHexDecode(const String &hex, uint8_t *out, size_t cap);
static bool ozDpForwardable(uint8_t dp);
static bool ozM4SelfTest();
static bool ozTuyaFrameOk(const uint8_t *f, size_t n);
static bool touchReadRegs(uint8_t *buf);
static size_t ozBuildDpFrame(uint8_t dp, uint8_t type, const uint8_t *val, size_t vlen, uint8_t *out);
static void addIdentity(JsonDocument &doc);
static void bond0Accept(OzBondVerdict v, const uint8_t provPub[32]);
static void copyLabelUtf8(const char *src, char *dst, size_t cap);
static void ctlConsume(size_t n);
static void ctlReset();
static void handleBondRevoke(int senderSlot, const uint8_t *v, size_t vlen);
static void handleInviteCancel(int senderSlot, const uint8_t *v, size_t vlen);
static void handleListBonds(int senderSlot, const uint8_t *v, size_t vlen);
static void ozControlDispatch(int slot, const uint8_t *frame, size_t flen);
static void ozSemanticDispatch(int slot, const char *json, size_t len);
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
void pollThreadUdp();
void publishEnroll();
void publishHeartbeat();
void publishLog(const char *result, const char *detail);
void publishUnpairedAnnounce();
void saveConfig();
void setup();
void startBle();
void threadUdpBegin();
void tuyaWirePump();
void tuyaWireSend(const uint8_t *f, size_t n);
void txlogAppend(const char *result, const char *detail);

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
WiFiClient wifiTcp;
PubSubClient mqtt(wifiTcp);
unsigned long lastHeartbeat = 0, lastMqttAttempt = 0, wifiJoinStart = 0;
unsigned long lastThreadBeaconAt = 0; // ozkey-20 R3 — Thread presence beacon
unsigned long lastEnrollSent = 0;
uint8_t enrollAttempts = 0;
unsigned long lastUnpairedAnnounce = 0;
String topicCommand, topicEnroll, topicHeartbeat, topicLog, topicPairConfirm;
String topicUplink; // ozkey-17 U1 — sealed lock->app content, opaque to the server
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
void drawStatusLine() {
  ot_device_role_t liveRole = isThread() ? OpenThread::otGetDeviceRole() : OT_ROLE_DISABLED;
  bool threadAttached = (liveRole == OT_ROLE_CHILD || liveRole == OT_ROLE_ROUTER ||
                         liveRole == OT_ROLE_LEADER);
  bool netUp = isThread() ? threadAttached
                          : ((WiFi.status() == WL_CONNECTED) && mqtt.connected());
  uint16_t border = netUp ? C_GREEN : C_RED;

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
  gfx->print(" ");
  gfx->setTextColor(C_WHITE); // same contrast fix as the hex line's name (C_GREY was unreadable)
  String name = cfgName.length() ? cfgName : deviceId;
  gfx->print(name.substring(0, 10));

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
#define BLE_WINDOW_MS 60000UL
#define BUTTON_DEBOUNCE_MS 60UL
// bleWindowUntil is declared with the other BLE globals near the top — see the
// note there about drawStatus() needing it before this point.

void startBle(); // defined with the GATT setup, further down

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
  if (!wasOpen)
    Serial.printf("[BLE] window OPEN %lus (%s)\n", BLE_WINDOW_MS / 1000, gesture);
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
  screenDirty = true; // hex readout lives on the dashboard
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

  mcuRxFrames++;
  lastMcuFrameAt = millis();
  lastActivityAt = millis();
  lastMcuSummary = describeDpid(f, n);
  Serial.printf("[TUYA<-] %s (%u bytes)\n", lastMcuSummary.c_str(), (unsigned)n);
  screenDirty = true;

  if (n >= 4 && f[3] == 0x00) return; // MCU heartbeat = link-alive only

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
      factoryReset(); // never returns
      return;
    }
    Serial.printf("[TUYA<-] 0x34 sub 0x%02X — not handled\n", sub);
    return;
  }

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

    // ── PROPOSED DP 60 — keypad pairing gesture (ozkey-22 §7) ──────────────
    //
    // On production the keypad belongs to the DL MCU and our board has NO
    // touch panel, so this is the only way a member standing at the door can
    // make the lock advertise. Without it member_enroll is unreachable on real
    // hardware — see ozdoorlock_core.h's own "M3 PREREQUISITE" note, whose
    // reasoning is right and whose mechanism does not exist in production.
    //
    // ⚠️ THE DP NUMBER IS A PLACEHOLDER pending manufacturer allocation. No
    // shipping DL MCU sends this; only LockSim does. Handling it now lets the
    // ceremony be developed and tested against the real topology instead of
    // against a dev board's incidental touch screen.
    //
    // SECURITY: unchanged from the touch path it replaces. The gesture is
    // still physical, still on the keypad outside the door, still in the
    // user's hand. XF-52 §4 forbids a REMOTE verb opening this window; a
    // keypress relayed over the in-door UART is not remote.
    if (dpid == 60) {
      if (!provisioned) {
        Serial.println("[BLE] DL MCU pairing gesture ignored — lock not provisioned");
        return;
      }
      Serial.println("[BLE] pairing gesture from DL MCU keypad (proposed DP 60)");
      openBleWindow("DL MCU keypad gesture");
      return;
    }
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

  if (n >= 11 && f[3] == 0x06) {
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
  // R5 / ozkey-20 R4: the epoch rides the heartbeat so an app can detect a
  // missed roster change with NO push having succeeded. This is the pull half
  // of the design and it is what makes the system converge.
  //
  // NOTE the `mqtt.connected()` guard above: on a THREAD lock this whole
  // function never runs, which is ozkey-20 §2.1 — the reason Thread locks have
  // never reported liveness. Not fixed here; that is ozkey-20 R3 and it needs
  // the uplink path, not this one.
  doc["roster_epoch"] = g_rosterEpoch;
  // On EVERY heartbeat, not just enroll. A transport change does not always
  // re-enroll (a re-provision of an already-enrolled lock does not), and this is
  // the only message a lock in service sends unprompted — so it is the only
  // thing that can make a stale server row self-heal within one interval.
  addIdentity(doc);
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
static void addIdentity(JsonDocument &doc) {
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
  if (isThread()) caps.add("remote_unlock");
  else caps.add("assisted_unlock");
  caps.add("pin_sync");
  caps.add("audit");
}

void publishEnroll() {
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["mac"] = macStr;
  doc["fw"] = FW_VERSION;
  addIdentity(doc);
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
static bool ozDpForwardable(uint8_t dp) {
  return dp == 1 || (dp >= 21 && dp <= 24);
}

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
  String target = (const char *)(doc["target"] | "");
  if (target != deviceId) { // not for us
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
      notifyStatus("UNLOCK_DENIED");
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
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    ozControlVerifyAndDispatch(slot, pt, ptLen, counter, false /*hasChallenge*/);
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

// Seal [json] to the bond in [slot] and emit it on whatever transport this lock
// has. Returns true only if it actually went out — a caller that needs to know
// whether the app can possibly have heard must check, because "sealed fine but
// nothing carried it" is the normal state of a Thread lock whose bridge is down.
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
    if (topicCommandLegacy.length()) mqtt.subscribe(topicCommandLegacy.c_str(), 1); // S16
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
      notifyStatus("UNLOCK_DENIED");
      return;
    }
    ozControlVerifyAndDispatch(slot, pt, ptLen, counter, false /*hasChallenge*/);
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
    if (topicCommandLegacy.length()) mqtt.subscribe(topicCommandLegacy.c_str(), 1); // S16
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
  // XF-87/ozkey-21: at v2 `me` is now SIGNED, so it is trustworthy the moment
  // we have a clock and somewhere to store it — T1 (clock) and T4 (bond
  // expires_at). Until then we log it and do nothing, and the log says so
  // rather than implying enforcement that does not exist.
  Serial.printf("[MEMBER] invite v%d VERIFIED label='%s' qr_expires=%u", ver,
                label.c_str(), (unsigned)expires);
  if (ver >= 2)
    Serial.printf(" membership_expires=%u%s", (unsigned)memExp,
                  memExp ? " (SIGNED, not yet enforced — ozkey-21 T4/T5)"
                         : " (permanent)");
  Serial.println(" (expiry not enforced)");

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
    Serial.printf("[CRYPTO] selftest dp-allow-list %s (101/102 forwardable=%d/%d)\n",
                  pass ? "PASS" : "FAIL — 101/102 COULD REACH THE MCU",
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
  publishLog("bond_revoked", label); // …then the housekeeping
  // ozkey-17 U1: and tell every OTHER admin, unprompted. This is the event
  // whose absence produced XF-77 — a revoke that genuinely happened, which the
  // other side could not observe and therefore diagnosed as bond-table
  // corruption. The lock knows; it should say so rather than wait to be asked
  // by whoever next happens to stand in front of it.
  ozNotifyRosterChanged("bond_revoked");
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
  publishLog("invite_cancelled", "admin cancelled an unredeemed invite");
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
  publishLog("bonds_listed", detail.c_str());
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
                             : "BLE unlock (member)");
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
static size_t ozSemGrantValue(JsonDocument &doc, uint8_t *out, size_t cap) {
  const uint32_t slotNo = doc["slot"] | 0xFFFFFFFFu;
  const uint32_t from   = doc["from"] | 0u;
  const uint32_t to     = doc["to"]   | 0u;
  if (slotNo > 0xFFFF) return 0;

  uint8_t cred[72];
  const size_t clen =
      ozHexDecode(String((const char *)(doc["cred"] | "")), cred, sizeof(cred));
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

static void ozSemanticDispatch(int slot, const char *json, size_t len) {
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

  if (strcmp(kind, "unlock") == 0) {
    dp = 1; type = 0x01 /* BOOL */; val[0] = 0x01; vlen = 1;
  } else if (strcmp(kind, "grant_pin") == 0 || strcmp(kind, "grant_rfid") == 0) {
    dp = (strcmp(kind, "grant_pin") == 0) ? 21 : 23;
    type = 0x00 /* RAW */;
    vlen = ozSemGrantValue(doc, val, sizeof(val));
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
  if (dp != 1 && g_bonds[slot].role != OZ_ROLE_ADMIN) {
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
  publishLog("granted", slot == 0
                            ? (dp == 1 ? "OZKIE unlock (owner)" : "OZKIE credential (owner)")
                            : "OZKIE unlock (member)");
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
    return OZCTL_FAILED_DEFINITE;
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
                                        uint64_t counter, bool hasChallenge) {
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
    notifyStatus("UNLOCK_DENIED");
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
    ozSemanticDispatch(slot, (const char *)body, blen);
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
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  ozControlVerifyAndDispatch(slot, pt, ptLen, counter, true /*hasChallenge*/);
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
    if (!provisioned || bleWindowOpen()) {
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
    if (!provisioned || bleWindowOpen()) {
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
  // ozkey-17 §6a: a SEPARATE topic from heartbeat/log on purpose. Those two are
  // operational metadata the server legitimately reads (presence, fw, transport,
  // and the wake that flushes its queue). This one carries sealed content the
  // server must never parse. Splitting them at the topic level means the rule is
  // enforced by routing rather than by everyone remembering it.
  topicUplink = base + "uplink";
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

bool touchRead(int &tx, int &ty) {
  uint8_t buf[7];
  if (!touchReadRegs(buf)) return false;
  uint8_t count = buf[2];
  bool down = (count > 0 && count <= 5);
  if (down) {
    lastActivityAt = millis();
    // Stamped on TOUCH-DOWN, not on the completed tap, so the clock starts the
    // instant the visitor's finger lands — the wake, Wi-Fi reassociation and
    // broker dial that follow all happen inside the window they just opened.
    lastTouchAt = lastActivityAt;
    if (touchWasDown) {
      int rawX = ((buf[3] & 0x0F) << 8) | buf[4];
      int rawY = ((buf[5] & 0x0F) << 8) | buf[6];
      int x, y;
      mapTouchRaw(rawX, rawY, x, y); // board-specific transform + clamp
      lastTapX = x;
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
      // '#' is the pairing key (2026-08-11) — it opens the BLE window, and no
      // other key does. A designated key nobody can pick out is the same as no
      // key, so it gets its own colour: amber, matching the amber BLE badge on
      // the status line so the two read as the same feature.
      const bool isPairKey = (KP_KEYS[r][c] == '#');
      gfx->fillRect(x + 2, y + 2, KEY_W - 4, KEY_H - 4,
                    lit ? C_GREEN : (isPairKey ? C_AMBER : C_BLUE));
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
  gfx->fillRect(0, HEX_TOP, PANEL_W, HEX_H, C_BLACK);

  bool open = doorStatus == "UNLOCKED";
  const int barW = 40;
  gfx->fillRect(0, HEX_TOP + 2, barW, HEX_H - 4, open ? C_GREEN : C_RED);

  // textSize 1 (operator, 2026-08-07: size 2 didn't fit horizontally on real
  // hardware — "1 step smaller"). Still truncated with "..." rather than
  // letting a long frame (list_bonds chunks are ~180 B / 540 hex chars,
  // nowhere close to fitting at any readable size either way) run off
  // screen invisibly. Full bytes are always in the [TUYA->] serial line
  // regardless.
  gfx->setTextSize(1);
  gfx->setCursor(barW + 6, HEX_TOP + 6);
  gfx->setTextColor(lastMcuHex.length() ? C_AMBER : C_DIM);
  const char *text = lastMcuHex.length() ? lastMcuHex.c_str() : "no command sent yet";
  const size_t maxChars = 44; // more room now the name moved to line 1
  String full = String(text);
  bool truncated = full.length() > maxChars;
  gfx->print(full.substring(0, maxChars));
  if (truncated) gfx->print("...");
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n*** OZLOCK COMM MODULE — unified doorlock (MCU = LockSim on UART) ***");
  Serial.printf("[FW] %s built %s %s\n", FW_VERSION, __DATE__, __TIME__);

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
  digitalWrite(LCD_BL, LCD_BL_ON);
  gfx->begin();
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(C_BLACK);
  drawSplash();

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
  ozUplinkLoadPeer(); // ozkey-19 v2 R2 — restore the unicast target BEFORE any
                      // uplink can fire, so a reboot does not silently demote
                      // this lock to unacknowledged multicast.
  ozRosterEpochLoad(); // R5 — must survive the reboot that lost the push.
  buildTopics();

  // Ceremony identity (RF is up → TRNG seeded) + boot known-answer self-test.
  ozLockKeyInit();
  Serial.printf("[CRYPTO] info.pub=%s\n", ozLockPubHex().c_str());
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
  ozCryptoSelfTest();
  ozM4SelfTest();

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
      millis() - lastThreadBeaconAt > cfgHeartbeatS * 1000UL) {
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
    hb["bonds"] = ozBondCount();
    hb["mcu_link_up"] = mcuLinkUp();          // ozkey-20 §5a
    hb["uptime_s"] = (uint32_t)(millis() / 1000);
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

      // ── BLE window: '#' ONLY, not any tap (operator, 2026-08-11) ───────
      //
      // WHY THIS PANEL IS NOT BENCH SCAFFOLDING — corrected after the operator
      // pointed it out: this board is intended to work as a SELF-CONTAINED
      // doorlock with no DL MCU and no LockSim attached. For that variant the
      // LCD keypad is the product's real keypad, not a stand-in. So there are
      // two legitimate pairing gestures, not one deprecated and one real:
      //
      //   • self-contained lock  -> '#' here
      //   • lock with a DL MCU   -> the DL MCU's keypad -> DP 60 (ozkey-22 §7)
      //
      // WHY '#' AND NOT ANY TAP: any tap re-armed a 60 s advertising window,
      // so a sleeve brushing the panel — or a user pressing digits for any
      // reason at all — left the lock discoverable and connectable
      // indefinitely. The window is a physical-presence CLAIM (XF-52 §4); it
      // should cost a deliberate act, not an accident.
      //
      // '#' is free: '*' arms factory reset and '5' confirms it, and '#' reads
      // as "enter/confirm" in the same idiom. The other keys still register as
      // touches for assisted-unlock presence above — that is unchanged, and it
      // SHOULD be any tap, because there the tap is proving a human is at the
      // door, not requesting anything.
      if (provisioned && k == '#') {
        Serial.println("[BLE] '#' pressed — pairing window requested");
        openBleWindow("keypad '#'");
      }

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
  static unsigned long lastScreenTick = 0;
  if (millis() - lastScreenTick > 3000) {
    lastScreenTick = millis();
    if (state == ST_OPERATIONAL) drawStatusLine();
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
      !bleWindowOpen() && !bleClientConnected && !resetArm &&
      doorStatus == "LOCKED" && !touchWasDown && !mrdyAsserted &&
      millis() - lastActivityAt > SLEEP_IDLE_MS) {
    enterKeepAliveSleep();
  }

  delay(15);
}
