// ozbridgebond.h — the bridge's own `bridge` bond (F1-BRIDGE-BOND-1,
// ozkey-48 rev 7 §2.3 / §6). Included by ozbleozkie2.h.
//
// Why: the lock lets ONLY a connection whose `ozkie` session completed EDHOC
// as an enrolled role=bridge bond own `relay` and skip the app slot. Without
// it, an idle bridge after a lock reboot makes the lock BUSY and the owner's
// app at the door can't connect (F1 field bug, 2026-11-06).
//
// What it does:
//  - one X25519 key per bridge, made once on the bridge, kept in NVS
//    (namespace "ozkie", key "bk"), never sent anywhere; the PUBLIC key goes
//    out in the bridge's presence and its BLE info as "ozkie_pub" (hex). The
//    BLE value is authoritative; the app checks the server copy against it.
//  - on every v2 attach, EDHOC as initiator on the lock's `ozkie`, presenting
//    the key by value (kccs). The lock maps it to the bond (bond_by_pub).
//  - success = the lock accepts message_3 silently. Refusal (not enrolled) =
//    an EDHOC error after message_3: logged, and the lock stays on rev 6
//    behaviour for this bridge. Retried every OZ_BB_RETRY_MS while attached.
//  - one handshake at a time (one oz_edhoc, ~2 KB, shared).
//  - relayed traffic is still never opened: this key is the bridge's own bond,
//    and that bond may only set the time (§6).
//  - NEVER run against an UNCLAIMED lock: there EDHOC on `ozkie` starts first
//    pairing for the bridge's key. oz2Attach() releases an unclaimed lock
//    before we get here (and holds off OZ2_UNCLAIMED_HOLDOFF_MS).
#pragma once

extern "C" {
#include "oz_port.h"
#include "oz_edhoc.h"
#include "oz_lock.h"          /* oz_ccs_for, OZ_CCS_LEN */
#include "oz_port_esp32c6.h"
}

#define OZ2_CHR_OZKIE   "4f5a4b32-0003-4c4f-434b-000000000001"
#define OZ_BB_M2_MS     5000UL     /* message_2 must come back within this */
#define OZ_BB_M3_MS     1500UL     /* silence this long after message_3 = accepted */
#define OZ_BB_RETRY_MS  60000UL    /* refused / failed: try again while attached */

enum { BB_NONE = 0, BB_M1, BB_M3, BB_OK, BB_REFUSED, BB_FAIL };

static uint8_t g_bbPriv[32], g_bbPub[32], g_bbCcs[OZ_CCS_LEN], g_bbKccs[64];
static size_t  g_bbKccsLen = 0;
static bool    g_bbReady = false;
static oz_edhoc g_bbE;
static int     g_bbOwner = -1;                 /* slot index mid-handshake */
static uint8_t g_bbLockCcs[OZ_CCS_LEN];
static uint8_t g_bbLockPub[32];
static oz_reasm g_bbRa[OZ_BLE_MAX_LOCKS];       /* `ozkie` notify reassembly */
static volatile bool g_bbIn[OZ_BLE_MAX_LOCKS];  /* a whole message is waiting */
struct BbCounters { uint32_t ok = 0, refused = 0, fail = 0; };
static BbCounters g_bb;

static void bbInit() {
  if (g_bbReady) return;
  if (oz_esp32c6_init() != OZ_OK) {
    Serial.println("[BOND] NVS unavailable: no bridge key, bridge bond DISABLED (rev 6 behaviour)");
    return;
  }
  size_t n = 0;
  if (oz_port_kv_get("bk", g_bbPriv, sizeof g_bbPriv, &n) != OZ_OK || n != 32) {
    if (oz_port_random(g_bbPriv, 32) != OZ_OK || oz_port_kv_set("bk", g_bbPriv, 32) != OZ_OK) {
      Serial.println("[BOND] could not make/store a bridge key: bridge bond DISABLED");
      return;
    }
    Serial.println("[BOND] new bridge key generated (first boot of this firmware line)");
  }
  oz_port_x25519_public_key(g_bbPub, g_bbPriv);
  oz_ccs_for(g_bbPub, g_bbCcs);
  oz_cbor_w w;
  oz_cbor_w_init(&w, g_bbKccs, sizeof g_bbKccs);
  oz_cbor_put_map(&w, 1);
  oz_cbor_put_uint(&w, 14);
  oz_cbor_put_raw(&w, g_bbCcs, OZ_CCS_LEN);
  g_bbKccsLen = w.len;
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", g_bbPub[i]);
  g_bbPubHex = String(hex);
  g_bbReady = true;
  Serial.printf("[BOND] bridge key ready, ozkie_pub=%s\n", hex);
}

