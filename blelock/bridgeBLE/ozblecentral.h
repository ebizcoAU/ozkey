/*
 * ozblecentral.h — bridgeBLE's BLE CENTRAL role (XFtposDecisions-138 P3,
 * ozkey-45). Included by bridgeBLE.ino, NOT compiled on its own.
 *
 * 🔴 WHY THIS IS A HEADER AND NOT PART OF THE .ino.
 * Arduino's auto-prototype generator scans ONLY the primary .ino file and
 * hoists a prototype for every top-level function to the top of the
 * translation unit — above any type those prototypes reference. The helpers
 * below return/take `OzBleLock *`, so as .ino code they generated prototypes
 * that named a struct not yet declared ("'OzBleLock' does not name a type"),
 * and a forward declaration could not fix it: the generator inserts its block
 * higher than any line you can put one on. The generator does not scan
 * #included headers, which is exactly why common/ozdoorlock_core.h is a header
 * too — see its own note on this. Same problem, same established fix.
 *
 * Included at a point in bridgeBLE.ino where mqttClient, cfgSiteId and the
 * logging helpers already exist — it is a textual continuation of that file,
 * not a standalone module, and deliberately has no include guard duplication
 * beyond the one below.
 */
#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// P3 — BLE CENTRAL: scan for OZKIE/CH584M locks, relay commands both ways
// (XFtposDecisions-138 P3, ozkey-45)
//
// This is the BLE mirror of the Thread relay above. Same job, same two
// directions, different transport:
//
//   downlink  MQTT `envelope_hex` -> lock's `control` characteristic
//   uplink    lock's `member` notify (chunked JSON) -> MQTT locks/<id>/uplink
//   status    lock's `status` notify -> serial diagnostics only
//
// 🔴 WHICH GATT CONTRACT THIS SPEAKS, AND WHY IT IS *NOT* XF-138's UUIDs.
// XFtposDecisions-138's appendix specifies a new service (0000FFF1-4:
// Command/Status/OTA/Handshake). This code deliberately targets the EXISTING
// OZKIE service (`4f5a4b31-...`, blelock/CONTRACT.md) instead, because that
// is what `dlch584m_19` — the CH584M simulator, the only thing there is to
// talk to right now — actually serves today. Building against FFF1-4 would
// produce a bridge that compiles, looks finished, and cannot connect to a
// single real device on this bench.
//
// The mapping is 1:1, so this is a UUID-constant swap when PM answers the
// open question in ozkey-45.md §1 (dual service, or replace):
//   XF-138 Command   <-> CHR_CONTROL  ...0006  (write, sealed envelope in)
//   XF-138 Status    <-> CHR_STATUS   ...0003  (notify, wire status strings)
//   XF-138 Handshake <-> CHR_CHALLENGE...0005  (read, M4 challenge)
//   XF-138 OTA       <-> (none yet — no OTA path exists on either side; the
//                         server has none either, see S4's 🔴 in XF-138)
//
// 🔴 TASK SEPARATION IS LOAD-BEARING, NOT STYLE. Scan results and GATT
// notifications arrive on the BLE stack's own task. Two things must never
// happen there: (1) calling connect() from a scan callback — it re-enters the
// stack and deadlocks; (2) touching PubSubClient, which is not thread-safe and
// would corrupt the MQTT session under the main loop. So both callbacks do
// nothing but copy into a queue under a critical section, and ozBleCentralTick()
// — called from loop(), on the main task — does all connecting and publishing.
// ═══════════════════════════════════════════════════════════════════════════

// Same UUIDs the lock serves (common/ozdoorlock_core.h:179-189). Duplicated as
// literals rather than #included: that header pulls in the whole doorlock
// firmware (crypto, profiles, LCD), which this sketch is not and must not be.
#define OZL_SVC_UUID       "4f5a4b31-0001-4c4f-434b-000000000001"
#define OZL_CHR_STATUS     "4f5a4b31-0003-4c4f-434b-000000000001"
#define OZL_CHR_INFO       "4f5a4b31-0004-4c4f-434b-000000000001"
#define OZL_CHR_CONTROL    "4f5a4b31-0006-4c4f-434b-000000000001"
// P5.5 — the lock's RELAY inlet. `control` is the app's direct M4 path and
// demands a challenge this bridge cannot produce (it holds no key material by
// design), so a server envelope written there dies as "plaintext too short for
// challenge + frame". `command` takes the same sealed envelope with no
// challenge — the disposition MQTT/Thread always had. XFtposDecisions-138 P5.5.
#define OZL_CHR_COMMAND    "0000FFF1-0000-1000-8000-00805F9B34FB"
#define OZL_CHR_MEMBER     "4f5a4b31-0007-4c4f-434b-000000000001"
// The bridge's time-push outlet — see the lock's CHR_TIME comment
// (ozdoorlock_core.h) for the full reasoning. Write-only from here.
#define OZL_CHR_TIME       "4f5a4b31-0008-4c4f-434b-000000000001"

// ── CEILING RAISED TO 10 BY A REBUILT LIB SET (P6, 2026-08-30) ────────────
//
// This was 10, justified as "well inside the stack's ceiling — 70 concurrent
// connections on NimBLE / 50 on Bluedroid". That justification was WRONG, and
// the error is worth spelling out because it is easy to repeat: 70/50 are the
// stack's maxima WHEN COMPILED WITH THOSE VALUES. What we actually link
// against is arduino-esp32 3.3.11's PRECOMPILED libs, and its sdkconfig
// (~/Library/Arduino15/packages/esp32/tools/esp32c6-libs/3.3.11/sdkconfig)
// says:
//
//     CONFIG_BT_NIMBLE_ENABLED=y
//     CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3
//     CONFIG_BT_NIMBLE_ROLE_CENTRAL=y + ROLE_PERIPHERAL=y
//
// THREE, shared across BOTH roles — and this bridge is dual-role, because it
// still serves its own provisioning service to the phone. So the working
// budget is 2 locks + 1 phone, or 3 locks and no phone. The bench ran 2 locks
// and never hit the wall, which is exactly why a wrong constant survived: 10
// was never reachable, and nothing failed to prove it.
//
// Raising it needs a REBUILT lib set (esp32-arduino-lib-builder with a higher
// MAX_CONNECTIONS) or a native ESP-IDF project that owns its own sdkconfig —
// arduino-cli cannot override a value baked into a static library. Until one
// of those happens, a larger number here would be a lie that fails at the
// fourth lock.
//
// 🟢 DONE (P6): arduino-esp32 3.3.11's C6 libs were REBUILT with
// CONFIG_BT_NIMBLE_MAX_CONNECTIONS=10 (and MAX_BONDS=10 — a bridge that can
// hold N links but bond fewer is a trap that only shows under load). Verified
// against the INSTALLED sdkconfig, not the build output. Stock libs are backed
// up at esp32c6-libs/3.3.11.STOCK-BACKUP.
//
// 🔴 THIS IS A LOCAL ARTEFACT ANY CORE UPGRADE WILL SILENTLY OVERWRITE, which
// reverts the ceiling to 3 and makes the bridge start refusing the 4th lock
// with no obvious cause. Re-run docs/P6-lib-rebuild.md after every
// arduino-esp32 update. That maintenance burden is the accepted cost of not
// migrating to native ESP-IDF (XFtposDecisions-138 §P6 option 2).
//
// Still shared across BOTH roles (central + peripheral), so the working budget
// is 9 locks + 1 phone, not 10 + 1.
#define OZ_BLE_MAX_LOCKS 9
#define OZ_BLE_SCAN_SECS 5
// RECONNECTION IS THE RESCAN, deliberately — there is no separate per-lock
// retry timer. A dropped link is reaped in ozBleCentralTick(), which frees the
// slot; the lock itself resumes advertising when it loses the connection, so
// the next scan finds it exactly like a new one and the normal connect path
// runs. One mechanism, not two that can disagree.
//
// ── ADAPTIVE SCAN RATE (operator, 2026-08-30) ─────────────────────────────
//
// A fixed 30 s rescan is wrong in both directions at once: too slow when a
// paired lock is missing (that is an OUTAGE — a door the bridge cannot reach),
// and pure waste when every paired lock is already connected, because a scan
// then can only ever find strangers we are going to reject anyway.
//
// So the rate follows the one number that matters: how many PAIRED locks are
// currently unreachable.
//   • any missing  -> FAST. We are hunting a door that should be here.
//   • none missing -> IDLE. Everything we own is connected; scanning is now
//                     only a watchdog against a lock that dropped between
//                     reaps, so it can be slow and cheap.
//
// This is not just a power tweak on a mains-powered bridge — scanning shares
// the one 2.4 GHz front end with the BLE links themselves and (when Wi-Fi is
// up) with Wi-Fi. Not scanning when there is nothing to find is airtime handed
// back to the connections that are actually carrying traffic.
#define OZ_BLE_RESCAN_FAST_MS 10000UL   // a paired lock is missing — hunt
#define OZ_BLE_RESCAN_IDLE_MS 120000UL  // all present — watchdog only
// Stalled-reassembly timeout. ozNotifyChunked() sleeps 30 ms BETWEEN chunks,
// so 1 s is ~30x the real inter-chunk gap — long enough never to cut a healthy
// multi-chunk message, short enough that a dropped chunk cannot poison the
// next one. Same role as the lock's OZ_CTL_IDLE_MS.
#define OZ_BLE_RX_IDLE_MS 1000UL
#define OZ_BLE_PENDING   8
#define OZ_BLE_UPQ       8

