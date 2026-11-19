// ozbcmd.h — BRIDGE-CMD-AUTH-1: verify signed bridge commands (CONTRACT-BRIDGE.md,
// "MQTT signed bridge commands", v1). Plain C/C++ (no Arduino): host-tested by
// test/test_bcmd.cpp against the shared vectors (ozcore/tests/vectors/bridge_cmd_v1.*).
//
// Frame = sig(64) ‖ body; body = CBOR [v, op, bridge, ctr, args];
// sig = Ed25519 (RFC 8032) over "ozkie-bcmd-v1" ‖ body, by the site key.
// The order of checks is the contract's: no key → refuse; size; SIGNATURE
// BEFORE ANY PARSING; then v / bridge / op / args; then the counter.
#ifndef OZBCMD_H
#define OZBCMD_H
#include <stdint.h>
#include <string.h>
#include "oz_cbor.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#define OZB_BODY_MAX   512u
#define OZB_DOMAIN     "ozkie-bcmd-v1"
#define OZB_DOMAIN_LEN 13u
#define OZB_TIME_SAVE_MS (24ull * 3600ull * 1000ull)   // persist a time ctr this far above NVS

enum { OZB_OP_NONE = 0, OZB_OP_PAIR_LOCK, OZB_OP_UNPAIR_LOCK, OZB_OP_RELEASE_LOCK, OZB_OP_SCAN,
       OZB_OP_FACTORY_RESET, OZB_OP_UNPAIR, OZB_OP_TIME, OZB_OP_SET_SITE_KEY, OZB_OP_TZ };

// Results. OZB_DUP = a QoS 1 redelivery: not acted on, not a refusal.
enum { OZB_OK = 0, OZB_DUP, OZB_E_NOKEY, OZB_E_SIG, OZB_E_PARSE, OZB_E_BRIDGE, OZB_E_REPLAY, OZB_E_OP,
       OZB_N_RESULTS };

struct OzBcmdState {
  uint8_t  has_key;
  uint8_t  site_pub[32];
  uint64_t last;            // highest accepted ctr (RAM; >= nvs_last)
  uint64_t nvs_last;        // the copy in NVS
  uint8_t  last_hash[32];   // BLAKE2b of the last accepted frame (duplicates)
  uint8_t  has_hash;
};

struct OzBcmd {
  int      op;
  uint64_t ctr;
  char     id[32];          // pair_lock / unpair_lock / release_lock: the lock; scan: target ("" = none)
  uint32_t duration_s;      // scan
  uint64_t utc;             // time
  int32_t  tz_min;          // tz: minutes east of UTC, -720..840
  uint8_t  new_pub[32];     // set_site_key
  int      persist;         // 1: write ctr to NVS BEFORE acting (then ozBcmdSaved)
};

// Commissioning (provision payload, claim window) or the bench console.
static inline void ozBcmdProvision(OzBcmdState *st, const uint8_t pub[32], uint64_t ctr_floor) {
  memset(st, 0, sizeof *st);
  memcpy(st->site_pub, pub, 32);
  st->has_key = 1;
  st->last = st->nvs_last = ctr_floor;
}

// The caller wrote out->ctr to NVS.
static inline void ozBcmdSaved(OzBcmdState *st, uint64_t ctr) { if (ctr > st->nvs_last) st->nvs_last = ctr; }

// After an accepted set_site_key, once the new key is in NVS.
static inline void ozBcmdRotate(OzBcmdState *st, const uint8_t pub[32]) { memcpy(st->site_pub, pub, 32); }

// A text string into dst (NUL-terminated); 0 if it is not a tstr or too long.
static inline int ozBcmdTstr(oz_cbor_r *r, char *dst, size_t cap) {
  if (oz_cbor_peek(r) != OZ_CBOR_TSTR) return 0;
  size_t n;
  const uint8_t *v = oz_cbor_get_tstr(r, &n);
  if (!v || r->err || n == 0 || n >= cap) return 0;
  memcpy(dst, v, n);
  dst[n] = 0;
  return 1;
}

// The args map of one op into out. 0 = malformed / missing a required field.
// Unknown keys are skipped (a newer server may add optional fields).
static inline int ozBcmdArgs(oz_cbor_r *r, int op, OzBcmd *out) {
  const int null_args = op == OZB_OP_FACTORY_RESET || op == OZB_OP_UNPAIR;
  if (r->pos < r->len && r->p[r->pos] == 0xf6) {            // CBOR null
    r->pos++;
    return null_args;
  }
  if (null_args || oz_cbor_peek(r) != OZ_CBOR_MAP) return 0;
  size_t pairs = oz_cbor_get_map(r);
  int have_id = 0, have_dur = 0, have_utc = 0, have_pub = 0, have_tz = 0;
  for (size_t i = 0; i < pairs && !r->err; i++) {
    char key[16];
    if (!ozBcmdTstr(r, key, sizeof key)) return 0;
    if ((!strcmp(key, "id") && op != OZB_OP_SCAN) || (!strcmp(key, "target") && op == OZB_OP_SCAN)) {
      if (!ozBcmdTstr(r, out->id, sizeof out->id)) return 0;
      have_id = 1;
    } else if (!strcmp(key, "duration_s") && op == OZB_OP_SCAN) {
      if (oz_cbor_peek(r) != OZ_CBOR_UINT) return 0;
      uint64_t d = oz_cbor_get_uint(r);
      if (d < 1 || d > 30) return 0;
      out->duration_s = (uint32_t)d;
      have_dur = 1;
    } else if (!strcmp(key, "utc") && op == OZB_OP_TIME) {
      if (oz_cbor_peek(r) != OZ_CBOR_UINT) return 0;
      out->utc = oz_cbor_get_uint(r);
      have_utc = 1;
    } else if (!strcmp(key, "min") && op == OZB_OP_TZ) {
      const int t = oz_cbor_peek(r);
      if (t != OZ_CBOR_UINT && t != OZ_CBOR_NINT) return 0;
      const int64_t m = oz_cbor_get_int(r);
      if (r->err || m < -720 || m > 840) return 0;
      out->tz_min = (int32_t)m;
      have_tz = 1;
    } else if (!strcmp(key, "pub") && op == OZB_OP_SET_SITE_KEY) {
      size_t n;
      const uint8_t *v = oz_cbor_peek(r) == OZ_CBOR_BSTR ? oz_cbor_get_bstr(r, &n) : NULL;
      if (!v || n != 32) return 0;
      memcpy(out->new_pub, v, 32);
      have_pub = 1;
    } else {
      size_t n;
      if (!oz_cbor_skip(r, &n)) return 0;
    }
  }
  if (r->err) return 0;
  switch (op) {
    case OZB_OP_PAIR_LOCK: case OZB_OP_UNPAIR_LOCK: case OZB_OP_RELEASE_LOCK: return have_id;
    case OZB_OP_SCAN: return have_dur;                         // target is optional
    case OZB_OP_TIME: return have_utc;
    case OZB_OP_SET_SITE_KEY: return have_pub;
    case OZB_OP_TZ: return have_tz;
    default: return 0;
  }
}