// The lock's credential: the canonical CCS of the key it published in `info`
// (key 3), as the app does (ozkey-48 §3). Only the lock's own id {4: 0}.
static int bbLockCred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out) {
  (void)app;
  static const uint8_t lock_idc[4] = { 0xa1, 0x04, 0x41, 0x00 };
  if (len != 4 || memcmp(id, lock_idc, 4)) return OZ_E_NOT_FOUND;
  oz_ccs_for(g_bbLockPub, g_bbLockCcs);
  out->cred = g_bbLockCcs;
  out->cred_len = OZ_CCS_LEN;
  memcpy(out->pub, g_bbLockPub, 32);
  return OZ_OK;
}

// BLE host task: reassemble `ozkie` notifications; the main task consumes.
static void bbNotifyCb(BLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool) {
  for (auto &l : g_bleLocks) {
    if (!l.inUse || l.ozkie != 2 || chr != l.chrOzkie) continue;
    const int i = (int)(&l - g_bleLocks);
    if (g_bbIn[i]) return;                      /* lockstep: one message at a time */
    int done = 0;
    if (oz_reasm_feed(&g_bbRa[i], data, len, &done) != OZ_OK) { oz_reasm_reset(&g_bbRa[i]); return; }
    if (done) g_bbIn[i] = true;
    return;
  }
}

// Fragment at the ATT MTU and write with response. Main task only.
static bool bbWrite(OzBleLock &l, uint8_t type, const uint8_t *msg, size_t len) {
  const size_t mtu = l.mtu < OZ_FRAME_MIN_MTU ? OZ_FRAME_MIN_MTU : l.mtu;
  unsigned count = 0;
  bool ok = oz_frag_count(len, mtu, &count) == OZ_OK;
  for (unsigned i = 0; ok && i < count; i++) {
    uint8_t f[OZ_FRAME_MAX_MSG + 1];
    size_t n = 0;
    ok = oz_frag(type, msg, len, mtu, i, f, sizeof f, &n) == OZ_OK && l.chrOzkie->writeValue(f, n, true);
  }
  return ok;
}

static void bbEnd(OzBleLock &l, uint8_t state) {
  l.bb = state;
  l.bbAt = millis();
  oz_edhoc_wipe(&g_bbE);
  g_bbOwner = -1;
  const int i = (int)(&l - g_bleLocks);
  oz_reasm_reset(&g_bbRa[i]);
  g_bbIn[i] = false;
}

// Called by oz2Attach() once `relay` is up. lockPub = info key 3.
static void bbAttach(OzBleLock *l, BLERemoteCharacteristic *ozkie, const uint8_t lockPub[32]) {
  const int i = (int)(l - g_bleLocks);
  l->chrOzkie = nullptr;
  l->bb = BB_NONE;
  if (!g_bbReady || !ozkie || !ozkie->canNotify() || !ozkie->canWrite()) {
    Serial.printf("[BOND] %s: %s, no bridge bond on this link (rev 6 behaviour)\n", l->deviceId.c_str(),
                  !g_bbReady ? "no bridge key" : "lock has no usable `ozkie` characteristic");
    return;
  }
  memcpy(l->lockPub, lockPub, 32);
  oz_reasm_reset(&g_bbRa[i]);
  g_bbIn[i] = false;
  l->chrOzkie = ozkie;
  ozkie->registerForNotify(bbNotifyCb);
}