struct OzBleLock {
  bool     inUse = false;
  String   addr;                       // BLE address — the key before identity
  String   deviceId;                   // "ozk-…", read from `info` after connect
  BLEClient *client = nullptr;
  BLERemoteCharacteristic *chrControl = nullptr; // app-direct M4 (unused by us)
  BLERemoteCharacteristic *chrCommand = nullptr; // OUR relay inlet (P5.5)
  BLERemoteCharacteristic *chrMember  = nullptr;
  BLERemoteCharacteristic *chrStatus  = nullptr;
  BLERemoteCharacteristic *chrTime    = nullptr;
  bool timePushed = false;             // one push per connection is enough
  String   rxBuf;                      // chunked-notify reassembly (see below)
  unsigned long rxLastChunkAt = 0;     // for the stalled-reassembly timeout
  unsigned long connectedAt = 0;
};
static OzBleLock g_bleLocks[OZ_BLE_MAX_LOCKS];

// ── XF-144 P2 — server-requested targeted scan ──────────────────────────────
//
// "Can THIS bridge reach lock X right now" — a one-shot, bounded scan, not the
// adaptive discovery loop above. Scoped deliberately narrow for v1: a raw BLE
// advertisement carries only the MAC + RSSI, never the device_id (that only
// comes from a full connect + `info` GATT read, done today in
// ozBleConnectOne()). Connecting to every stranger seen during a scan window
// just to identify it would be the exact kind of extra central-role radio
// activity XF-143 already showed can hurt a concurrent peripheral/central op —
// worse here, since it could run while OTHER paired locks are actively
// connected, not just during an idle claim window.
//
// So v1 matches ONLY against identity this bridge already knows: the
// currently-paired table (g_bleLocks) and the seen-but-unpaired cache
// (g_seenUnpaired) both already map deviceId -> last-known address. If the
// target has never been seen by this specific bridge, the honest answer is
// "not found" — no stranger-connecting. This is the useful case anyway: the
// failover ask is "which of my ALREADY-associated candidate bridges can best
// reach this lock", not cold discovery of a lock this bridge has zero history
// with.
struct OzScanRequest {
  bool active = false;
  String target;         // device_id being hunted
  String knownAddr;       // resolved once at request time; "" = never seen here
  unsigned long deadlineMs = 0;
  bool found = false;
  int rssi = 0;
};
static OzScanRequest g_scanReq;

// ═══════════════════════════════════════════════════════════════════════════
// P5 F1-F3 — PAIRING: which locks is this bridge actually responsible for?
//
// Before this, the bridge connected to ANY device advertising the OZKIE
// service. On a two-lock bench that looks like success; in a building it
// means we connect a neighbour's lock, burn our ten slots on locks that are
// not ours, and hold a stranger's lock away from ITS bridge. See
// XFtposDecisions-138 §P5.
//
// 🔴 THIS IS ROUTING AUTHORISATION, NOT CRYPTOGRAPHIC TRUST, and the
// distinction is what keeps it small. The lock's bond #0 is with the APP; the
// bridge relays sealed envelopes it cannot decrypt and holds no key material
// (CONTRACT-BRIDGE.md — "relay, not authority"). So this list decides WHICH
// LOCKS THIS BRIDGE CARRIES MAIL FOR. It does not decide who may unlock —
// that is still the sealed envelope, the bond, the challenge and the counter
// floor, none of which this file touches. Deliberately NO new crypto here.
//
// 🔴 WHY PAIRING IS KEYED ON device_id AND NOT ON THE BLE ADDRESS. Same
// reason the connection table is (see ozBleConnectOne's identity read): the
// radio address is NOT the identity. Measured on this bench — LockA advertises
// `ac:eb:e6:39:f8:c6` but IS `ozk-acebe639f8c4`, and LockB advertises
// `b0:a6:04:8b:5f:da` but IS `ozk-b0a6048b5fd8` — MAC+2 in both cases. Worse,
// CONTRACT.md:104/569 commits us to RPA rotation, after which the address
// CHANGES BY DESIGN. An allowlist keyed on address would silently start
// refusing our own locks.
//
// The unavoidable consequence: a lock's identity is only knowable AFTER
// connecting and reading `info`. So an unknown lock is connected, identified,
// and then dropped if it is not ours — it is never HELD. The seen-but-unpaired
// list (F3) is what the app will offer the user to pair, and the cooldown is
// what stops us reconnecting to the same stranger every scan.
// ═══════════════════════════════════════════════════════════════════════════
#define OZ_BLE_MAX_PAIRED 10
#define OZ_BLE_UNPAIRED_SLOTS 8
#define OZ_BLE_UNPAIRED_COOLDOWN_MS 300000UL // 5 min before re-probing a stranger

static String  g_pairedIds[OZ_BLE_MAX_PAIRED];
static uint8_t g_pairedCount = 0;

// F3 — locks we can SEE but are not paired to. This is the list the app needs
// in order to offer "pair this doorlock". Kept in RAM only: it is a live
// observation, and a stale one restored from flash would offer the user a
// lock that is no longer in range.
struct OzBleSeen { String deviceId; String addr; int rssi; unsigned long at; };
static OzBleSeen g_seenUnpaired[OZ_BLE_UNPAIRED_SLOTS];

// Defined further down (it uses ozBlePairAdd/Remove and the seen-list), but
// called from ozBleCentralTick() above them.
static void ozBleSerialConsole();

static uint8_t ozBlePairedTotal() { return g_pairedCount; }

static bool ozBleIsPaired(const String &deviceId) {
  for (uint8_t i = 0; i < g_pairedCount; i++)
    if (g_pairedIds[i] == deviceId) return true;
  return false;
}

/** Persist the allowlist. Stored as one newline-joined string rather than N
 *  keys: NVS keys are capped at 15 chars, and "paired0".."paired9" would work
 *  but leaves orphans behind when the list shrinks. One key is atomic. */
static void ozBleSavePaired() {
  String blob;
  for (uint8_t i = 0; i < g_pairedCount; i++) { blob += g_pairedIds[i]; blob += '\n'; }
  prefs.begin("bridge32", false);
  prefs.putString("paired", blob);
  prefs.end();
  Serial.printf("[PAIR] saved %u paired lock(s)\n", g_pairedCount);
}

static void ozBleLoadPaired() {
  prefs.begin("bridge32", true);
  const String blob = prefs.getString("paired", "");
  prefs.end();
  g_pairedCount = 0;
  int start = 0;
  while (start < (int)blob.length() && g_pairedCount < OZ_BLE_MAX_PAIRED) {
    int nl = blob.indexOf('\n', start);
    if (nl < 0) nl = blob.length();
    const String id = blob.substring(start, nl);
    if (id.length()) g_pairedIds[g_pairedCount++] = id;
    start = nl + 1;
  }
  Serial.printf("[PAIR] loaded %u paired lock(s)%s\n", g_pairedCount,
                g_pairedCount ? "" : " — bridge is UNPAIRED, will hold no locks");
  for (uint8_t i = 0; i < g_pairedCount; i++)
    Serial.printf("[PAIR]   %s\n", g_pairedIds[i].c_str());
}

/** F4 (half): add/remove. Transport-neutral on purpose — whichever of MQTT or
 *  BLE the app/server teams choose (XF-138 A6) calls these. */
static bool ozBlePairAdd(const String &deviceId) {
  if (!deviceId.length()) return false;
  if (ozBleIsPaired(deviceId)) return true;            // idempotent
  if (g_pairedCount >= OZ_BLE_MAX_PAIRED) {
    Serial.printf("[PAIR] REFUSED %s — already at %d paired locks\n",
                  deviceId.c_str(), OZ_BLE_MAX_PAIRED);
    return false;
  }
  g_pairedIds[g_pairedCount++] = deviceId;
  ozBleSavePaired();

  // 🔴 CLEAR THE SEEN-UNPAIRED ENTRY, or this lock cannot be connected for up
  // to OZ_BLE_UNPAIRED_COOLDOWN_MS (5 min) AFTER being paired.
  //
  // Found on the bench 2026-08-30, and it is worth naming because it looked
  // exactly like "pairing does not work": both locks paired successfully, the
  // allowlist persisted correctly, and neither ever reconnected. The cooldown
  // that stops us re-probing strangers is keyed on ADDRESS and consulted in
  // the scan callback — long before we know the lock is now ours — so a
  // freshly-paired lock kept being skipped by the very guard that exists for
  // locks we rejected. Pairing must therefore retract the rejection.
  for (auto &s : g_seenUnpaired) {
    if (s.deviceId == deviceId) { s = OzBleSeen(); break; }
  }

  Serial.printf("[PAIR] + %s (now %u) — will connect on the next scan\n",
                deviceId.c_str(), g_pairedCount);
  return true;
}

