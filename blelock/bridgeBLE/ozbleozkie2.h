// ozbleozkie2.h — the OZKIE v2 relay (ozkey-55 F1, ozkey-48 rev 6 §2.3–§2.4).
//
// Included by ozblecentral.h just before ozBleConnectOne(). v1 is untouched:
// a lock advertising the v1 service (…4b31) takes the old path; a lock
// advertising the v2 service (…4b32) takes this one.
//
// The bridge is a BLIND relay. It never opens an OZKIE message and holds no
// key. Its whole job:
//   lock `relay` notify  → reassemble (oz_frame) → MQTT  type‖message
//                          on ozkie/<site>/locks/<lock>/ozkie/<this bridge>
//   MQTT  ozkie/<site>/bridges/<this bridge>/ozkie/<lock>  type‖message
//                        → check → fragment at the ATT MTU → write `relay`
//
// Closed type set (§2.4.1): 0 EDHOC, 1 OSCORE, 2 error. Anything else, an
// empty payload or one over 1 + 1024 B is dropped and COUNTED, both ways.
// A downlink for a lock this bridge doesn't hold is dropped and counted; it
// is never queued for later (§2.4.2, §2.4.4: no v2 frame is ever held).
//
// The bridge's own `bridge` bond (a session on `ozkie`) is in ozbridgebond.h
// (F1-BRIDGE-BOND-1, rev 7). Not here yet: the time set over it; the counters
// on MQTT (F2).
#pragma once

extern "C" {
#include "oz_frame.h"
#include "oz_cbor.h"
}

#define OZ2_SVC_UUID   "4f5a4b32-0001-4c4f-434b-000000000001"
#define OZ2_CHR_INFO   "4f5a4b32-0002-4c4f-434b-000000000001"
#define OZ2_CHR_RELAY  "4f5a4b32-0004-4c4f-434b-000000000001"

#define OZ2_TYPE_MAX   2                        /* EDHOC, OSCORE, error */
#define OZ2_MSG_MAX    OZ_FRAME_MAX_MSG          /* 1024 */
#define OZ2_UPQ        4
#define OZ2_DNQ        4

struct Oz2Counters {
  uint32_t up = 0, dn = 0;                      /* relayed whole messages */
  uint32_t badType = 0, badLen = 0;             /* refused by the closed set */
  uint32_t notHeld = 0;                         /* downlink for a lock we don't hold */
  uint32_t upDropped = 0, dnDropped = 0;        /* a queue was full */
  uint32_t rxBad = 0;                           /* malformed GATT frames */
  uint32_t writeFail = 0;
};
static Oz2Counters g_oz2;

// One reassembler per lock slot (indexed like g_bleLocks). 1 KB each, so they
// live here rather than in OzBleLock, which is reset by value.
static oz_reasm g_oz2Ra[OZ_BLE_MAX_LOCKS];

#include "ozbridgebond.h"                        // F1-BRIDGE-BOND-1: the bridge's own bond

struct Oz2Msg {
  volatile bool inUse = false, ready = false;
  char     dev[65] = {0};
  uint16_t len = 0;                             /* type byte + message */
  uint8_t  buf[1 + OZ2_MSG_MAX];
};
static Oz2Msg g_oz2Up[OZ2_UPQ];                 /* BLE task → main task */
static Oz2Msg g_oz2Dn[OZ2_DNQ];                 /* MQTT callback → tick (both main task) */

static int oz2SlotIndex(const OzBleLock *l) { return (int)(l - g_bleLocks); }

// ── uplink: lock → MQTT ─────────────────────────────────────────────────────

