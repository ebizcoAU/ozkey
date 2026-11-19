// ozblerules.h — BRIDGE-HOST-SUITE-1 step 2: bridgeBLE's pure rules, out of
// the Arduino code so the host suite can test them (test/test_blerules.cpp).
// Behaviour is unchanged: these are the checks oz2OnMqtt() and
// ozBleLoadPaired() made inline.
#ifndef OZBLERULES_H
#define OZBLERULES_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ── OZKIE v2 downlink gate (ozkey-48 rev 6 §2.4): topic = <prefix><lock>,
// payload = type ‖ message, type 0..type_max, 1..msg_max message bytes.
enum { OZ2_DN_OK = 0, OZ2_DN_NOT_OURS, OZ2_DN_BAD_TOPIC, OZ2_DN_BAD_LEN, OZ2_DN_BAD_TYPE };

static inline int oz2DownlinkCheck(const char *topic, const char *prefix, const uint8_t *payload, size_t len,
                                   size_t msg_max, uint8_t type_max, const char **lock_out) {
  const size_t plen = strlen(prefix);
  *lock_out = "";
  if (strncmp(topic, prefix, plen) != 0) return OZ2_DN_NOT_OURS;
  const char *lock = topic + plen;
  *lock_out = lock;
  if (!*lock || strchr(lock, '/')) return OZ2_DN_BAD_TOPIC;
  if (len < 2 || len > 1 + msg_max) return OZ2_DN_BAD_LEN;
  if (payload[0] > type_max) return OZ2_DN_BAD_TYPE;
  return OZ2_DN_OK;
}

// ── The paired-lock allowlist blob in NVS: ids joined by '\n'. Empty lines are
// skipped; at most max ids are taken. emit() gets each id (not NUL-terminated).
// Returns the number emitted.
static inline unsigned ozPairBlobSplit(const char *blob, unsigned max,
                                       void (*emit)(const char *id, size_t n, void *ctx), void *ctx) {
  unsigned count = 0;
  const char *p = blob;
  while (*p && count < max) {
    const char *nl = strchr(p, '\n');
    const size_t n = nl ? (size_t)(nl - p) : strlen(p);
    if (n) { emit(p, n, ctx); count++; }
    if (!nl) break;
    p = nl + 1;
  }
  return count;
}

// ── BRIDGE-THREADFAIL-1: what a failed Thread attach means for the bridge.
// With a Wi-Fi uplink the bridge still goes OPERATIONAL (MQTT + BLE relay; BLE
// locks need no Thread) and picks the attach up later; without one it fails
// closed as before (the Wi-Fi-failed fallback re-arms and retries).
enum { OZ_TH_ATTACHED = 0, OZ_TH_RUN_WITHOUT, OZ_TH_FAIL_CLOSED };

static inline int ozThreadAttachOutcome(int attached, int wifi_uplink) {
  if (attached) return OZ_TH_ATTACHED;
  return wifi_uplink ? OZ_TH_RUN_WITHOUT : OZ_TH_FAIL_CLOSED;
}

// The late attach: run the attached tail once, when the bridge runs without
// Thread and the stack now reports an attached role.
static inline int ozThreadLateAttachDue(int operational, int watching, int attached_now) {
  return operational && watching && attached_now;
}
#endif