static bool ozBlePairRemove(const String &deviceId) {
  for (uint8_t i = 0; i < g_pairedCount; i++) {
    if (g_pairedIds[i] != deviceId) continue;
    for (uint8_t j = i; j + 1 < g_pairedCount; j++) g_pairedIds[j] = g_pairedIds[j + 1];
    g_pairedIds[--g_pairedCount] = "";
    ozBleSavePaired();
    Serial.printf("[PAIR] - %s (now %u)\n", deviceId.c_str(), g_pairedCount);
    // Drop the live link too, or the lock stays connected until it happens to
    // disconnect — an unpair that leaves the connection up is not an unpair.
    for (auto &l : g_bleLocks) {
      if (l.inUse && l.deviceId == deviceId && l.client) {
        Serial.printf("[PAIR] dropping live link to %s\n", deviceId.c_str());
        l.client->disconnect();
      }
    }
    return true;
  }
  return false;
}

/** F3 — note a lock we saw but are not paired to, for the app's pairing UI.
 *
 *  🔴 MUST re-publish the roster on a genuinely NEW entry. The only other
 *  publish trigger is ozBleCentralTick()'s ON/TOTAL repaint gate, and
 *  discovering-but-not-pairing a lock changes neither number — so before
 *  this call existed, `g_seenUnpaired` filled correctly in RAM (confirmed
 *  live in the serial log's repeated "offered for pairing" lines) while the
 *  app's `locks` characteristic stayed frozen at whatever it read at boot
 *  (empty), forever. The app reported "no unpaired lock nearby" while the
 *  bridge's own console `seen` command showed every lock, and both were
 *  telling the truth about two different things. Observed live 2026-08-30
 *  blocking App's first end-to-end pairing retry after dlch584m-0.12.
 *
 *  Not called on the refresh branch above — that fires every scan cycle
 *  for an already-known device (rssi/age churn only) and would notify BLE
 *  continuously for no new information the app doesn't already have. */
static void ozBlePublishRoster(); // defined below; forward decl for the new-entry branch
static void ozBleNoteUnpaired(const String &deviceId, const String &addr, int rssi) {
  for (auto &s : g_seenUnpaired) {
    if (s.deviceId == deviceId) { s.addr = addr; s.rssi = rssi; s.at = millis(); return; }
  }
  // New: take a free slot, else the oldest.
  OzBleSeen *victim = &g_seenUnpaired[0];
  for (auto &s : g_seenUnpaired) {
    if (!s.deviceId.length()) { victim = &s; break; }
    if (s.at < victim->at) victim = &s;
  }
  victim->deviceId = deviceId; victim->addr = addr; victim->rssi = rssi;
  victim->at = millis();
  ozBlePublishRoster();
}

/** Record a probe that FAILED before we learned an identity, so it gets the
 *  same backoff as a rejected stranger.
 *
 *  🔴 WITHOUT THIS THE BRIDGE HOT-LOOPS. ozBleNoteUnpaired() is only reached
 *  on the not-paired branch; an identification FAILURE returns earlier and
 *  recorded nothing, so ozBleRecentlyRejected() stayed false and the scanner
 *  re-probed the same device every cycle — 10 s apart in discovery mode.
 *  Each probe holds the lock in SCAN_IND (non-connectable), so this starved
 *  the APP of the very lock the user was trying to commission. Observed on
 *  the bench 2026-08-30 blocking a live pairing attempt.
 *
 *  Keyed on ADDRESS with an empty device_id: the roster skips empty entries,
 *  so a device we could not identify is never offered as pairable. */
static void ozBleNoteProbeFailed(const String &addr) {
  for (auto &s : g_seenUnpaired) {
    if (s.addr == addr) { s.at = millis(); return; }
  }
  OzBleSeen *victim = &g_seenUnpaired[0];
  for (auto &s : g_seenUnpaired) {
    if (!s.addr.length()) { victim = &s; break; }
    if (s.at < victim->at) victim = &s;
  }
  victim->deviceId = ""; victim->addr = addr; victim->rssi = 0;
  victim->at = millis();
}

/** True if we probed this stranger recently — do not reconnect just to learn
 *  the same answer. Keyed on ADDRESS because that is all a scan result gives
 *  us before connecting. */
static bool ozBleRecentlyRejected(const String &addr) {
  for (auto &s : g_seenUnpaired) {
    if (s.addr == addr && s.at && millis() - s.at < OZ_BLE_UNPAIRED_COOLDOWN_MS)
      return true;
  }
  return false;
}

// Scan callback -> loop() handoff. Addresses only; no BLE object outlives the
// callback (BLEAdvertisedDevice is a stack copy there).
static String   g_blePending[OZ_BLE_PENDING];
static uint8_t  g_blePendingCount = 0;

// Notify callback -> loop() handoff for anything bound for MQTT.
// `inUse` claims the slot (set under the lock); `ready` says the payload is
// fully written and safe to publish. Two flags, because the write happens
// OUTSIDE the lock — see ozBleQueueUplink().
struct OzBleUp { volatile bool inUse = false; volatile bool ready = false;
                 String deviceId; String payload; };
static OzBleUp  g_bleUpQ[OZ_BLE_UPQ];

static portMUX_TYPE g_bleMux = portMUX_INITIALIZER_UNLOCKED;
static String g_lastInfoRaw;   // last `info` payload, for the failure log
static unsigned long g_bleLastScanAt = 0;
static bool g_bleCentralReady = false;
static uint32_t g_bleUpRelayed = 0, g_bleUpDropped = 0, g_bleCmdSent = 0;

// XF-149 — mqttMessageReceived() -> ozBleCentralTick() handoff for relay
// writes. forwardOverBle() used to call sink->writeValue(..., true) directly
// from inside the MQTT callback: a synchronous write-with-response that, under
// BLE link contention, can block for seconds. While blocked, the WHOLE
// Arduino loop() is stuck — no second mqttClient.loop() to pick up a buffered
// message, no ozBleCentralTick() reaping/uplink service, and MQTT's own
// keepalive (also serviced inside loop()) at risk of lapsing. Same call path
// every relayed command (unlock, grant_pin, grant_rfid, delete_pin,
// delete_rfid, factory_reset, unpair_lock) goes through, so this affects all
// of them, not just one. Traced live twice (XF-147, XF-149) and deliberately
// left unfixed both times pending a real structural change — this is it.
//
// No mutex needed: unlike g_blePending (fed from the BLE scan callback, a
// different task), the producer (mqttMessageReceived(), via mqttClient.loop())
// and the consumer (ozBleCentralTick()) both run from the same main loop()
// on the same task, never concurrently.
struct OzBleCmdWrite { String deviceId; String fieldName; uint8_t buf[600]; size_t len = 0; };
#define OZ_BLE_CMDQ 4
static OzBleCmdWrite g_bleCmdQ[OZ_BLE_CMDQ];
static uint8_t g_bleCmdQHead = 0, g_bleCmdQCount = 0;

/** Variable-length hex -> bytes. The file's own hexToBytes() takes a FIXED
 *  expected length (dataset fields); an envelope is any length. */