// BLE host task. Same claim-then-copy discipline as ozBleQueueUplink().
static void oz2NotifyCb(BLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool) {
  for (auto &l : g_bleLocks) {
    if (!l.inUse || l.ozkie != 2 || chr != l.chrRelay) continue;
    oz_reasm *ra = &g_oz2Ra[oz2SlotIndex(&l)];
    int done = 0;
    if (oz_reasm_feed(ra, data, len, &done) != OZ_OK) { g_oz2.rxBad++; return; }
    if (!done) return;
    int idx = -1;
    portENTER_CRITICAL(&g_bleMux);
    for (int i = 0; i < OZ2_UPQ; i++)
      if (!g_oz2Up[i].inUse) { g_oz2Up[i].inUse = true; idx = i; break; }
    portEXIT_CRITICAL(&g_bleMux);
    if (idx < 0) { g_oz2.upDropped++; return; }
    Oz2Msg &m = g_oz2Up[idx];
    strlcpy(m.dev, l.deviceId.c_str(), sizeof m.dev);
    m.buf[0] = ra->type;
    memcpy(m.buf + 1, ra->buf, ra->len);
    m.len = (uint16_t)(1 + ra->len);
    m.ready = true;
    return;
  }
}

// ── downlink: MQTT → lock ───────────────────────────────────────────────────

// Called first thing from mqttMessageReceived(). Returns true when the topic
// is a v2 downlink (handled or refused here), false for everything else.
static bool oz2OnMqtt(const char *topic, const byte *payload, unsigned int len) {
  static String prefix;
  prefix = "ozkie/" + cfgSiteId + "/bridges/" + deviceId + "/ozkie/";
  if (strncmp(topic, prefix.c_str(), prefix.length()) != 0) return false;
  const char *lock = topic + prefix.length();
  if (!*lock || strchr(lock, '/')) { g_oz2.notHeld++; return true; }
  if (len < 2 || len > 1 + OZ2_MSG_MAX) {
    g_oz2.badLen++;
    Serial.printf("[OZ2-DN] %s: %u B refused (closed set: 2..%u)\n", lock, len, 1 + OZ2_MSG_MAX);
    return true;
  }
  if (payload[0] > OZ2_TYPE_MAX) {
    g_oz2.badType++;
    Serial.printf("[OZ2-DN] %s: type %u refused (closed set 0..2)\n", lock, payload[0]);
    return true;
  }
  OzBleLock *l = ozBleFindByDeviceId(String(lock));
  if (!l || l->ozkie != 2 || !ozBleConnected(*l) || !l->chrRelay) {
    g_oz2.notHeld++;                            // never queued for later (§2.4.4)
    Serial.printf("[OZ2-DN] %s: not held by this bridge, dropped\n", lock);
    return true;
  }
  int idx = -1;
  for (int i = 0; i < OZ2_DNQ; i++)
    if (!g_oz2Dn[i].inUse) { idx = i; break; }
  if (idx < 0) { g_oz2.dnDropped++; Serial.printf("[OZ2-DN] %s: queue full, dropped\n", lock); return true; }
  Oz2Msg &m = g_oz2Dn[idx];
  m.inUse = true;
  strlcpy(m.dev, lock, sizeof m.dev);
  memcpy(m.buf, payload, len);
  m.len = (uint16_t)len;
  m.ready = true;
  return true;
}

// ── main-task servicing ─────────────────────────────────────────────────────

static void oz2PublishUplinks() {
  for (auto &m : g_oz2Up) {
    if (!m.ready) continue;
    const String t = "ozkie/" + cfgSiteId + "/locks/" + String(m.dev) + "/ozkie/" + deviceId;
    const bool ok = mqttClient.publish(t.c_str(), m.buf, m.len, false);   // QoS 0, never retained
    if (ok) g_oz2.up++; else g_oz2.upDropped++;
    Serial.printf("[OZ2-UP] %s type %u, %u B%s\n", m.dev, m.buf[0], (unsigned)(m.len - 1),
                  ok ? "" : " PUBLISH FAILED");
    m.ready = false;
    m.inUse = false;
  }
}

