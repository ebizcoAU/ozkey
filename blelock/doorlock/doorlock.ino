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
// M3/M4 (CONTRACT.md "Operational / member profile"). …0006 `control` exists as
// of M4; until then it was deliberately absent, because an advertised-but-inert
// characteristic would let the app's capability probe conclude the lock can
// authorise an unlock when it could not.
#define CHR_CHALLENGE "4f5a4b31-0005-4c4f-434b-000000000001"
#define CHR_CONTROL "4f5a4b31-0006-4c4f-434b-000000000001"
#define CHR_MEMBER "4f5a4b31-0007-4c4f-434b-000000000001"

// Bluedroid advertising PDU types (esp_ble_adv_type_t). Spelled out here rather
// than included: BLEAdvertising::setAdvertisementType() takes a bare uint8_t and
// the BLE library does not re-export Bluedroid's enum, while
// esp_gap_ble_api.h is not on the sketch include path in core 3.3.11. Values are
// the Bluetooth Core spec's own PDU type codes and cannot drift.
#define OZ_ADV_TYPE_IND 0x00      // connectable, scannable, undirected
#define OZ_ADV_TYPE_SCAN_IND 0x02 // scannable, NON-connectable, undirected
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
#define FW_VERSION "doorlock-1.9"
#define FW_DISPLAY_VERSION "v1.9" // shown on-screen next to the OZLOCK logo

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
unsigned long lastEnrollSent = 0;
uint8_t enrollAttempts = 0;
unsigned long lastUnpairedAnnounce = 0;
String topicCommand, topicEnroll, topicHeartbeat, topicLog, topicPairConfirm;
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
  if (ozBond0Present()) {
    gfx->setTextColor(C_GREEN);
    gfx->print(ozBond0PubHex().substring(0, 8));
    // M3: members, if any. An installer verifying an enrolment has no other way
    // to see it happened — the member's phone shows its own view, not the
    // lock's, and the two disagreeing is precisely the failure worth catching.
    const int members = ozBondCount() - 1;
    if (members > 0) {
      gfx->setTextColor(C_WHITE);
      gfx->printf(" +%d", members);
    }
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

// No default argument on `gesture`: the IDE auto-prototypes this file, and a
// generated prototype that repeats a default argument is a hard compile error.
void openBleWindow(const char *gesture) {
  const bool wasOpen = bleWindowOpen();
  bleWindowUntil = millis() + BLE_WINDOW_MS;
  if (bleServer == nullptr) startBle(); // first open: build the GATT server
  else if (!bleClientConnected)
    BLEDevice::startAdvertising(); // already built: just go discoverable
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
  // M4 verbs. Named here so a bench capture reads as English rather than "DP
  // 101 type 0 len 32" — the whole point of the LockSim test is being able to
  // see at a glance that these never crossed the wire.
  if (dpid == 101) return String("bond_revoke (in-lock, never forwarded)");
  if (dpid == 102) return String("invite_cancel (in-lock, never forwarded)");
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

  if (ver != 1 || nonceHx.length() != 32 || macHex.length() != 64) {
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
      ozInviteMac(s0, 32, invDev, issuer, roleStr, label, nonceHx, expires, want);
  ozFromHex(macHex.c_str(), got, 32);
  memset(s0, 0, sizeof(s0));
  if (!macOk || !ozCtEq(want, got, 32)) {
    Serial.println("[MEMBER] invite MAC does not verify — refused");
    notifyStatus("MEMBER_FAIL");
    return;
  }

  // `expires` is parse-and-ignore in v1 (XF-47): the lock has no clock, so
  // enforcing it would be theatre. MEMBER_EXPIRED is reserved and NEVER
  // emitted. The nonce is the hard guarantee; DPID 102 is the kill switch.
  Serial.printf("[MEMBER] invite VERIFIED label='%s' expires=%u (not enforced)\n",
                label.c_str(), (unsigned)expires);

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

// Execute an opened, challenged, floor-cleared frame.
static void ozControlDispatch(int slot, const uint8_t *frame, size_t flen) {
  const uint8_t dp = frame[6];
  const size_t vlen = ((size_t)frame[8] << 8) | frame[9];
  const uint8_t *v = frame + 10;

  if (dp == 101) { handleBondRevoke(slot, v, vlen); return; }
  if (dp == 102) { handleInviteCancel(slot, v, vlen); return; }

  // v1 carries exactly one MCU verb over BLE: DP 1, the at-the-door unlock
  // (CONTRACT.md …0006, "DP 1 remote-unlock in v1"). Credential frames 21-24
  // are NOT accepted here — they reach the MCU on the server path, which is
  // where issuance lives. Accepting them over BLE too would mean two authorities
  // for one operation, and the BLE one has no server-side record of what it
  // issued. Anything else is unknown and is rejected, never forwarded.
  if (dp != 1) {
    Serial.printf("[CTL] DP %u is not a v1 `control` verb — rejected, "
                  "NOT forwarded\n", dp);
    notifyStatus("UNLOCK_DENIED");
    return;
  }

  Serial.printf("[CTL] unlock authorised by bond %d ('%s', %s)\n", slot,
                g_bonds[slot].label,
                g_bonds[slot].role == OZ_ROLE_ADMIN ? "admin" : "member");
  forwardFrameToMcu(frame, flen);
  publishLog("granted", slot == 0 ? "BLE unlock (owner)" : "BLE unlock (member)");
  notifyStatus("UNLOCK_OK");
}

// Try to open whatever is buffered. [final] means the idle timer fired and no
// more bytes are coming, so an unopenable buffer must be answered rather than
// left to accumulate. Returns true when the buffer was consumed either way.
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

  // The first 64 bytes are whole as soon as we are past the minimum length, so
  // a malformed app_id is answerable now — no reason to make the caller wait
  // out the idle timer for a verdict that cannot change.
  char appIdHex[65];
  memcpy(appIdHex, ctlBuf, 64);
  appIdHex[64] = 0;
  if (!ozIsHex(appIdHex, 32)) {
    Serial.println("[CTL] leading 64 bytes are not an app_id hex string");
    ctlConsume(n);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }
  uint8_t senderPub[32];
  ozFromHex(appIdHex, senderPub, 32);
  const int slot = ozBondFind(senderPub);
  if (slot < 0) {
    Serial.printf("[CTL] %.16s… holds no bond on this lock\n", appIdHex);
    ctlConsume(n);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  uint8_t ps[32], key[32];
  const bool haveKey =
      ozBondSecret(slot, ps) &&
      ozEnvKey(ps, 32, deviceId, String(appIdHex), true /*app->lock*/, key);
  memset(ps, 0, sizeof(ps));
  if (!haveKey) {
    Serial.println("[CTL] could not derive the bond's app->lock key");
    memset(key, 0, sizeof(key));
    ctlConsume(n);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  uint8_t pt[OZ_CTL_MAX];
  uint64_t counter = 0;
  const int ptLen = ozEnvOpen(key, deviceId, ctlBuf + 64, n - 64, pt,
                              sizeof(pt), &counter);
  memset(key, 0, sizeof(key));
  if (ptLen < 0) {
    // Indistinguishable from here: still arriving, or forged. Keep waiting
    // unless the idle timer already gave up on it.
    if (!final) return false;
    Serial.printf("[CTL] envelope did not open (%u B, bond %d) — denied\n",
                  (unsigned)(n - 64), slot);
    ctlConsume(n);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  // Opened successfully: the plaintext is in pt[] and ctlBuf is no longer
  // needed. Release it here, before any status goes out.
  ctlConsume(n);

  if (ptLen < 16 + 11) {
    Serial.printf("[CTL] plaintext is %d B — too short for challenge + frame\n",
                  ptLen);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  // Gate 3 — UNCONDITIONAL, every verb, no exceptions (XF-47).
  if (!bleChallengeValid || !ozCtEq(pt, bleChallenge, 16)) {
    Serial.printf("[CTL] challenge %s — denied\n",
                  bleChallengeValid ? "does not match the one issued on this "
                                      "connection"
                                    : "was never read on this connection");
    notifyStatus("UNLOCK_DENIED");
    return true;
  }
  // Burn it. The app reads …0005 before every control write, so requiring a
  // fresh one costs nothing — and without this, one challenge read would
  // authorise every frame sent for the rest of the connection.
  bleChallengeValid = false;

  // Gate 4 — strictly greater. Equal is a replay of the frame we just accepted.
  if (counter <= g_bonds[slot].floor) {
    Serial.printf("[CTL] counter %llu is not above bond %d's floor %llu — replay\n",
                  (unsigned long long)counter, slot,
                  (unsigned long long)g_bonds[slot].floor);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  const uint8_t *frame = pt + 16;
  const size_t flen = (size_t)ptLen - 16;
  if (!ozTuyaFrameOk(frame, flen)) {
    Serial.printf("[CTL] plaintext is authentic but not a valid DP frame (%u B)\n",
                  (unsigned)flen);
    notifyStatus("UNLOCK_DENIED");
    return true;
  }

  // Advance the floor BEFORE executing. If execution reboots the lock (or the
  // MCU write hangs), the frame must not become replayable on the way back up —
  // an unlock that maybe happened is a far better outcome than one that can be
  // repeated by anyone who captured it.
  g_bonds[slot].floor = counter;
  ozBondsSave();

  Serial.printf("[CTL] OPENED — bond %d, counter %llu, DP %u\n", slot,
                (unsigned long long)counter, frame[6]);
  ozControlDispatch(slot, frame, flen);
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
void bleSetBusy(bool busy) {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (adv == nullptr) return;
  BLEAdvertisementData scanResp;
  String sd;
  sd += (char)(busy ? 0x01 : 0x00);
  scanResp.setServiceData(BLEUUID(SVC_UUID), sd);
  // Writes esp_ble_gap_config_scan_rsp_data_raw immediately, so the byte can be
  // updated mid-advertising without a stop/start cycle.
  adv->setScanResponseData(scanResp);
}

// Connectable (normal) vs scannable-only (busy). MUST stop first — Bluedroid
// ignores an adv-params change while advertising is running, and a silently
// ignored change here would leave a lock permanently non-connectable.
void bleSetConnectable(bool connectable) {
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (adv == nullptr) return;
  BLEDevice::stopAdvertising();
  adv->setAdvertisementType(connectable ? OZ_ADV_TYPE_IND : OZ_ADV_TYPE_SCAN_IND);
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
      bleSetBusy(true);
      bleSetConnectable(false);
      BLEDevice::startAdvertising();
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
      bleSetBusy(false);
      bleSetConnectable(true); // undo the SCAN_IND switch BEFORE re-advertising
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
  // well past one MTU).
  BLECharacteristic *mem = svc->createCharacteristic(CHR_MEMBER, BLECharacteristic::PROPERTY_WRITE);
  mem->setCallbacks(new MemberCB());

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
  bleSetBusy(false); // M3: publish the flag from the first advert, not on first connect
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising as OZLOCK (busy=0)");
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
    // Stamped on TOUCH-DOWN, not on the completed tap, so the clock starts the
    // instant the visitor's finger lands — the wake, Wi-Fi reassociation and
    // broker dial that follow all happen inside the window they just opened.
    lastTouchAt = lastActivityAt;
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
      char k = keyAt(tx, ty);
      Serial.printf("[TOUCH] %d,%d -> key '%c'\n", tx, ty, k ? k : '-');

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

      if (provisioned) openBleWindow("keypad touch"); // re-arms 60 s per tap

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