static inline int ozBcmdOpOf(const char *s) {
  static const struct { const char *name; int op; } t[] = {
    { "pair_lock", OZB_OP_PAIR_LOCK }, { "unpair_lock", OZB_OP_UNPAIR_LOCK },
    { "release_lock", OZB_OP_RELEASE_LOCK }, { "scan", OZB_OP_SCAN },
    { "factory_reset", OZB_OP_FACTORY_RESET }, { "unpair", OZB_OP_UNPAIR },
    { "time", OZB_OP_TIME }, { "set_site_key", OZB_OP_SET_SITE_KEY }, { "tz", OZB_OP_TZ },
  };
  for (auto &e : t) if (!strcmp(s, e.name)) return e.op;
  return OZB_OP_NONE;
}

// One received frame. On OZB_OK the caller: if out->persist, writes out->ctr to
// NVS (then ozBcmdSaved) BEFORE acting; a failed write = do not act.
static inline int ozBcmdCheck(OzBcmdState *st, const char *self_id, const uint8_t *pkt, size_t len, OzBcmd *out) {
  memset(out, 0, sizeof *out);
  if (!st->has_key) return OZB_E_NOKEY;
  if (len < 64 + 1 || len > 64 + OZB_BODY_MAX) return OZB_E_PARSE;
  // 1. The signature, before anything in the body is looked at.
  static uint8_t msg[OZB_DOMAIN_LEN + OZB_BODY_MAX];
  memcpy(msg, OZB_DOMAIN, OZB_DOMAIN_LEN);
  memcpy(msg + OZB_DOMAIN_LEN, pkt + 64, len - 64);
  if (crypto_ed25519_check(pkt, st->site_pub, msg, OZB_DOMAIN_LEN + len - 64) != 0) return OZB_E_SIG;
  // 2. The body: [v, op, bridge, ctr, args], nothing after it.
  oz_cbor_r r;
  oz_cbor_r_init(&r, pkt + 64, len - 64);
  char op[24], bridge[32];
  if (oz_cbor_peek(&r) != OZ_CBOR_ARRAY || oz_cbor_get_array(&r) != 5) return OZB_E_PARSE;
  if (oz_cbor_peek(&r) != OZ_CBOR_UINT || oz_cbor_get_uint(&r) != 1) return OZB_E_PARSE;
  if (!ozBcmdTstr(&r, op, sizeof op) || !ozBcmdTstr(&r, bridge, sizeof bridge)) return OZB_E_PARSE;
  if (oz_cbor_peek(&r) != OZ_CBOR_UINT) return OZB_E_PARSE;
  out->ctr = oz_cbor_get_uint(&r);
  if (r.err) return OZB_E_PARSE;
  if (strcmp(bridge, self_id) != 0) return OZB_E_BRIDGE;
  out->op = ozBcmdOpOf(op);
  if (out->op == OZB_OP_NONE) return OZB_E_OP;
  if (!ozBcmdArgs(&r, out->op, out) || !oz_cbor_at_end(&r)) return OZB_E_PARSE;
  // 3. The counter: strictly increasing; the same bytes again = a QoS 1 duplicate.
  uint8_t h[32];
  crypto_blake2b(h, sizeof h, pkt, len);
  if (out->ctr == st->last && st->has_hash && !memcmp(h, st->last_hash, 32)) return OZB_DUP;
  if (out->ctr <= st->last) return OZB_E_REPLAY;
  st->last = out->ctr;
  memcpy(st->last_hash, h, 32);
  st->has_hash = 1;
  // time and tz (PM 2026-11-20: tz has time's counter rules): RAM unless 24 h ahead.
  out->persist = (out->op != OZB_OP_TIME && out->op != OZB_OP_TZ) || out->ctr > st->nvs_last + OZB_TIME_SAVE_MS;
  return OZB_OK;
}

static inline const char *ozBcmdResultName(int r) {
  static const char *const n[OZB_N_RESULTS] = { "ok", "dup", "nokey", "sig", "parse", "bridge", "replay", "op" };
  return (r >= 0 && r < OZB_N_RESULTS) ? n[r] : "?";
}
#endif