static size_t ozHexToBytesVar(const String &hex, uint8_t *out, size_t cap) {
  size_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < hex.length(); i++) {
    const char c = hex[i];
    if (c == ' ' || c == ':') continue;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return 0;                       // not hex at all — reject the lot
    if (hi < 0) { hi = v; }
    else { if (n >= cap) return 0; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return (hi < 0) ? n : 0;               // odd nibble count = malformed
}

static OzBleLock *ozBleFindByAddr(const String &addr) {
  for (auto &l : g_bleLocks) if (l.inUse && l.addr == addr) return &l;
  return nullptr;
}
static OzBleLock *ozBleFindByDeviceId(const String &id) {
  for (auto &l : g_bleLocks) if (l.inUse && l.deviceId == id) return &l;
  return nullptr;
}
// XF-144 P2 — last-known address for a device_id, from either identity source
// this bridge has: a currently-held slot, or the seen-but-unpaired cache
// (P5 F2). "" means this bridge has no record of ever hearing from it.
static String ozBleLastKnownAddr(const String &deviceId) {
  OzBleLock *l = ozBleFindByDeviceId(deviceId);
  if (l) return l->addr;
  for (auto &s : g_seenUnpaired) if (s.deviceId == deviceId) return s.addr;
  return String("");
}
static bool ozBleConnected(const OzBleLock &l) {
  return l.inUse && l.client && l.client->isConnected();
}
/** For the Thread liveness sweep's g_onlineLocks[] guard (bridgeBLE.ino) —
 *  true if we currently hold a live BLE link to this device_id, so a lock
 *  that was never a Thread child in the first place doesn't get retracted
 *  as "left the child table" every liveness tick. */
static bool ozBleLockConnected(const String &id) {
  OzBleLock *l = ozBleFindByDeviceId(id);
  return l && ozBleConnected(*l);
}
/** XF-144 Phase 2 — is deviceId currently visible over BLE (seen in a recent
 *  scan, whether or not paired here)? Reuses g_seenUnpaired, the same
 *  visibility data ozBleStartTargetedScan()'s match logic already reads.
 *  Lives here, not in bridgeBLE.ino's peer-takeover code, because
 *  g_seenUnpaired is only in scope after this header's include point — same
 *  reason ozBleLockConnected() above is forward-declared and defined here.
 *  30s freshness window: long enough to survive one scan gap, short enough
 *  that a lock that walked out of range does not look reachable forever. */
static bool ozBleCurrentlyVisible(const String &deviceId) {
  for (auto &s : g_seenUnpaired) {
    if (s.deviceId == deviceId && s.at && millis() - s.at < 30000UL) return true;
  }
  return ozBleLockConnected(deviceId); // already connected also counts as visible
}

/** XF-144 Phase 2 — our own roster, broadcast periodically so peers know
 *  what to take over if we go dark. Lives here, not in bridgeBLE.ino's
 *  peer-roster code, because g_pairedIds/g_pairedCount (the allowlist —
 *  the same source pair_lock/unpair_lock already maintain, not a second
 *  notion of "what do I hold") are only in scope after this header's
 *  include point. threadUdp/deviceId/OZ_THREAD_UDP_PORT are all declared
 *  in bridgeBLE.ino BEFORE the #include, so still visible here. */
static bool sendBridgeRosterBeacon() {
  if (!threadUdpReady) return false;
  JsonDocument doc;
  doc["from"] = deviceId;
  doc["kind"] = "bridge_roster";
  JsonArray locks = doc["locks"].to<JsonArray>();
  for (uint8_t i = 0; i < g_pairedCount; i++) locks.add(g_pairedIds[i]);
  String out;
  serializeJson(doc, out);

  IPAddress all;
  if (!all.fromString("ff03::1")) return false;
  // 🔴 PORT 5053, NOT 5052. Found live 2026-09-02 on the first real-hardware
  // test of this feature: rosters were transmitting correctly every 15s and
  // NOBODY was receiving them. sendTimeBeacon() — which this was modelled on —
  // sends to OZ_THREAD_UDP_PORT (5052) because its audience is LOCKS, which
  // listen there. Bridges listen on OZ_THREAD_UPLINK_PORT (5053), the uplink
  // RX socket pollUplinkOne() reads. A bridge-to-bridge message sent to 5052
  // is transmitted perfectly and lands where no bridge is listening — the
  // send-side log line looks completely healthy, which is exactly why this
  // took a real two-bridge test to catch rather than a code read.
  if (!threadUdp.beginPacket(all, OZ_THREAD_UPLINK_PORT)) return false;
  threadUdp.write((const uint8_t *)out.c_str(), out.length());
  if (!threadUdp.endPacket()) {
    Serial.println("[PEER] roster beacon endPacket failed");
    return false;
  }
  g_lastRosterBroadcastAt = millis();
  Serial.printf("[PEER] roster -> ff03::1 (%u lock(s))\n", (unsigned)g_pairedCount);
  return true;
}
/** How many locks this bridge currently holds a live BLE link to — drawn on
 *  the panel, so the central role is observable without a serial cable. */
static uint8_t ozBleConnectedCount() {
  uint8_t n = 0;
  for (auto &l : g_bleLocks) if (ozBleConnected(l)) n++;
  return n;
}

/** Total nodes this bridge OWNS — i.e. the paired allowlist. Declared here
 *  (not in the pairing block below) because the panel and the scan-rate logic
 *  both need it, and both sit above that block. */
static uint8_t ozBlePairedTotal();

/** Paired locks we currently have NO link to. This is the number that drives
 *  both the adaptive scan rate and the panel's OFF figure — an unreachable
 *  paired lock is a door the bridge cannot open, which is the only BLE fact
 *  on this panel that represents an outage. */
static uint8_t ozBleMissingCount() {
  const uint8_t total = ozBlePairedTotal();
  const uint8_t up = ozBleConnectedCount();
  return (total > up) ? (uint8_t)(total - up) : 0;
}

/** Queue one uplink payload for the main loop to publish. Runs on the BLE
 *  task — must not touch MQTT here (see the header note). */
static void ozBleQueueUplink(const String &deviceId, const String &payload) {
  // 🔴 NO String ASSIGNMENT INSIDE THE CRITICAL SECTION. Assigning an Arduino
  // String can call malloc(), and allocating with interrupts disabled on ESP32
  // can deadlock or abort. The critical section therefore only CLAIMS a slot
  // (a bool write); the copy happens outside it. Claiming first is what makes
  // that safe — once `inUse` is set no other caller will touch this slot, so
  // the unlocked write cannot race the drain side.
  int idx = -1;
  portENTER_CRITICAL(&g_bleMux);
  for (int i = 0; i < OZ_BLE_UPQ; i++) {
    if (!g_bleUpQ[i].inUse) { g_bleUpQ[i].inUse = true; idx = i; break; }
  }
  portEXIT_CRITICAL(&g_bleMux);

  const bool placed = (idx >= 0);
  if (placed) {
    g_bleUpQ[idx].deviceId = deviceId;   // outside the critical section
    g_bleUpQ[idx].payload  = payload;
    g_bleUpQ[idx].ready    = true;       // publish only once fully written
  }
  if (!placed) {
    g_bleUpDropped++;
    // Loud, and counted. A silently dropped uplink is the exact class of
    // invisible loss the Thread path's own "broker down, uplink DROPPED"
    // line exists to prevent.
    Serial.printf("[BLE-UP] %s: QUEUE FULL — uplink DROPPED (total=%lu)\n",
                  deviceId.c_str(), (unsigned long)g_bleUpDropped);
  }
}

// GATT notification — BLE task. Identify which lock by matching the
// characteristic pointer, then reassemble and hand off.
static void ozBleNotifyCb(BLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool) {
  for (auto &l : g_bleLocks) {
    if (!l.inUse) continue;

    if (chr == l.chrStatus) {
      // Wire status strings ("BLE_OK", "UNLOCK_OK", "UNLOCK_DENIED"…).
      // Diagnostics only: these are the app-ceremony channel, not the audit
      // trail, and inventing an MQTT topic for them would be firmware
      // deciding a server contract on its own. The sealed `member` path
      // below is the one that carries real outcomes.
      String s; s.reserve(len);
      for (size_t i = 0; i < len; i++) s += (char)data[i];
      Serial.printf("[BLE-ST] %s: %s\n",
                    l.deviceId.length() ? l.deviceId.c_str() : l.addr.c_str(), s.c_str());
      return;
    }

    if (chr == l.chrMember) {
      // Chunked JSON, ozNotifyChunked()'s convention (ozdoorlock_core.h):
      // no length prefix, no end marker — "it parses" IS the end marker, and
      // a chunk starting a fresh document restarts the buffer. Mirrored here
      // exactly rather than reinvented.
      String piece; piece.reserve(len);
      for (size_t i = 0; i < len; i++) piece += (char)data[i];

      // ── 🔴 DO NOT RESET ON A NESTED '{' — SILENT DATA LOSS ───────────────
      //
      // This read `if (piece[0]=='{' || piece[0]=='[') rxBuf = "";` — a
      // straight port of ozNotifyChunked()'s "a chunk starting '{' restarts
      // the buffer" rule. That rule is only safe for a FLAT payload.
      //
      // The lock's `list_bonds` reply is NOT flat: it is
      // `[{"slot":0,...},{"slot":1,...}]`, and each bond object is ~100+ B
      // (`pub` alone is 64 hex chars). At ozNotifyChunked()'s 180 B chunks, a
      // boundary landing on a NESTED '{' is not merely possible with two or
      // more bonds — it is likely. When it happened we discarded everything
      // buffered so far and never recovered: the whole reply lost, silently,
      // with nothing in the log to say so.
      //
      // Credit where due: the APP team hit this independently while writing
      // the test for their `locks`-roster reassembler (XF-138, 2026-08-30)
      // and flagged it back. Their payload nests too (`seen:[{...}]`) —
      // because I specified it that way. Same defect, two implementations,
      // found by someone testing rather than reading.
      //
      // Fix, matching theirs: only reset when the buffer is EMPTY. Once we
      // are accumulating, a '{' is just more of the same message.
      if (l.rxBuf.length() && millis() - l.rxLastChunkAt > OZ_BLE_RX_IDLE_MS) {
        // Their fix alone leaves one hole: a DROPPED chunk means the buffer
        // never completes and never resets, so it poisons every subsequent
        // message on this link forever. The lock's own `ctlBuf` solved this
        // years ago with an idle timer (OZ_CTL_IDLE_MS) — same answer here.
        Serial.printf("[BLE-UP] %s: reassembly stalled %lums — buffer reset\n",
                      l.deviceId.c_str(), millis() - l.rxLastChunkAt);
        l.rxBuf = "";
      }
      l.rxLastChunkAt = millis();
      l.rxBuf += piece;

      JsonDocument probe;
      if (deserializeJson(probe, l.rxBuf) == DeserializationError::Ok) {
        ozBleQueueUplink(l.deviceId, l.rxBuf);
        l.rxBuf = "";
      } else if (l.rxBuf.length() > 4096) {
        // Never let a malformed stream grow without bound.
        Serial.printf("[BLE-UP] %s: reassembly buffer overflow — reset\n",
                      l.deviceId.c_str());
        l.rxBuf = "";
      }
      return;
    }
  }
}

class OzBleScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!dev.haveServiceUUID() || !dev.isAdvertisingService(BLEUUID(OZL_SVC_UUID))) return;
    const String addr = dev.getAddress().toString();

    // XF-144 P2 — a targeted scan is watching for one specific address this
    // tick, independent of (and not gated by) the normal pairing/discovery
    // bookkeeping below, since the target may well be a lock this bridge does
    // NOT hold and never will.
    if (g_scanReq.active && g_scanReq.knownAddr.length() && addr == g_scanReq.knownAddr) {
      g_scanReq.found = true;
      g_scanReq.rssi = dev.getRSSI();
    }

    if (ozBleFindByAddr(addr)) return;            // already known/connected
    // P5 F2 — a stranger we probed recently. Re-connecting would cost a full
    // connect+GATT-discover just to re-learn an identity we already rejected,
    // every 30 s scan, for as long as it stays in range.
    if (ozBleRecentlyRejected(addr)) return;
    portENTER_CRITICAL(&g_bleMux);
    bool dup = false;
    for (uint8_t i = 0; i < g_blePendingCount; i++) if (g_blePending[i] == addr) dup = true;
    if (!dup && g_blePendingCount < OZ_BLE_PENDING) g_blePending[g_blePendingCount++] = addr;
    portEXIT_CRITICAL(&g_bleMux);
    if (!dup) Serial.printf("[BLE-SCAN] OZLOCK found: %s rssi=%d\n", addr.c_str(), dev.getRSSI());
  }
};
static OzBleScanCb g_bleScanCb;