// One downlink per tick (XF-149 discipline: writes-with-response are slow
// under contention, and mqttClient.loop() must not starve). Returns true when
// it wrote, so the caller ends this tick.
static bool oz2WriteOneDownlink() {
  for (auto &m : g_oz2Dn) {
    if (!m.ready) continue;
    m.ready = false;
    OzBleLock *l = ozBleFindByDeviceId(String(m.dev));
    bool ok = false;
    if (l && l->ozkie == 2 && ozBleConnected(*l) && l->chrRelay) {
      const size_t mtu = l->mtu < OZ_FRAME_MIN_MTU ? OZ_FRAME_MIN_MTU : l->mtu;
      unsigned count = 0;
      ok = oz_frag_count(m.len - 1, mtu, &count) == OZ_OK;
      for (unsigned i = 0; ok && i < count; i++) {
        uint8_t f[OZ_FRAME_MAX_MSG + 1];
        size_t n = 0;
        ok = oz_frag(m.buf[0], m.buf + 1, m.len - 1, mtu, i, f, sizeof f, &n) == OZ_OK &&
             l->chrRelay->writeValue(f, n, true);
      }
      Serial.printf("[OZ2-DN] %s <- type %u, %u B in %u frame(s)%s\n", m.dev, m.buf[0],
                    (unsigned)(m.len - 1), count, ok ? "" : " WRITE FAILED");
    } else {
      g_oz2.notHeld++;                          // dropped while queued
      // F1-BRIDGE-LOG-1: this drop was silent. The lock went away (or its relay
      // characteristic did) between queueing and writing; say so, like the
      // enqueue-time "not held by this bridge, dropped" line does.
      Serial.printf("[OZ2-DN] %s: type %u, %u B dropped while queued (lock %s)\n", m.dev,
                    m.buf[0], (unsigned)(m.len - 1),
                    !l ? "not held" : l->ozkie != 2 ? "not v2" :
                    !ozBleConnected(*l) ? "disconnected" : "has no relay characteristic");
    }
    if (ok) g_oz2.dn++; else g_oz2.writeFail++;
    m.inUse = false;
    return true;
  }
  return false;
}

// ── attach: a v2 lock just connected ────────────────────────────────────────

// `info` (ozkey-48 §2): {1: device_id, 2: fw, 3: pub, 4: state, 5: profile}.
// Returns the device id ("" if unreadable); pub/state when present
// (state −1 = absent; 0 = unclaimed).
static String oz2ReadInfo(BLERemoteCharacteristic *info, uint8_t pub[32], bool *havePub, int *state) {
  *havePub = false;
  *state = -1;
  if (!info || !info->canRead()) return String();
  const String raw = info->readValue();
  oz_cbor_r r;
  oz_cbor_r_init(&r, (const uint8_t *)raw.c_str(), raw.length());
  const size_t pairs = oz_cbor_get_map(&r);
  String id;
  for (size_t i = 0; i < pairs && !r.err; i++) {
    const uint64_t k = oz_cbor_get_uint(&r);
    size_t n = 0;
    if (k == 1 && oz_cbor_peek(&r) == OZ_CBOR_TSTR) {
      const uint8_t *s = oz_cbor_get_tstr(&r, &n);
      if (s && n >= 1 && n <= 64) id = String((const char *)s, n);
    } else if (k == 3 && oz_cbor_peek(&r) == OZ_CBOR_BSTR) {
      const uint8_t *b = oz_cbor_get_bstr(&r, &n);
      if (b && n == 32) { memcpy(pub, b, 32); *havePub = true; }
    } else if (k == 4 && oz_cbor_peek(&r) == OZ_CBOR_UINT) {
      *state = (int)oz_cbor_get_uint(&r);
    } else {
      (void)oz_cbor_skip(&r, &n);
    }
  }
  return r.err ? String() : id;
}