// Main loop. Runs at most one handshake step per call; returns true when it
// wrote (the caller ends the tick, XF-149 one-write discipline).
static bool bbTick() {
  // A handshake whose lock went away is abandoned.
  if (g_bbOwner >= 0) {
    OzBleLock &o = g_bleLocks[g_bbOwner];
    if (!o.inUse || o.ozkie != 2 || !o.chrOzkie || !ozBleConnected(o)) {
      oz_edhoc_wipe(&g_bbE);
      g_bbOwner = -1;
    }
  }
  for (auto &l : g_bleLocks) {
    if (!l.inUse || l.ozkie != 2 || !l.chrOzkie || !ozBleConnected(l)) continue;
    const int i = (int)(&l - g_bleLocks);
    uint8_t out[512];
    size_t ol = 0;
    switch (l.bb) {
    case BB_REFUSED:
    case BB_FAIL:
      if (millis() - l.bbAt < OZ_BB_RETRY_MS) break;
      l.bb = BB_NONE;
      /* fall through */
    case BB_NONE: {
      if (g_bbOwner >= 0) break;
      oz_edhoc_init(&g_bbE, 1);
      g_bbE.c_own.len = 1;
      g_bbE.c_own.b[0] = 0x05;
      g_bbE.own.cred = g_bbCcs;
      g_bbE.own.cred_len = OZ_CCS_LEN;
      g_bbE.own.id_cred = g_bbKccs;
      g_bbE.own.id_cred_len = g_bbKccsLen;
      memcpy(g_bbE.own_priv, g_bbPriv, 32);
      memcpy(g_bbLockPub, l.lockPub, 32);
      g_bbE.get_cred = bbLockCred;
      if (oz_edhoc_i_msg1(&g_bbE, NULL, 0, out, sizeof out, &ol) != OZ_OK || !bbWrite(l, OZ_FRAME_EDHOC, out, ol)) {
        Serial.printf("[BOND] %s: message_1 not sent\n", l.deviceId.c_str());
        g_bb.fail++;
        bbEnd(l, BB_FAIL);
        return true;
      }
      g_bbOwner = i;
      l.bb = BB_M1;
      l.bbAt = millis();
      return true;
    }
    case BB_M1:
      if (g_bbIn[i]) {
        oz_reasm *ra = &g_bbRa[i];
        uint8_t lo[256];
        size_t lol = 0;
        const bool m2ok = ra->type == OZ_FRAME_EDHOC &&
                          oz_edhoc_i_msg2(&g_bbE, ra->buf, ra->len, lo, sizeof lo, &lol) == OZ_OK;
        oz_reasm_reset(ra);
        g_bbIn[i] = false;
        if (!m2ok) {
          Serial.printf("[BOND] %s: message_2 invalid or an EDHOC error, bridge bond FAILED\n", l.deviceId.c_str());
          g_bb.fail++;
          bbEnd(l, BB_FAIL);
          return false;
        }
        if (oz_edhoc_i_msg3(&g_bbE, NULL, 0, out, sizeof out, &ol) != OZ_OK || !bbWrite(l, OZ_FRAME_EDHOC, out, ol)) {
          Serial.printf("[BOND] %s: message_3 not sent\n", l.deviceId.c_str());
          g_bb.fail++;
          bbEnd(l, BB_FAIL);
          return true;
        }
        l.bb = BB_M3;
        l.bbAt = millis();
        return true;
      }
      if (millis() - l.bbAt > OZ_BB_M2_MS) {
        Serial.printf("[BOND] %s: no message_2 in %lu ms, bridge bond FAILED\n", l.deviceId.c_str(), OZ_BB_M2_MS);
        g_bb.fail++;
        bbEnd(l, BB_FAIL);
      }
      break;
    case BB_M3:
      if (g_bbIn[i]) {                           /* the lock answered message_3: a refusal */
        oz_reasm *ra = &g_bbRa[i];
        String why = "?";
        oz_cbor_r r;
        oz_cbor_r_init(&r, ra->buf, ra->len);
        (void)oz_cbor_get_int(&r);
        size_t tl = 0;
        const uint8_t *t = oz_cbor_get_tstr(&r, &tl);
        if (t && tl < 40) why = String((const char *)t, tl);
        Serial.printf("[BOND] %s: REFUSED (%s): this bridge is not enrolled as the lock's bridge "
                      "(rev 6 behaviour; retry in %lu s)\n",
                      l.deviceId.c_str(), why.c_str(), OZ_BB_RETRY_MS / 1000);
        g_bb.refused++;
        bbEnd(l, BB_REFUSED);
        return false;
      }
      if (millis() - l.bbAt > OZ_BB_M3_MS) {
        Serial.printf("[BOND] %s: AUTHENTICATED as its bridge bond (owns relay, never takes the app slot)\n",
                      l.deviceId.c_str());
        g_bb.ok++;
        bbEnd(l, BB_OK);
      }
      break;
    default:
      break;
    }
  }
  return false;
}