// XF-144 P2 — single source of truth for the scan-request capacity guard,
// forward-declared in bridgeBLE.ino (same reason as forwardOverBle()).
static bool ozBleAtCapacity() { return ozBleConnectedCount() >= OZ_BLE_MAX_LOCKS; }

// XF-144 P2 — arm a bounded, targeted scan. Caller (mqttMessageReceived())
// already applied the busy/capacity guard; this just does the mechanics.
// Forward-declared in bridgeBLE.ino (same reason as forwardOverBle()).
static void ozBleStartTargetedScan(const String &target, uint32_t durationS) {
  g_scanReq = OzScanRequest();
  g_scanReq.active = true;
  g_scanReq.target = target;
  g_scanReq.knownAddr = ozBleLastKnownAddr(target);
  g_scanReq.deadlineMs = millis() + durationS * 1000UL;

  BLEScan *scan = BLEDevice::getScan();
  if (scan) {
    if (scan->isScanning()) scan->stop();
    scan->setAdvertisedDeviceCallbacks(&g_bleScanCb, false);
    scan->setActiveScan(true);
    scan->start(durationS, nullptr, false);
  }
  Serial.printf("[SCAN] started target=%s known_addr=%s duration=%us\n",
                target.c_str(),
                g_scanReq.knownAddr.length() ? g_scanReq.knownAddr.c_str() : "(never seen)",
                (unsigned)durationS);
}

// 2026-09-02 — three small accessors so ozPeerTimeoutTick() (bridgeBLE.ino)
// can drive a sequential per-lock targeted scan without reaching into
// g_scanReq's fields directly, matching this file's existing
// accessor-function convention (ozBleAtCapacity() etc. above).
//
// Why the peer-takeover retry needed this at all, not just a longer wait:
// ozBleCurrentlyVisible() only reads g_seenUnpaired, which is written by
// ozBleNoteUnpaired() — reached only after a full connect+identify, which
// ozBleRecentlyRejected() then throttles to once per 5 MINUTES per address.
// A lock last identified at boot (the common case right after a bridge
// reboots and re-discovers everyone) will not refresh that cache again for
// up to 5 minutes — a passive retry loop checking the same cache gains
// nothing. A targeted scan bypasses this: onResult()'s target-match check
// runs before ozBleRecentlyRejected()'s early return (see onResult() above),
// so it still sees the advertisement even while that address is in its
// rejection cooldown.
static bool ozBleScanSlotFree() { return !g_scanReq.active; }
static bool ozBleScanIsFor(const String &target) {
  return g_scanReq.active && g_scanReq.target == target;
}
static bool ozBleScanFoundFor(const String &target) {
  return ozBleScanIsFor(target) && g_scanReq.found;
}

/** Connect + discover + subscribe. Main task only. */
static bool ozBleConnectOne(const String &addr) {
  OzBleLock *slot = nullptr;
  for (auto &l : g_bleLocks) if (!l.inUse) { slot = &l; break; }
  if (!slot) {
    Serial.printf("[BLE] %s: no free slot (max %d locks)\n", addr.c_str(), OZ_BLE_MAX_LOCKS);
    return false;
  }

  BLEClient *cl = BLEDevice::createClient();
  if (!cl) return false;
  if (!cl->connect(BLEAddress(addr))) {
    Serial.printf("[BLE] %s: connect failed\n", addr.c_str());
    delete cl;
    return false;
  }

  // 🔴 NEGOTIATE MTU BEFORE READING ANYTHING. The default ATT MTU is 23, so a
  // read returns ~20 bytes and anything longer comes back TRUNCATED — not as
  // an error, as a short string that is no longer valid JSON.
  //
  // This bit us on the bench 2026-08-30 in the most misleading way possible:
  // the `info` read "worked" and even began with the right content
  // ('{"device_id":"ozk-acebe639f8c4",...'), but the document was cut off, so
  // deserializeJson() failed and the bridge reported "`info` unreadable — no
  // device_id". Unprovisioned locks parsed FINE because they carry no `name`
  // field and stayed under the limit — so the failure looked lock-specific
  // and intermittent rather than like a missing MTU negotiation.
  //
  // 517 is the ATT maximum; the stack negotiates down to whatever the peer
  // supports, so asking high is free.
  if (!cl->setMTU(517))
    Serial.printf("[BLE] %s: MTU negotiation refused — long reads may truncate\n",
                  addr.c_str());

  BLERemoteService *svc = cl->getService(BLEUUID(OZL_SVC_UUID));
  if (!svc) {
    Serial.printf("[BLE] %s: OZKIE service absent — not a lock, dropping\n", addr.c_str());
    cl->disconnect();
    delete cl;
    return false;
  }

  slot->inUse = true;
  slot->addr = addr;
  slot->client = cl;
  slot->rxBuf = "";
  slot->connectedAt = millis();
  slot->chrControl = svc->getCharacteristic(BLEUUID(OZL_CHR_CONTROL));
  slot->chrCommand = svc->getCharacteristic(BLEUUID(OZL_CHR_COMMAND));
  slot->chrMember  = svc->getCharacteristic(BLEUUID(OZL_CHR_MEMBER));
  slot->chrStatus  = svc->getCharacteristic(BLEUUID(OZL_CHR_STATUS));
  slot->chrTime    = svc->getCharacteristic(BLEUUID(OZL_CHR_TIME));
  slot->timePushed = false;

  // IDENTITY. This is the BLE equivalent of the Thread path's ozNoteLockExt()
  // join: MQTT addresses locks by device_id ("ozk-…"), the radio addresses them
  // by BLE address, and nothing connects the two until the lock tells us. Read
  // `info` and keep the mapping — without it a downlink can never be routed.
  BLERemoteCharacteristic *info = svc->getCharacteristic(BLEUUID(OZL_CHR_INFO));
  g_lastInfoRaw = info ? String("(read pending)") : String("(no info char)");
  String lockFw; // captured for the presence publish below when available
  if (info && info->canRead()) {
    const String js = info->readValue();
    g_lastInfoRaw = js;   // kept for the failure log — diagnosis beats guessing
    JsonDocument d;
    if (deserializeJson(d, js) == DeserializationError::Ok) {
      slot->deviceId = (const char *)(d["device_id"] | "");
      lockFw = (const char *)(d["fw"] | "");
      Serial.printf("[BLE] %s: identified as %s (fw=%s)\n", addr.c_str(),
                    slot->deviceId.c_str(), (const char *)(d["fw"] | "?"));
    } else {
      // ── TRUNCATED `info` — TAKE THE ONE FIELD WE NEED ────────────────────
      //
      // 🔴 THE BRIDGE MUST NOT DEPEND ON PARSING THE WHOLE DOCUMENT. `info`
      // exists for the APP: device_id, mac, fw, name, the 64-hex pubkey,
      // profile, firmware_id, device_type, transport... It grows whenever
      // anyone adds a field, and a PROVISIONED lock is longer than an
      // unprovisioned one because it carries `name`.
      //
      // Bench, 2026-08-30: LockB and LockC parsed fine while LockA — the only
      // one with a name — came back truncated at 316 B, so the bridge called
      // it unidentifiable and hot-looped on it, starving the app of the very
      // lock the operator was trying to commission. Raising the MTU did not
      // move that 316 B boundary, so the limit is elsewhere in the read path.
      //
      // The bridge needs exactly ONE field: device_id. It sits at the front of
      // the document, so it survives truncation. Take it directly rather than
      // making routing depend on the app's payload staying under some size
      // nobody is tracking.
      const int k = js.indexOf("\"device_id\"");
      const int q1 = (k < 0) ? -1 : js.indexOf('"', js.indexOf(':', k) + 1);
      const int q2 = (q1 < 0) ? -1 : js.indexOf('"', q1 + 1);
      if (q1 > 0 && q2 > q1) {
        slot->deviceId = js.substring(q1 + 1, q2);
        Serial.printf("[BLE] %s: identified as %s (info truncated at %u B — "
                      "device_id recovered directly)\n",
                      addr.c_str(), slot->deviceId.c_str(), (unsigned)js.length());
      }
    }
  }
  if (!slot->deviceId.length()) {
    // Fail loud rather than hold a connection we can never address. Same
    // reasoning as the Thread side refusing to publish an unattached dataset.
    Serial.printf("[BLE] %s: `info` unreadable — no device_id, dropping "
                  "(raw %u B: '%.60s')\n",
                  addr.c_str(), (unsigned)g_lastInfoRaw.length(),
                  g_lastInfoRaw.c_str());
    ozBleNoteProbeFailed(addr); // back off — do NOT hot-loop on this device
    cl->disconnect();
    delete cl;
    *slot = OzBleLock();
    return false;
  }

  // ── P5 F2 — IS THIS LOCK OURS? ───────────────────────────────────────────
  //
  // Checked HERE, after the identity read, because it cannot be checked any
  // earlier: a scan result carries only the BLE address, and the address is
  // not the identity (see the pairing block's note — MAC+2 on both bench
  // locks, and RPA rotation will change it by design). So an unknown lock is
  // connected, identified, and released. It is never HELD.
  //
  // Released, not merely ignored: holding a stranger's lock in a connection
  // is a denial of service against ITS bridge, and it burns one of our ten
  // slots. The seen-list + cooldown are what stop us re-probing the same
  // stranger on every scan.
  if (!ozBleIsPaired(slot->deviceId)) {
    Serial.printf("[BLE] %s (%s) is NOT PAIRED to this bridge — released, "
                  "offered for pairing\n", slot->deviceId.c_str(), addr.c_str());
    ozBleNoteUnpaired(slot->deviceId, addr, 0);
    cl->disconnect();
    delete cl;
    *slot = OzBleLock();
    return false;
  }

  if (slot->chrMember && slot->chrMember->canNotify())
    slot->chrMember->registerForNotify(ozBleNotifyCb);
  if (slot->chrStatus && slot->chrStatus->canNotify())
    slot->chrStatus->registerForNotify(ozBleNotifyCb);

  Serial.printf("[BLE] %s CONNECTED (command=%s control=%s member=%s status=%s)\n",
                slot->deviceId.c_str(),
                slot->chrCommand ? "y" : "N",
                slot->chrControl ? "y" : "N", slot->chrMember ? "y" : "N",
                slot->chrStatus ? "y" : "N");
  if (!slot->chrCommand)
    Serial.printf("[BLE] %s has NO `command` char — pre-P5.5 firmware; "
                  "relayed unlocks WILL be denied (needs dlch584m-0.5+)\n",
                  slot->deviceId.c_str());

  // 2026-08-31 — push time once per connection. Same payload shape as
  // sendTimeBeacon() (bridgeBLE.ino), the Thread equivalent, deliberately —
  // the lock's ozHarvestTime() is transport-agnostic and expects this exact
  // field set regardless of which radio carried it.
  if (slot->chrTime && slot->chrTime->canWrite()) {
    const uint32_t utc = ozBridgeUtc();
    if (utc) {
      JsonDocument doc;
      doc["target"] = "*";
      doc["kind"] = "time";
      doc["utc"] = utc;
      doc["utc_src"] = g_utcFromServer ? "server" : "nvs";
      doc["tz"] = cfgTzMin;
      String out;
      serializeJson(doc, out);
      const bool ok = slot->chrTime->writeValue((uint8_t *)out.c_str(), out.length(), true);
      slot->timePushed = ok;
      Serial.printf("[BLE-TIME] %s <- utc=%lu tz=%+d%s\n", slot->deviceId.c_str(),
                    (unsigned long)utc, (int)cfgTzMin, ok ? "" : " WRITE FAILED");
    } else {
      Serial.printf("[BLE-TIME] %s: bridge itself has no clock yet — skipped\n",
                    slot->deviceId.c_str());
    }
  }

  // ── 2026-08-31, PM directive — the BRIDGE reports presence, not the lock ──
  //
  // Confirmed live: a bonded+routed CH584M lock was refused as unreachable
  // (409) even with `caps.remote_unlock=true`, because R6's queue-time check
  // reads `.../presence`, and no CH584M lock has ever published there — the
  // lock-side periodic Thread beacon that normally does this is hardcoded
  // `if (isThread() ...)`, forced false under OZ_NO_MCU, so that whole path
  // never ran. Rather than build the lock-side equivalent (new BLE traffic,
  // battery cost, periodic-timer latency), the bridge already knows this
  // lock's connection state in real time — it's the one holding the link —
  // so it reports on the lock's behalf, event-driven off this very connect.
  //
  // ozBuildLockPresence()/ozPresenceShouldRetain() reused verbatim: this is
  // the SAME topic Thread locks already publish to via the SAME builder
  // (ozpresence.h's whole reason for existing — two producers, one shape,
  // after XF-115 taught this codebase what happens when that's not true).
  // ozClaimLockOnline() is the SAME de-dup table Thread's beacon already
  // shares, keyed only on device_id — publishing here is exactly the "any
  // transport, same online-transition tracking" case it was built for.
  if (ozClaimLockOnline(slot->deviceId.c_str())) {
    // `via` = this bridge's own id — see ozBuildLockPresence()'s doc comment
    // (XF-144, 2026-09-02): lets the server correct locks.bridge_id off a
    // real connect event instead of trusting whatever it last wrote.
    const String out = ozBuildLockPresence(slot->deviceId, true, OZ_PRESENCE_ONLINE,
                                           String(""), lockFw.c_str(), deviceId);
    const String ptopic = "ozkie/" + cfgSiteId + "/locks/" + slot->deviceId + "/presence";
    const bool ok = mqttClient.publish(ptopic.c_str(), out.c_str(),
                                       ozPresenceShouldRetain(true));
    Serial.printf("[BLE-PRESENCE] %s -> %s online%s\n", slot->deviceId.c_str(),
                  ptopic.c_str(), ok ? "" : " PUBLISH FAILED");
  }
  return true;
}