// Called by ozBleConnectOne() when the lock has the v2 service and not v1.
// Takes over `cl` on success; on failure it disconnects and frees it.
static bool oz2Attach(OzBleLock *slot, BLEClient *cl, BLERemoteService *svc, const String &addr) {
  uint8_t lockPub[32];
  bool havePub = false;
  int state = -1;
  const String id = oz2ReadInfo(svc->getCharacteristic(BLEUUID(OZ2_CHR_INFO)), lockPub, &havePub, &state);
  BLERemoteCharacteristic *relay = svc->getCharacteristic(BLEUUID(OZ2_CHR_RELAY));
  if (!id.length() || !relay || !relay->canNotify() || !relay->canWrite()) {
    Serial.printf("[OZ2] %s: v2 lock without a readable id or a `relay` characteristic, dropped\n",
                  addr.c_str());
    ozBleNoteProbeFailed(addr);
    cl->disconnect();
    delete cl;
    return false;
  }
  if (!ozBleIsPaired(id)) {                     // same allowlist as v1 (P5 F2)
    Serial.printf("[OZ2] %s (%s) is NOT PAIRED to this bridge, released\n", id.c_str(), addr.c_str());
    ozBleNoteUnpaired(id, addr, 0);
    cl->disconnect();
    delete cl;
    return false;
  }
  // F1-BRIDGE-BOND-1: never hold an UNCLAIMED v2 lock. Relay is useless there
  // (an unclaimed lock answers no relay EDHOC), holding it blocks first
  // pairing at the door (our idle link takes the app slot), and running the
  // bridge bond on it would START first pairing for the bridge's key.
  if (state == 0) {
    Serial.printf("[OZ2] %s is UNCLAIMED: not holding it (no relay possible, frees the door); "
                  "recheck in %lu s\n", id.c_str(), OZ2_UNCLAIMED_HOLDOFF_MS / 1000);
    oz2HoldOff(addr);
    cl->disconnect();
    delete cl;
    return false;
  }
  slot->inUse = true;
  slot->addr = addr;
  slot->client = cl;
  slot->deviceId = id;
  slot->connectedAt = millis();
  slot->ozkie = 2;
  slot->chrRelay = relay;
  slot->mtu = cl->getMTU();
  oz_reasm_reset(&g_oz2Ra[oz2SlotIndex(slot)]);
  relay->registerForNotify(oz2NotifyCb);
  Serial.printf("[OZ2] %s CONNECTED (v2, relay, ATT MTU %u)\n", id.c_str(), (unsigned)slot->mtu);
  if (havePub && state > 0)
    bbAttach(slot, svc->getCharacteristic(BLEUUID(OZ2_CHR_OZKIE)), lockPub);
  else
    Serial.printf("[BOND] %s: `info` has no key/state, no bridge bond on this link\n", id.c_str());

  if (ozClaimLockOnline(id.c_str())) {
    const String out = ozBuildLockPresence(id, true, OZ_PRESENCE_ONLINE, String(""), nullptr,
                                           deviceId, 2);
    const String ptopic = "ozkie/" + cfgSiteId + "/locks/" + id + "/presence";
    const bool ok = mqttClient.publish(ptopic.c_str(), out.c_str(), ozPresenceShouldRetain(true));
    Serial.printf("[BLE-PRESENCE] %s -> %s online (v2)%s\n", id.c_str(), ptopic.c_str(),
                  ok ? "" : " PUBLISH FAILED");
  }
  return true;
}

// Serial `oz2` on the bench console: the counters, until F2 puts them on MQTT.
static void oz2PrintCounters() {
  Serial.printf("[OZ2] up=%lu dn=%lu bad_type=%lu bad_len=%lu not_held=%lu up_drop=%lu "
                "dn_drop=%lu rx_bad=%lu write_fail=%lu\n",
                (unsigned long)g_oz2.up, (unsigned long)g_oz2.dn, (unsigned long)g_oz2.badType,
                (unsigned long)g_oz2.badLen, (unsigned long)g_oz2.notHeld,
                (unsigned long)g_oz2.upDropped, (unsigned long)g_oz2.dnDropped,
                (unsigned long)g_oz2.rxBad, (unsigned long)g_oz2.writeFail);
}