/** Downlink. Returns false if this target is not a BLE lock we hold, so the
 *  caller can fall through to Thread — a bridge may carry both kinds.
 *
 *  XF-149 — does NOT write to the BLE characteristic itself anymore. Decodes
 *  and validates here (cheap, non-blocking), then hands the bytes to
 *  ozBleCentralTick() to actually write, one per tick, off the MQTT-callback
 *  call stack. See the g_bleCmdQ comment above for why. */
static bool forwardOverBle(const String &target, const String &fieldName,
                           const String &valueHex) {
  OzBleLock *l = ozBleFindByDeviceId(target);
  if (!l || !ozBleConnected(*l)) return false;
  // P5.5 — RELAY GOES TO `command`, NOT `control`. Writing a server envelope
  // to `control` invokes the app's M4 path, which requires a challenge this
  // bridge cannot produce; that is exactly the UNLOCK_DENIED seen on the
  // 2026-08-30 end-to-end test. No fallback to `control` on purpose: it would
  // "work" only by silently failing, which is worse than a named error.
  if (!l->chrCommand) {
    Serial.printf("[BLE-CMD] %s: no `command` characteristic (pre-P5.5 lock "
                  "firmware) — dropped, NOT retried on control\n", target.c_str());
    return true;                          // it IS ours; do not also try Thread
  }

  // 🔴 THE CONTROL CHARACTERISTIC TAKES RAW BYTES, NOT HEX. The Thread path
  // forwards the hex string verbatim inside a JSON datagram and lets the lock
  // decode it; BLE `control` is written binary (ControlCB::onWrite reads
  // getData()/getLength() precisely because a c_str() copy would truncate at
  // the first zero byte). So the bridge decodes here. It still never
  // interprets the envelope — decoding hex is not decoding the seal.
  uint8_t buf[600];
  const size_t n = ozHexToBytesVar(valueHex, buf, sizeof(buf));
  if (!n) {
    Serial.printf("[BLE-CMD] %s: bad/oversized %s (%u chars) — dropped\n",
                  target.c_str(), fieldName.c_str(), (unsigned)valueHex.length());
    return true;
  }

  if (g_bleCmdQCount >= OZ_BLE_CMDQ) {
    // Real backpressure, not a hang — the write-per-tick drain is only ever
    // this shallow behind if a link is badly contended right now. Dropping
    // (rather than growing unbounded) matches every other queue in this file;
    // the caller side (server) already retries relays it never sees confirmed.
    Serial.printf("[BLE-CMD] %s: write queue full (%u pending) — %s DROPPED\n",
                  target.c_str(), (unsigned)g_bleCmdQCount, fieldName.c_str());
    return true;
  }
  OzBleCmdWrite &slot = g_bleCmdQ[(g_bleCmdQHead + g_bleCmdQCount) % OZ_BLE_CMDQ];
  slot.deviceId = target;
  slot.fieldName = fieldName;
  memcpy(slot.buf, buf, n);
  slot.len = n;
  g_bleCmdQCount++;
  Serial.printf("[BLE-CMD] %s: %s (%u B) queued (%u pending)\n", target.c_str(),
                fieldName.c_str(), (unsigned)n, (unsigned)g_bleCmdQCount);
  return true;
}

/** Main-task servicing: reap dead links, drain both queues, rescan. */
static void ozBleCentralTick() {
  if (!g_bleCentralReady) return;
  ozBleSerialConsole(); // bench pairing tool — see its header

  // XF-144 P2 — close out a targeted scan once its window elapses. Checked
  // every tick rather than on a dedicated timer; the request is short-lived
  // (duration_s, typically single-digit seconds) so tick-granularity latency
  // here is negligible next to the scan itself.
  if (g_scanReq.active && millis() >= g_scanReq.deadlineMs) {
    BLEScan *scan = BLEDevice::getScan();
    if (scan && scan->isScanning()) scan->stop();
    JsonDocument doc;
    doc["target"] = g_scanReq.target;
    doc["found"] = g_scanReq.found;
    if (g_scanReq.found) doc["rssi"] = g_scanReq.rssi;
    String out;
    serializeJson(doc, out);
    const String topic = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/scan_result";
    const bool ok = mqttClient.publish(topic.c_str(), out.c_str());
    Serial.printf("[SCAN] %s -> %s found=%s%s%s\n", g_scanReq.target.c_str(),
                  topic.c_str(), g_scanReq.found ? "y" : "n",
                  g_scanReq.found ? " rssi=" : "",
                  g_scanReq.found ? String(g_scanReq.rssi).c_str() : "");
    if (!ok) Serial.println("[SCAN] result publish FAILED");
    g_scanReq = OzScanRequest();
  }

  // ── REPAINT THE PANEL WHEN THE LOCK COUNT CHANGES ────────────────────────
  //
  // 🔴 drawStatus() runs on EVENTS ONLY — state transitions, notifyStatus(),
  // the RX-banner lapse. Nothing repaints it periodically except the clock row.
  // So the footer's `BLE:n` and its dot would show whatever was true at the
  // last unrelated event and then sit there: a lock could connect or drop and
  // the panel would go on asserting the old number indefinitely. That is the
  // panel lying about state, which this codebase has been bitten by before and
  // is exactly what the dot exists to prevent.
  //
  // Checked at the TOP so it runs on every tick regardless of the early
  // returns further down (the connect path returns before the rescan block).
  // Only repaints on an actual CHANGE — drawStatus() does a full fillScreen(),
  // which is far too heavy to run every loop iteration.
  {
    // Keyed on BOTH numbers the footer shows. Connected-count alone would miss
    // a pairing change: `pair`/`unpair` moves TOTAL (and therefore OFF) while
    // ON stays put, so the panel would keep asserting the old totals until
    // some unrelated event repainted it.
    static uint16_t lastPainted = 0xFFFF; // 0xFFFF = nothing painted yet
    const uint16_t now = (uint16_t)(ozBleConnectedCount() << 8) | ozBlePairedTotal();
    if (now != lastPainted) {
      lastPainted = now;
      if (lcdOn) drawStatus();
      // P5 F3 — the app's pairing list is driven by the same edge. Anything
      // that changes ON/TOTAL changes the roster, so publishing here means
      // the app never has to poll.
      ozBlePublishRoster();
    }
  }

  // Reap. registerForNotify keeps raw pointers into the client's service map,
  // so a dropped link must clear them or the next notify dereferences freed
  // memory.
  for (auto &l : g_bleLocks) {
    if (!l.inUse || !l.client) continue;
    if (!l.client->isConnected()) {
      Serial.printf("[BLE] %s DISCONNECTED after %lus\n",
                    l.deviceId.length() ? l.deviceId.c_str() : l.addr.c_str(),
                    (millis() - l.connectedAt) / 1000);
      // 2026-08-31 — the offline half of the bridge-reports-presence fix
      // above. Read l.deviceId BEFORE the reset below wipes it. Retained
      // (ozPresenceShouldRetain(false)==true) so a late subscriber learns
      // this lock went away rather than reading stale "online" — same rule
      // Thread's own disconnect path already follows via OZ_PRESENCE_LWT,
      // reused here for the identical situation on a different transport.
      if (l.deviceId.length()) {
        ozForgetLockOnline(l.deviceId.c_str());
        // `via` set here too, for audit symmetry with the online publish —
        // but server's reconciliation logic should gate on the ONLINE
        // transition specifically (a bridge losing its own connection is
        // not a claim of ownership, so an offline `via` must not get read
        // as "this bridge now owns the lock").
        const String out = ozBuildLockPresence(l.deviceId, false, OZ_PRESENCE_LWT,
                                               String(""), nullptr, deviceId);
        const String ptopic = "ozkie/" + cfgSiteId + "/locks/" + l.deviceId + "/presence";
        const bool ok = mqttClient.publish(ptopic.c_str(), out.c_str(),
                                           ozPresenceShouldRetain(false));
        Serial.printf("[BLE-PRESENCE] %s -> %s offline%s\n", l.deviceId.c_str(),
                      ptopic.c_str(), ok ? "" : " PUBLISH FAILED");
      }
      delete l.client;
      l = OzBleLock();                    // full reset — pointers included
    }
  }

  // Publish whatever the notify callback queued. Main task, so PubSubClient
  // is safe to touch here and only here.
  for (auto &q : g_bleUpQ) {
    if (!q.inUse || !q.ready) continue;   // claimed-but-still-being-written: wait
    // Same rule as the producer: copy OUTSIDE any critical section, because
    // String assignment can allocate. `ready` already guarantees the producer
    // has finished with this slot, so no lock is needed to read it.
    const String deviceId = q.deviceId;
    const String payload  = q.payload;
    q.deviceId = ""; q.payload = "";      // release the heap now, not at reuse
    q.ready = false;
    q.inUse = false;                      // publish LAST — frees the slot

    if (!mqttClient.connected()) {
      // Same honesty as the Thread path: the lock has already spent this
      // message, so say it is gone rather than let it vanish.
      g_bleUpDropped++;
      Serial.printf("[BLE-UP] %s: broker down, uplink DROPPED (%u B)\n",
                    deviceId.c_str(), (unsigned)payload.length());
      continue;
    }
    // 2026-08-31, PM directive — forward EVERYTHING notified on `member`
    // verbatim to `.../uplink`, no branching here. An earlier draft of this
    // fix special-cased `reset_outcome` onto `.../presence` (matching
    // Thread's own reset path); superseded — server built and LIVE-VERIFIED
    // `handleUplinkReset()` on THIS topic instead, keyed on `type==="reset"`
    // checked before the sealed-envelope path (server.js:1155/1778,
    // XFtposDecisions-138 2026-08-31 08:49:09). Match what's actually
    // running, not what was designed before that existed.
    //
    // U3's rule, unchanged: the LOCK's own topic, so ozlockserv stays a
    // mailman and needs no change to accept a BLE lock (confirmed by server
    // in XFtposDecisions-138 S1-S3: every path is keyed on bridge_id + caps,
    // never on transport).
    const String topic = "ozkie/" + cfgSiteId + "/locks/" + deviceId + "/uplink";
    const bool ok = mqttClient.publish(topic.c_str(), payload.c_str());
    if (ok) g_bleUpRelayed++;
    Serial.printf("[BLE-UP] %s -> %s (%u B)%s\n", deviceId.c_str(), topic.c_str(),
                  (unsigned)payload.length(), ok ? "" : " PUBLISH FAILED");
  }

  // XF-149 — service ONE queued relay write per tick, same "one blocking op
  // per tick" discipline as the connect-drain below. sink->writeValue(...,
  // true) is a write-WITH-RESPONSE: under a contended link it can take far
  // longer than a healthy one, so bounding it to one per tick (and returning
  // immediately after, like the connect case) keeps a slow write from also
  // delaying the reap/uplink-drain/rescan work above and below it — and
  // critically keeps mqttClient.loop() (called later in the same main
  // loop()) from being starved, which was the actual mechanism XF-149 traced.
  if (g_bleCmdQCount) {
    OzBleCmdWrite &cmd = g_bleCmdQ[g_bleCmdQHead];
    g_bleCmdQHead = (g_bleCmdQHead + 1) % OZ_BLE_CMDQ;
    g_bleCmdQCount--;

    OzBleLock *l = ozBleFindByDeviceId(cmd.deviceId);
    if (!l || !ozBleConnected(*l) || !l->chrCommand) {
      Serial.printf("[BLE-CMD] %s: %s dropped from queue — no longer connected\n",
                    cmd.deviceId.c_str(), cmd.fieldName.c_str());
    } else {
      const bool ok = l->chrCommand->writeValue(cmd.buf, cmd.len, true /*response*/);
      g_bleCmdSent++;
      Serial.printf("[BLE-CMD] %s <- %s (%u B)%s\n", cmd.deviceId.c_str(),
                    cmd.fieldName.c_str(), (unsigned)cmd.len, ok ? "" : " WRITE FAILED");
    }
    return;
  }

  // Connect anything the scanner queued. One per tick: connect() blocks for
  // up to a few hundred ms and loop() has a watchdog and an LCD to service.
  String next;
  portENTER_CRITICAL(&g_bleMux);
  if (g_blePendingCount) {
    next = g_blePending[0];
    for (uint8_t i = 1; i < g_blePendingCount; i++) g_blePending[i - 1] = g_blePending[i];
    g_blePendingCount--;
  }
  portEXIT_CRITICAL(&g_bleMux);
  if (next.length()) { ozBleConnectOne(next); return; }

  // Rescan on a slow timer. Locks that are already connected re-advertise
  // nothing, so this only ever finds new or returning ones.
  // Adaptive: hunt hard while a paired lock is unreachable, idle down to a
  // watchdog once everything we own is connected. See the interval defines.
  // 🔴 ZERO PAIRED IS *NOT* "NOTHING TO LOOK FOR" — it is DISCOVERY MODE.
  //
  // This read `missing ? FAST : IDLE`, and missing = paired - connected. With
  // an empty allowlist that is 0, so a freshly-reset bridge — the exact state
  // in which the app needs `seen` populated to offer anything to pair —
  // dropped to the 120 s idle rate and appeared to do nothing. Caught on the
  // bench 2026-08-30 while the operator was trying to pair: bridge sat at
  // ble=0/0 with no scan results at all.
  //
  // The flaw was conflating two states that share a number: "everything I own
  // is connected" (idle is right) and "I own nothing yet" (hunt).
  // CLAIM WINDOW TAKES THE RADIO (2026-09-01, XF-143). A freshly-reset/unowned
  // bridge is simultaneously the exact state that wants aggressive lock
  // discovery (comment above, 2026-08-30) AND the state a phone is most
  // likely trying to connect over BLE to actually CLAIM the bridge — the
  // ESP32-C6 has one shared 2.4GHz radio for both central scan and peripheral
  // GATT, and an active scan measurably hurts a concurrent peripheral
  // connection attempt. Two bridges on the bench both failed to receive a
  // claim/provision write while sitting in FAST discovery-scan mode; pausing
  // scanning for the ~60s the window is open is a narrow, reversible trade —
  // discovery just resumes on the next tick once the window closes, and
  // already-paired locks are unaffected (they don't need scanning to work).
  if (claimWindowOpen()) return;

  const uint8_t missing = ozBleMissingCount();
  const bool discovering = (ozBlePairedTotal() == 0);
  const unsigned long scanEvery =
      (missing || discovering) ? OZ_BLE_RESCAN_FAST_MS : OZ_BLE_RESCAN_IDLE_MS;
  if (millis() - g_bleLastScanAt > scanEvery) {
    // Log the RATE only when it changes, not every scan — otherwise this line
    // becomes the noisiest thing in the log and stops being read.
    static uint16_t lastRate = 0xFFFF;
    const uint16_t rateKey = (uint16_t)(missing << 1) | (discovering ? 1 : 0);
    if (rateKey != lastRate) {
      lastRate = rateKey;
      Serial.printf("[BLE-SCAN] rate -> %s (%u of %u paired missing%s)\n",
                    (missing || discovering) ? "FAST" : "idle", missing,
                    ozBlePairedTotal(),
                    discovering ? ", DISCOVERY: none paired yet" : "");
    }
    g_bleLastScanAt = millis();
    BLEScan *scan = BLEDevice::getScan();
    if (scan && !scan->isScanning()) {
      scan->setAdvertisedDeviceCallbacks(&g_bleScanCb, false /*no duplicates*/);
      scan->setActiveScan(true);
      scan->start(OZ_BLE_SCAN_SECS, nullptr, false); // async — cb does the work
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// BENCH PAIRING CONSOLE — a TOOL, not the product path
//
// F1-F3 make the bridge hold ONLY paired locks. That is correct, and it also
// means a bridge with an empty allowlist holds nothing at all — which is
// exactly what a freshly-flashed bridge has. The product answer is F4 (the
// app pairs locks), but F4's transport is still undecided (XF-138 A6), and
// without SOME way to pair, flashing F1-F3 would take a working bench to zero
// connected locks with no way back.
//
// So: a serial console. Deliberately bench-only and deliberately dumb — it
// prejudges nothing about A6, adds no wire protocol, and is reachable only by
// someone already holding a USB cable (which, on this product, is a stronger
// physical-presence proof than any gesture we gate on elsewhere).
//
//   paired            list the allowlist
//   seen              list locks seen but NOT paired (this is F3's output)
//   pair <device_id>  add to the allowlist
//   pair all          add everything currently in `seen` — bench convenience
//   unpair <device_id>
//
// When F4 lands this stays useful for exactly the case it was written for:
// recovering a bridge whose app is unavailable.
// ═══════════════════════════════════════════════════════════════════════════
// 🔴 2026-09-01 — the actual command logic, extracted so bridgeBLE.ino's
// ozBridgeConsolePoll() can call it too. That function reads raw Serial
// input every loop tick and, being called first, ALWAYS drains the whole
// buffer before ozBleSerialConsole() below ever runs — meaning these five
// commands have been silently unreachable since ozBridgeConsolePoll() was
// added earlier tonight. Caught live trying to run `pair all` for a bench
// test. ozBleSerialConsole() itself is kept below as a second (now
// harmless, never-first) reader in case call order ever changes, but the
// real entry point for these commands today is ozBridgeConsolePoll().
static void ozBlePairConsoleCmd(const String &line) {
  if (line == "paired") {
    Serial.printf("[PAIR] %u paired lock(s):\n", g_pairedCount);
    for (uint8_t i = 0; i < g_pairedCount; i++)
      Serial.printf("[PAIR]   %s%s\n", g_pairedIds[i].c_str(),
                    ozBleFindByDeviceId(g_pairedIds[i]) ? "  (connected)" : "");
    return;
  }
  if (line == "seen") {
    bool any = false;
    for (auto &s : g_seenUnpaired) {
      if (!s.deviceId.length()) continue;
      any = true;
      Serial.printf("[SEEN] %s  addr=%s  %lus ago\n", s.deviceId.c_str(),
                    s.addr.c_str(), (millis() - s.at) / 1000);
    }
    if (!any) Serial.println("[SEEN] (none — no unpaired lock has been "
                             "identified since boot)");
    return;
  }
  if (line == "pair all") {
    uint8_t n = 0;
    for (auto &s : g_seenUnpaired)
      if (s.deviceId.length() && ozBlePairAdd(s.deviceId)) n++;
    Serial.printf("[PAIR] pair all -> %u added\n", n);
    return;
  }
  if (line.startsWith("pair ")) {
    const String id = line.substring(5);
    Serial.printf("[PAIR] pair %s -> %s\n", id.c_str(),
                  ozBlePairAdd(id) ? "OK" : "REFUSED");
    return;
  }
  if (line.startsWith("unpair ")) {
    const String id = line.substring(7);
    Serial.printf("[PAIR] unpair %s -> %s\n", id.c_str(),
                  ozBlePairRemove(id) ? "OK" : "not in list");
    return;
  }
  Serial.println("[PAIR] commands: paired | seen | pair <id> | pair all | unpair <id>");
}

static void ozBleSerialConsole() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;
  ozBlePairConsoleCmd(line);
}

// ═══════════════════════════════════════════════════════════════════════════
// P5 F3/F4 — THE ROSTER, and the app-facing pairing contract
//
// `locks` (CHR_LOCKS, read+notify) carries this JSON:
//
//   {"max":3,
//    "paired":[{"id":"ozk-acebe639f8c4","up":true},
//              {"id":"ozk-b0a6048b5fd8","up":false}],
//    "seen":  [{"id":"ozk-...","rssi":-70,"age_s":12}]}
//
//   max     OZ_BLE_MAX_LOCKS — how many this bridge can hold AT ALL. Sent so
//           the app can grey out "pair" at capacity instead of discovering
//           the limit by having a write refused. Currently 3 (see the
//           constant's own note; it is a build-time ceiling, not silicon).
//   paired  the allowlist. `up` = link live right now. A paired lock with
//           up:false is an OUTAGE — a door this bridge cannot reach.
//   seen    identified but NOT paired — this is the list to offer the user.
//
// Chunked with the SAME convention as the lock's `member` characteristic
// (ozdoorlock_core.h ozNotifyChunked): no length prefix, no end marker, a
// chunk starting '{' restarts the buffer, and "it parses" IS the end marker.
// Reused rather than invented because the app already implements exactly this
// reassembly for member enrolment — one convention, not two.
// ═══════════════════════════════════════════════════════════════════════════
static String ozBleRosterJson() {
  JsonDocument doc;
  doc["max"] = OZ_BLE_MAX_LOCKS;
  JsonArray pa = doc["paired"].to<JsonArray>();
  for (uint8_t i = 0; i < g_pairedCount; i++) {
    JsonObject o = pa.add<JsonObject>();
    o["id"] = g_pairedIds[i];
    o["up"] = ozBleFindByDeviceId(g_pairedIds[i]) != nullptr;
  }
  JsonArray sn = doc["seen"].to<JsonArray>();
  for (auto &s : g_seenUnpaired) {
    if (!s.deviceId.length()) continue;
    JsonObject o = sn.add<JsonObject>();
    o["id"] = s.deviceId;
    o["rssi"] = s.rssi;
    o["age_s"] = (uint32_t)((millis() - s.at) / 1000);
  }
  String out; serializeJson(doc, out);
  return out;
}

/** Push the roster to whoever is subscribed. Called on every change that the
 *  app's pairing UI would want to see — pair, unpair, connect, disconnect. */
static void ozBlePublishRoster() {
  if (!chrLocks) return;
  const String js = ozBleRosterJson();
  chrLocks->setValue(js.c_str()); // READ always returns the current roster
  if (!bleClientConnected) return; // nobody to notify; the READ above suffices
  static const size_t CHUNK = 180;  // same 180 B the lock's member char uses
  for (size_t off = 0; off < js.length(); off += CHUNK) {
    const size_t n = min(CHUNK, (size_t)js.length() - off);
    chrLocks->setValue((uint8_t *)js.c_str() + off, n);
    chrLocks->notify();
    delay(20);
  }
  chrLocks->setValue(js.c_str()); // leave the full value readable afterwards
}

/** Called once BLE is up (startBle() has run BLEDevice::init()). */
static void ozBleCentralBegin() {
  ozBleLoadPaired();   // P5 F1 — the allowlist decides what we may hold
  g_bleCentralReady = true;
  g_bleLastScanAt = 0;                    // scan on the very next tick
  Serial.printf("[BLE] central role armed — scanning for %s (max %d locks)\n",
                OZL_SVC_UUID, OZ_BLE_MAX_LOCKS);
}
