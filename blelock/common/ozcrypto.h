// ozcrypto.h — blecomm member-ceremony crypto (XFtposDecisions-46 §7.1)
//
// Mirrors the Dart `ozkey_commissioner` package byte-for-byte so a BANOI phone
// and this firmware agree without a network round-trip. All primitives come
// from the ESP32 core's bundled mbedTLS (core 3.3.10 config verified to ship
// CURVE25519 / ECDH_C / HKDF_C / SHA256_C).
//
// Conventions locked to RFC 7748 + the frozen vectors:
//   • X25519 keys + shared secret are 32-byte little-endian (RFC 7748).
//   • invite MAC = HMAC-SHA256( HKDF-SHA256(ikm=s0,
//        salt=utf8(device_id‖issuer_app_id_hex), info="ozkey/invite-v1"),
//        utf8("1|device|issuer|role|label|nonce|expires") ).
//
// ozCryptoSelfTest() checks both against known-answer vectors at boot and logs
// the verdict over serial — the first flash tells us the mbedTLS wiring is
// interoperable before any dependent GATT logic is built on top.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "esp_random.h"
#define MBEDTLS_ALLOW_PRIVATE_ACCESS // ecp_point X/Y/Z are private in mbedTLS 3.x
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

extern Preferences prefs; // namespace "blelock" (declared in blecomm.ino)

// The lock's ceremony identity — minted once, NVS-persisted, survives
// re-provision, wiped only by factoryReset()'s prefs.clear().
static uint8_t g_lockPriv[32];
static uint8_t g_lockPub[32];
static bool g_lockKeyReady = false;

// ── low-level helpers ─────────────────────────────────────────────────────────

static void ozHex(const uint8_t *b, size_t n, char *out /* 2n+1 */) {
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = H[b[i] >> 4];
    out[2 * i + 1] = H[b[i] & 0xF];
  }
  out[2 * n] = 0;
}

// mbedTLS RNG callback backed by the ESP32 hardware TRNG (RF must be on, which
// it is once WiFi/BLE init — key mint happens after that in setup()).
static int ozRng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static void ozClamp(uint8_t k[32]) {
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
}

// RFC 7748 X25519: out = scalar · u  (all 32-byte little-endian).
// Returns true on success.
static bool ozX25519(const uint8_t scalar[32], const uint8_t u[32],
                     uint8_t out[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point Q, R;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&Q);
  mbedtls_ecp_point_init(&R);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
    uint8_t s[32];
    memcpy(s, scalar, 32);
    ozClamp(s); // idempotent; mbedTLS also masks, double-clamp is safe
    if (mbedtls_mpi_read_binary_le(&d, s, 32) != 0) break;
    // Montgomery point = its u-coordinate in X, Z = 1 (Y unused).
    if (mbedtls_mpi_read_binary_le(&Q.MBEDTLS_PRIVATE(X), u, 32) != 0) break;
    if (mbedtls_mpi_lset(&Q.MBEDTLS_PRIVATE(Z), 1) != 0) break;
    if (mbedtls_ecp_mul(&grp, &R, &d, &Q, ozRng, nullptr) != 0) break;
    if (mbedtls_mpi_write_binary_le(&R.MBEDTLS_PRIVATE(X), out, 32) != 0) break;
    ok = true;
  } while (false);
  mbedtls_ecp_point_free(&R);
  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// out = scalar · basepoint(9) — the public key for a private scalar.
static bool ozX25519Base(const uint8_t scalar[32], uint8_t out[32]) {
  uint8_t base[32] = {9}; // u = 9, little-endian
  return ozX25519(scalar, base, out);
}

static bool ozHkdfSha256(const uint8_t *ikm, size_t ikmLen, const uint8_t *salt,
                         size_t saltLen, const uint8_t *info, size_t infoLen,
                         uint8_t *out, size_t outLen) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md &&
         mbedtls_hkdf(md, salt, saltLen, ikm, ikmLen, info, infoLen, out,
                      outLen) == 0;
}

static bool ozHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg,
                         size_t msgLen, uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && mbedtls_md_hmac(md, key, keyLen, msg, msgLen, out) == 0;
}

// Recompute a member-invite MAC under bond #0's pairing secret [s0]. The
// canonical string + salt layout are frozen (ftpos member_invite.dart).
// XF-87 — v1 AND v2. v2 appends the MEMBERSHIP expiry to the signed canonical
// string, which is what lets the lock enforce "3 days then gone" itself instead
// of trusting the member's own phone to stop working (ozkey-21).
//
//   v1: 1|device|issuer|role|label|nonce|expires
//   v2: 2|device|issuer|role|label|nonce|expires|membershipExpires
//
// `membershipExpires` is rendered as literal 0 for "permanent", never omitted —
// an empty slot in a canonical string is how canonicalisation bugs turn into
// forgeries, and ftpos encode it the same way (member_invite.dart `?? 0`).
//
// The HKDF info stays "ozkey/invite-v1" at BOTH versions. It is a frozen crypto
// constant, not a version marker and not a name — see XF-85 §2, which is the
// exact trap of find-replacing `ozkey/` during the trademark rename. ftpos did
// not rename theirs either; if these two strings ever diverge, every invite
// fails to verify with no useful error.
static bool ozInviteMac(const uint8_t *s0, size_t s0Len, const String &deviceId,
                        const String &issuerAppId, const String &roleWire,
                        const String &label, const String &nonceHex,
                        uint32_t expires, uint8_t out[32], int version = 1,
                        uint32_t membershipExpires = 0) {
  String salt = deviceId + issuerAppId;
  uint8_t macKey[32];
  if (!ozHkdfSha256(s0, s0Len, (const uint8_t *)salt.c_str(), salt.length(),
                    (const uint8_t *)"ozkey/invite-v1", 15, macKey, 32))
    return false;
  String canonical = String(version) + "|" + deviceId + "|" + issuerAppId + "|" +
                     roleWire + "|" + label + "|" + nonceHex + "|" +
                     String(expires);
  if (version >= 2) canonical += "|" + String(membershipExpires);
  return ozHmacSha256(macKey, 32, (const uint8_t *)canonical.c_str(),
                      canonical.length(), out);
}

// ── lock keypair lifecycle ────────────────────────────────────────────────────

// Load the ceremony keypair from NVS, or mint + persist one on first boot.
// Call after WiFi/BLE init so the TRNG is seeded.
static void ozLockKeyInit() {
  prefs.begin("blelock", true);
  size_t haveP = prefs.getBytesLength("xpriv");
  size_t haveB = prefs.getBytesLength("xpub");
  if (haveP == 32 && haveB == 32) {
    prefs.getBytes("xpriv", g_lockPriv, 32);
    prefs.getBytes("xpub", g_lockPub, 32);
    prefs.end();
    g_lockKeyReady = true;
    Serial.println("[CRYPTO] lock keypair loaded from NVS");
    return;
  }
  prefs.end();

  esp_fill_random(g_lockPriv, 32);
  ozClamp(g_lockPriv);
  if (!ozX25519Base(g_lockPriv, g_lockPub)) {
    Serial.println("[CRYPTO] FATAL: keypair derivation failed");
    return;
  }
  prefs.begin("blelock", false);
  prefs.putBytes("xpriv", g_lockPriv, 32);
  prefs.putBytes("xpub", g_lockPub, 32);
  prefs.end();
  g_lockKeyReady = true;
  Serial.println("[CRYPTO] lock keypair minted + persisted");
}

// Lowercase 64-hex of the lock's X25519 public key (info.pub). Empty until
// ozLockKeyInit() has run.
static String ozLockPubHex() {
  if (!g_lockKeyReady) return String("");
  char h[65];
  ozHex(g_lockPub, 32, h);
  return String(h);
}

// ── M2: bond #0, the owner bond ───────────────────────────────────────────────
//
// Defined here rather than beside the self-test because the provisioning path
// needs it; ozFromHex() lives further down with the vector helpers, hence the
// forward declaration.
static void ozFromHex(const char *hex, uint8_t *out, size_t n);
// Likewise the big-endian u64 helpers, defined with the envelope further down —
// the bond table persists counter_floor in the same wire-order the envelope uses.
static void ozPutU64BE(uint64_t v, uint8_t out[8]);
static uint64_t ozGetU64BE(const uint8_t in[8]);
//
// CONTRACT.md "Bond #0 bootstrap" (XF-47, canonical). Bond #0 is minted from the
// `app_id` field ALREADY present in the provision payload — BANOI has sent it
// since `provision_payload.dart:63` — so commissioning creates the owner bond
// with zero wire change.
//
// Stored in the same "blelock" namespace as the lock keypair. That is
// deliberate: `factoryReset()` does `prefs.clear()` on this namespace, so a
// factory reset wipes the bond AND mints a new keypair, and per CONTRACT.md a
// factory reset is the ONLY way to clear ownership. A re-provision can never
// overwrite it — see OZ_BOND_DENIED below.
//
// counter_floor is persisted from the start even though nothing moves it until
// M4: a bond whose floor resets to 0 on reboot would re-open every captured
// frame, and the anti-replay rule (XF-47) leans on the floor surviving.

static const uint8_t OZ_ROLE_ADMIN  = 0;
static const uint8_t OZ_ROLE_MEMBER = 1;

// ── M3: the bond TABLE (bond #0 is slot 0) ───────────────────────────────────
//
// M2 stored the owner bond in three flat NVS keys (b0pub/b0role/b0ctr) because
// there was exactly one. M3 adds members, so the same data becomes slot 0 of a
// 16-slot table persisted as ONE blob ("bondtab"). CONTRACT.md caps bonds at 16
// (1 admin + up to 15 members).
//
// One blob rather than 16×3 keys: a bond is added, revoked and floor-bumped as a
// unit, and NVS gives no transaction across keys — a partial write that left a
// pubkey without its role would be an unauthenticated bond. 1280 B is one NVS
// page-ish write, and enrolments are rare.
//
// MIGRATION is silent and one-way: a lock already carrying b0pub from M2 loads
// it into slot 0 on first M3 boot, writes the table, then deletes the old keys
// so a stale owner cannot linger in an NVS dump and mislead a bench session.

#define OZ_BOND_MAX   16
#define OZ_LABEL_MAX  32  // bytes, UTF-8, NUL-terminated (labels live ON the lock)

// ── RECORD STRIDE — grew 80 -> 88 for ozkey-21 T4 (expires_at) ───────────────
//
// The old stride said "6 bytes spare for v2 fields"; U0's txReserved took all
// six (offset 74..79), so the 80-byte record was FULL. T4 needs 4 more, so the
// stride grows and OZ_BONDTAB_SZ with it.
//
// 🔴 THAT IS A MIGRATION, NOT A RECOMPILE. ozBondsLoad() gates on an EXACT
// length match, so a lock upgraded in the field would have read its 1280-byte
// blob as "no table" and silently come up with NO BONDS — no owner, no members,
// unrecoverable without a factory reset and a re-pair. Every deployed lock,
// on the OTA that shipped it. ozBondsLoad() below therefore reads BOTH strides.
//
// Layout v2 (88): 0 present | 1 role | 2..9 floor | 10..41 pub | 42..73 label
//                 74..79 txReserved | 80..83 expiresAt | 84..87 spare(v3)
#define OZ_BOND_REC    88
#define OZ_BOND_REC_V1 80  // pre-T4 stride — read-only, for the upgrade path
#define OZ_BONDTAB_SZ    (OZ_BOND_REC    * OZ_BOND_MAX)
#define OZ_BONDTAB_SZ_V1 (OZ_BOND_REC_V1 * OZ_BOND_MAX)

// ozkey-17 U0: 48-bit big-endian, for the outbound counter below. Six bytes,
// not eight, because that is exactly what OZ_BOND_REC left spare — and growing
// the record would change OZ_BONDTAB_SZ, which ozBondsLoad() compares against
// getBytesLength() to decide whether the blob is valid. A longer record would
// therefore make every existing table fail that check and silently drop every
// bond on the first boot after the upgrade. 2^48 sends is ~9 million years at
// one per second; the spare bytes are the right size and always were.
static void ozPutU48BE(uint64_t v, uint8_t out[6]) {
  for (int i = 5; i >= 0; i--) { out[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}
static uint64_t ozGetU48BE(const uint8_t in[6]) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | in[i];
  return v;
}

struct OzBond {
  bool     present;
  uint8_t  role;
  uint64_t floor;              // counter_floor — anti-replay, M4 moves it
  uint8_t  pub[32];            // the member's X25519 public key == its app_id
  char     label[OZ_LABEL_MAX];

  // ── ozkey-21 T4: membership expiry ────────────────────────────────────────
  // UTC seconds. 0 = PERMANENT, and 0 is what every pre-T4 bond reads back as,
  // which is why this is additive: nobody already enrolled changes behaviour.
  //
  // Set from the invite's `me` field, which is SIGNED inside the invite MAC at
  // v2 (XF-87). We have verified that value since v2 shipped and then thrown it
  // away — see ozkey-21 §9, where a membership that expired at 12:38 opened the
  // door at 12:39 on real hardware. This is the field that was missing.
  uint32_t expiresAt;

  // ── ozkey-17 U0: lock->app send counter ────────────────────────────────────
  // `floor` protects traffic coming IN. Sealing traffic going OUT (U1's uplink,
  // query responses, pushed roster_changed) needs its own strictly-increasing
  // counter, or the app cannot tell a fresh reply from a captured one.
  //
  // Only `txReserved` is persisted. Bumping NVS on every single send would be
  // ~1400 writes/day at a 60 s heartbeat, which is real flash wear for no gain,
  // so we reserve a block of counters per write instead: hand out from RAM
  // until the reservation is exhausted, then persist a new high-water mark.
  // After a reboot we resume from the persisted mark, skipping at most
  // OZ_TX_RESERVE unused values — harmless, because the app requires strictly
  // increasing, not gapless. This is what makes the counter survive the
  // brownout reboots this hardware actually does.
  uint64_t txCounter;          // RAM only — last counter handed out
  uint64_t txReserved;         // persisted high-water mark (48-bit on NVS)
};

#define OZ_TX_RESERVE 64       // counters claimed per NVS write

static OzBond g_bonds[OZ_BOND_MAX];

enum OzBondVerdict {
  OZ_BOND_ABSENT,    // no app_id in payload -> legacy path: no bond, NO error
  OZ_BOND_MALFORMED, // app_id present but not exactly 64 hex chars
  OZ_BOND_CREATE,    // no bond #0 yet -> mint it when the payload is accepted
  OZ_BOND_SAME,      // bond #0 exists and matches -> idempotent re-provision
  OZ_BOND_DENIED     // bond #0 exists and DIFFERS -> refuse, change NOTHING
};

static bool ozBond0Present() { return g_bonds[0].present; }

static int ozBondCount() {
  int n = 0;
  for (int i = 0; i < OZ_BOND_MAX; i++) if (g_bonds[i].present) n++;
  return n;
}

// Slot index of the bond holding [pub], or -1. Linear over 16 — the table is
// tiny and this runs once per enrol/control, not per packet.
static int ozBondFind(const uint8_t pub[32]) {
  for (int i = 0; i < OZ_BOND_MAX; i++)
    if (g_bonds[i].present && memcmp(g_bonds[i].pub, pub, 32) == 0) return i;
  return -1;
}

static void ozBondsSave() {
  uint8_t *buf = (uint8_t *)calloc(1, OZ_BONDTAB_SZ);
  if (!buf) {
    Serial.println("[BOND] FATAL: no heap for bond table save");
    return;
  }
  for (int i = 0; i < OZ_BOND_MAX; i++) {
    uint8_t *r = buf + i * OZ_BOND_REC;
    r[0] = g_bonds[i].present ? 1 : 0;
    r[1] = g_bonds[i].role;
    ozPutU64BE(g_bonds[i].floor, r + 2);
    memcpy(r + 10, g_bonds[i].pub, 32);
    memcpy(r + 42, g_bonds[i].label, OZ_LABEL_MAX);
    ozPutU48BE(g_bonds[i].txReserved, r + 74); // U0 — the old 6 spare bytes
    // T4: expiresAt, big-endian like every other integer in this record.
    // 84..87 stay zeroed by the calloc above, reserved for v3.
    r[80] = (uint8_t)(g_bonds[i].expiresAt >> 24);
    r[81] = (uint8_t)(g_bonds[i].expiresAt >> 16);
    r[82] = (uint8_t)(g_bonds[i].expiresAt >> 8);
    r[83] = (uint8_t)(g_bonds[i].expiresAt);
  }
  prefs.begin("blelock", false);
  prefs.putBytes("bondtab", buf, OZ_BONDTAB_SZ);
  prefs.end();
  free(buf);
}

// ozkey-17 U0: hand out the next lock->app counter for [slot], persisting a
// fresh reservation only when the current block runs out. Returns 0 if the slot
// holds no bond — 0 is never a valid outbound counter, so it doubles as the
// error signal and a caller that ignores it seals nothing.
static uint64_t ozBondNextTx(int slot) {
  if (slot < 0 || slot >= OZ_BOND_MAX || !g_bonds[slot].present) return 0;
  OzBond &b = g_bonds[slot];
  b.txCounter++;
  if (b.txCounter > b.txReserved) {
    b.txReserved = b.txCounter + OZ_TX_RESERVE;
    ozBondsSave(); // one NVS write per OZ_TX_RESERVE sends, not per send
  }
  return b.txCounter;
}

static void ozBondsLoad() {
  memset(g_bonds, 0, sizeof(g_bonds));

  prefs.begin("blelock", true);
  // T4 MIGRATION: accept BOTH strides. The v1 blob (1280 B) predates
  // expires_at; reading it with the v2 stride would walk off the end of every
  // record after the first and produce garbage bonds, and REJECTING it (the
  // old exact-match behaviour) would silently drop the owner on upgrade.
  const size_t tabLen = prefs.getBytesLength("bondtab");
  const bool   isV2   = (tabLen == OZ_BONDTAB_SZ);
  const bool   isV1   = (tabLen == OZ_BONDTAB_SZ_V1);
  if (isV2 || isV1) {
    const size_t stride = isV2 ? OZ_BOND_REC : OZ_BOND_REC_V1;
    uint8_t *buf = (uint8_t *)malloc(tabLen);
    if (buf) {
      prefs.getBytes("bondtab", buf, tabLen);
      for (int i = 0; i < OZ_BOND_MAX; i++) {
        const uint8_t *r = buf + i * stride;
        g_bonds[i].present = (r[0] == 1);
        g_bonds[i].role    = r[1];
        g_bonds[i].floor   = ozGetU64BE(r + 2);
        memcpy(g_bonds[i].pub, r + 10, 32);
        memcpy(g_bonds[i].label, r + 42, OZ_LABEL_MAX);
        g_bonds[i].label[OZ_LABEL_MAX - 1] = 0; // never trust NVS to terminate
        // U0: resume the send counter at the persisted reservation, so the
        // next value handed out is strictly above anything used before the
        // reboot. A pre-U0 table has these bytes zeroed by the calloc in
        // ozBondsSave(), which starts the counter at 0 — exactly right.
        g_bonds[i].txReserved = ozGetU48BE(r + 74);
        g_bonds[i].txCounter  = g_bonds[i].txReserved;
        // T4: a v1 record has no expiry to read. 0 = permanent, which is the
        // correct reading of a bond enrolled before expiry existed — it was
        // granted without a limit and we do not get to invent one now.
        g_bonds[i].expiresAt =
            isV2 ? ((uint32_t)r[80] << 24 | (uint32_t)r[81] << 16 |
                    (uint32_t)r[82] << 8  | (uint32_t)r[83])
                 : 0u;
      }
      free(buf);
    }
    prefs.end();
    Serial.printf("[BOND] table loaded (v%d) — %d bond(s)\n", isV2 ? 2 : 1,
                  ozBondCount());
    if (isV1) {
      // Rewrite at the new stride immediately, so this only happens once and a
      // later save cannot be the first thing to touch the format.
      Serial.println("[BOND] migrating bond table v1 -> v2 (T4 expires_at)");
      ozBondsSave();
    }
    return;
  }
  if (tabLen) // present but neither stride — do not guess at a layout
    Serial.printf("[BOND] bondtab has unexpected length %u — ignored\n",
                  (unsigned)tabLen);

  // No table yet: migrate the M2 singleton if this lock already has an owner.
  const bool haveM2 = (prefs.getBytesLength("b0pub") == 32);
  if (haveM2) {
    g_bonds[0].present = true;
    prefs.getBytes("b0pub", g_bonds[0].pub, 32);
    g_bonds[0].role  = prefs.getUChar("b0role", OZ_ROLE_ADMIN);
    g_bonds[0].floor = prefs.getULong64("b0ctr", 0);
    strncpy(g_bonds[0].label, "owner", OZ_LABEL_MAX - 1);
  }
  prefs.end();

  if (haveM2) {
    ozBondsSave();
    prefs.begin("blelock", false);
    prefs.remove("b0pub");
    prefs.remove("b0role");
    prefs.remove("b0ctr");
    prefs.end();
    Serial.println("[BOND] migrated M2 bond #0 into the M3 table (old keys removed)");
  }
}


// Strict: exactly n*2 chars, all hex. ozFromHex() silently maps junk to 0, so an
// app_id of "zzzz…" would otherwise bond a lock to an all-zero pubkey nobody
// holds the private half of — unrecoverable without a factory reset.
static bool ozIsHex(const char *h, size_t nbytes) {
  if (!h) return false;
  size_t i = 0;
  for (; h[i]; i++) {
    const char c = h[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return i == nbytes * 2;
}

// Pure decision — writes nothing. The caller must be able to refuse atomically,
// which means knowing the verdict BEFORE it has mutated any state.
static OzBondVerdict ozBond0Evaluate(const char *appIdHex, uint8_t outPub[32]) {
  if (!appIdHex || !*appIdHex) return OZ_BOND_ABSENT;
  if (!ozIsHex(appIdHex, 32)) return OZ_BOND_MALFORMED;
  ozFromHex(appIdHex, outPub, 32);
  if (!g_bonds[0].present) return OZ_BOND_CREATE;
  return memcmp(outPub, g_bonds[0].pub, 32) == 0 ? OZ_BOND_SAME : OZ_BOND_DENIED;
}

// Called ONLY on the accept path, after the payload is known good.
//
// M3: this mints slot 0 and deliberately leaves slots 1-15 alone. A re-provision
// by the SAME owner never reaches here (OZ_BOND_SAME is idempotent) and one by a
// different owner is refused before this point, so there is no path where a
// member silently survives an ownership change — the only way to slot 0 with a
// new key is a factory reset, which clears the whole table with prefs.clear().
static void ozBond0Commit(const uint8_t pub[32]) {
  memcpy(g_bonds[0].pub, pub, 32);
  g_bonds[0].role    = OZ_ROLE_ADMIN;
  g_bonds[0].floor   = 0;
  g_bonds[0].present = true;
  strncpy(g_bonds[0].label, "owner", OZ_LABEL_MAX - 1);
  ozBondsSave();
}

static String ozBond0PubHex() {
  if (!g_bonds[0].present) return String("");
  char h[65];
  ozHex(g_bonds[0].pub, 32, h);
  return String(h);
}

// ── M3: bond #0's pairing secret — the root the invite MAC hangs off ──────────
//
// s0 = X25519(lock_priv, bond0_pub). The app computes the mirror image,
// X25519(app_priv, lock_pub), and the two agree by ECDH. Nothing persists it:
// it is 32 bytes of derivation from two keys we already hold, and re-deriving
// per enrolment (a few ms) is cheaper than owning a second secret at rest.
// M4 generalises this to any slot: `control` frames are opened under the SENDER's
// pairing secret, and the sender may be any bond, not just the owner.
static bool ozBondSecret(int slot, uint8_t out[32]) {
  if (slot < 0 || slot >= OZ_BOND_MAX) return false;
  if (!g_lockKeyReady || !g_bonds[slot].present) return false;
  return ozX25519(g_lockPriv, g_bonds[slot].pub, out);
}

static bool ozBond0Secret(uint8_t out[32]) { return ozBondSecret(0, out); }

// ── M4: revocation (DPID 101) ────────────────────────────────────────────────
//
// Clearing `present` is not enough. The pubkey must go too: ozBondFind() skips
// absent slots, but a revoked key left in NVS is a key an attacker who dumps the
// flash can still tie to this lock, and a later re-invite lands on a slot that
// still holds its predecessor's counter_floor. Wipe the record and let the
// re-invite path build it fresh — that is the branch XF-47's floor rule assumes.
//
// Slot 0 is never revocable here; the caller enforces that, but assert it too so
// a future caller cannot orphan the lock by revoking its own owner.
static bool ozBondRevoke(int slot) {
  if (slot <= 0 || slot >= OZ_BOND_MAX) return false;
  if (!g_bonds[slot].present) return false;
  memset(&g_bonds[slot], 0, sizeof(OzBond));
  ozBondsSave();
  return true;
}

// ── M4: the invite-cancel marker (DPID 102) ──────────────────────────────────
//
// Cancelling an unredeemed invite burns its nonce against an all-zero pubkey.
// That value is deliberately not a usable X25519 public key, so no real member
// can ever present it: a later redeem of the cancelled nonce arrives with the
// member's real key, ozNonceCheck() sees a DIFFERENT pubkey, and the enrolment
// is refused as OZ_NONCE_REPLAY. The kill switch reuses the replay machinery
// rather than adding a second list to keep consistent with it.
static const uint8_t OZ_NONCE_CANCELLED[32] = {0};

// Constant-time equality. Used on MAC comparison, where an early-exit memcmp
// leaks the length of a correct prefix and turns forgery into 32 × 256 guesses.
static bool ozCtEq(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// ── M3: nonce replay cache (XF-47) ───────────────────────────────────────────
//
// Entry = {nonce[16], member_pubkey[32]}, 64 entries, one NVS blob, FIFO ring.
//
// TWO rules from XF-47, both load-bearing, neither obvious:
//  1. ONLY SUCCESSFUL enrolments write here. If failures wrote, an attacker
//     floods 64 junk nonces, evicts the record of a real one, and a captured
//     invite becomes replayable — the cache would defeat itself.
//  2. Burned nonce + MATCHING pubkey is an idempotent MEMBER_OK, not a replay.
//     Without that, one dropped BLE notify strands a member permanently: they
//     retry, get MEMBER_REPLAY, and the bond slot is consumed with no way back.
//
// Eviction is FIFO rather than LRU. LRU needs a touch on every read and buys
// nothing here: entries are never re-read in the normal case (a nonce is
// redeemed once), so recency and insertion order are the same ordering.

#define OZ_NONCE_MAX 64
#define OZ_NONCE_REC 48                                  // 16 nonce + 32 pubkey
#define OZ_NONCE_HDR 4                                   // count, head, 2 spare
#define OZ_NONCE_SZ  (OZ_NONCE_HDR + OZ_NONCE_REC * OZ_NONCE_MAX)

enum OzNonceState {
  OZ_NONCE_FRESH,    // never seen -> proceed with the enrolment
  OZ_NONCE_SAME_PUB, // burned by THIS pubkey -> idempotent retry, MEMBER_OK
  OZ_NONCE_REPLAY    // burned by a DIFFERENT pubkey -> MEMBER_REPLAY
};

// Read the blob into [buf] (OZ_NONCE_SZ bytes). Absent/short = empty cache.
static void ozNonceRead(uint8_t *buf) {
  memset(buf, 0, OZ_NONCE_SZ);
  prefs.begin("blelock", true);
  if (prefs.getBytesLength("noncecache") == OZ_NONCE_SZ)
    prefs.getBytes("noncecache", buf, OZ_NONCE_SZ);
  prefs.end();
}

static OzNonceState ozNonceCheck(const uint8_t nonce[16], const uint8_t pub[32]) {
  uint8_t *buf = (uint8_t *)malloc(OZ_NONCE_SZ);
  if (!buf) return OZ_NONCE_REPLAY; // fail CLOSED: no heap must not mean no check
  ozNonceRead(buf);
  const uint8_t count = buf[0] > OZ_NONCE_MAX ? OZ_NONCE_MAX : buf[0];
  OzNonceState st = OZ_NONCE_FRESH;
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t *r = buf + OZ_NONCE_HDR + i * OZ_NONCE_REC;
    if (memcmp(r, nonce, 16) == 0) {
      st = ozCtEq(r + 16, pub, 32) ? OZ_NONCE_SAME_PUB : OZ_NONCE_REPLAY;
      break;
    }
  }
  free(buf);
  return st;
}

static void ozNonceBurn(const uint8_t nonce[16], const uint8_t pub[32]) {
  uint8_t *buf = (uint8_t *)malloc(OZ_NONCE_SZ);
  if (!buf) {
    Serial.println("[MEMBER] FATAL: no heap for nonce cache write");
    return;
  }
  ozNonceRead(buf);
  uint8_t count = buf[0] > OZ_NONCE_MAX ? OZ_NONCE_MAX : buf[0];
  uint8_t head  = buf[1] % OZ_NONCE_MAX;
  uint8_t *r = buf + OZ_NONCE_HDR + head * OZ_NONCE_REC;
  memcpy(r, nonce, 16);
  memcpy(r + 16, pub, 32);
  head = (uint8_t)((head + 1) % OZ_NONCE_MAX);
  if (count < OZ_NONCE_MAX) count++;
  buf[0] = count;
  buf[1] = head;
  prefs.begin("blelock", false);
  prefs.putBytes("noncecache", buf, OZ_NONCE_SZ);
  prefs.end();
  free(buf);
  Serial.printf("[MEMBER] nonce burned — cache %u/%u\n", count, OZ_NONCE_MAX);
}

// ── M3: base64url decode (the OZINV1 QR body) ────────────────────────────────
//
// Accepts both alphabets ('-_' and '+/') and tolerates missing padding, because
// Dart's base64UrlEncode pads but a QR reader or a hand-pasted bench string may
// not. Rejects any other character outright — a lenient "skip junk" decoder
// would let two different QR strings decode to the same invite bytes.
static int ozB64UrlDecode(const char *in, size_t inLen, uint8_t *out,
                          size_t outCap) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == '=') break; // padding: nothing after it carries data
    const int v = val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outCap) return -1;
      out[n++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)n;
}

// ── boot self-test (known-answer vectors) ─────────────────────────────────────

static bool ozHexEq(const uint8_t *b, size_t n, const char *expectHex) {
  char h[129];
  ozHex(b, n, h);
  return strcmp(h, expectHex) == 0;
}

// Parse a hex string into bytes (len = strlen/2). No validation beyond length.
static void ozFromHex(const char *hex, uint8_t *out, size_t n) {
  auto nib = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; i < n; i++) out[i] = (nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]);
}

// ── ozkey-06 AES-256-GCM envelope ─────────────────────────────────────────────
//
// Frozen wire format (ozkey-06 §1, XF-42 §14.3; reference implementation is
// ftpos packages/ozkey_commissioner/lib/src/envelope.dart — that file is
// authoritative, this is the lock-side mirror):
//
//   envelope = ver(1B=0x02) ‖ counter(8B BE) ‖ nonce(12B) ‖ ciphertext ‖ tag(16B)
//   nonce    = prefix(4B random) ‖ counter(8B BE)
//   AAD      = ver(1B) ‖ counter(8B BE) ‖ utf8(device_id)
//   key      = HKDF-SHA256(ikm  = pairing_secret,
//                          salt = utf8(device_id) ‖ utf8(app_id),
//                          info = "ozkey/app->lock" | "ozkey/lock->app")
//
// The counter appears TWICE by design — once in the header (for AAD and
// anti-replay) and once as the nonce tail. Do not "optimise" one away; the wire
// format is frozen and byte-verified against the Dart side.
//
// Anti-replay is NOT done here. ozEnvOpen() returns the verified counter and the
// caller compares it against that bond's stored counter_floor (XF-47 §2.5).

#define OZ_ENV_VER 0x02
#define OZ_ENV_HDR 21 // 1 ver + 8 counter + 12 nonce
#define OZ_ENV_TAG 16
#define OZ_ENV_MIN (OZ_ENV_HDR + OZ_ENV_TAG)
#define OZ_ENV_AAD_MAX 96 // 1 + 8 + device_id (device ids are ≤ 64 chars)

static void ozPutU64BE(uint64_t v, uint8_t out[8]) {
  for (int i = 7; i >= 0; i--) {
    out[i] = (uint8_t)(v & 0xFF);
    v >>= 8;
  }
}

static uint64_t ozGetU64BE(const uint8_t in[8]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | in[i];
  return v;
}

// Per-direction AEAD key. appToLock=true derives the key the LOCK uses to OPEN
// app traffic; false derives the key the lock uses to SEAL its own uplink.
static bool ozEnvKey(const uint8_t *pairingSecret, size_t psLen,
                     const String &deviceId, const String &appId,
                     bool appToLock, uint8_t outKey[32]) {
  String salt = deviceId + appId;
  const char *info = appToLock ? "ozkey/app->lock" : "ozkey/lock->app";
  return ozHkdfSha256(pairingSecret, psLen, (const uint8_t *)salt.c_str(),
                      salt.length(), (const uint8_t *)info, strlen(info),
                      outKey, 32);
}

static size_t ozEnvAad(const String &deviceId, uint64_t counter,
                       uint8_t out[OZ_ENV_AAD_MAX]) {
  size_t dLen = deviceId.length();
  if (1 + 8 + dLen > OZ_ENV_AAD_MAX) return 0;
  out[0] = OZ_ENV_VER;
  ozPutU64BE(counter, out + 1);
  memcpy(out + 9, deviceId.c_str(), dLen);
  return 9 + dLen;
}

// Seal [pt] as a lock→app envelope. Returns envelope length, or -1 on failure.
// noncePrefix may be NULL for production (random); pass 4 bytes for test vectors.
static int ozEnvSeal(const uint8_t key[32], const String &deviceId,
                     uint64_t counter, const uint8_t *pt, size_t ptLen,
                     uint8_t *out, size_t outCap,
                     const uint8_t *noncePrefix) {
  if (outCap < OZ_ENV_HDR + ptLen + OZ_ENV_TAG) return -1;

  uint8_t nonce[12];
  if (noncePrefix) {
    memcpy(nonce, noncePrefix, 4);
  } else if (ozRng(nullptr, nonce, 4) != 0) {
    return -1;
  }
  ozPutU64BE(counter, nonce + 4);

  uint8_t aad[OZ_ENV_AAD_MAX];
  size_t aadLen = ozEnvAad(deviceId, counter, aad);
  if (!aadLen) return -1;

  out[0] = OZ_ENV_VER;
  ozPutU64BE(counter, out + 1);
  memcpy(out + 9, nonce, 12);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, ptLen, nonce, 12,
                                   aad, aadLen, pt, out + OZ_ENV_HDR,
                                   OZ_ENV_TAG, out + OZ_ENV_HDR + ptLen);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return -1;
  return (int)(OZ_ENV_HDR + ptLen + OZ_ENV_TAG);
}

// Open an app→lock envelope. Returns plaintext length, or -1 on any failure
// (malformed, wrong version, bad tag). Writes the verified counter to
// *counterOut for the caller's anti-replay check.
//
// A tag failure and a malformed envelope are deliberately indistinguishable to
// the caller — there is nothing useful to tell an attacker apart.
static int ozEnvOpen(const uint8_t key[32], const String &deviceId,
                     const uint8_t *env, size_t envLen, uint8_t *out,
                     size_t outCap, uint64_t *counterOut) {
  if (envLen < OZ_ENV_MIN || env[0] != OZ_ENV_VER) return -1;

  size_t ctLen = envLen - OZ_ENV_HDR - OZ_ENV_TAG;
  if (ctLen > outCap) return -1;

  uint64_t counter = ozGetU64BE(env + 1);
  const uint8_t *nonce = env + 9;

  // The nonce tail must equal the header counter. A well-formed sealer always
  // makes them equal; a mismatch is semantically ambiguous, so reject rather
  // than pick one. (GCM alone would not catch this — both are authenticated.)
  if (ozGetU64BE(nonce + 4) != counter) return -1;

  uint8_t aad[OZ_ENV_AAD_MAX];
  size_t aadLen = ozEnvAad(deviceId, counter, aad);
  if (!aadLen) return -1;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, ctLen, nonce, 12, aad, aadLen,
                                  env + OZ_ENV_HDR + ctLen, OZ_ENV_TAG,
                                  env + OZ_ENV_HDR, out);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return -1;

  if (counterOut) *counterOut = counter;
  return (int)ctLen;
}

// Returns true iff every vector matches. Logs each leg over serial.
static bool ozCryptoSelfTest() {
  bool ok = true;

  // 1) X25519 single scalar-mult — RFC 7748 §5.2.
  {
    uint8_t k[32], u[32], r[32];
    ozFromHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    ozFromHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    bool pass = ozX25519(k, u, r) &&
        ozHexEq(r, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    Serial.printf("[CRYPTO] selftest x25519-scalarmult %s\n", pass ? "PASS" : "FAIL");
    ok &= pass;
  }

  // 2) X25519 base-point (public-key derivation) — RFC 7748 §6.1 Alice.
  {
    uint8_t priv[32], pub[32];
    ozFromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", priv, 32);
    bool pass = ozX25519Base(priv, pub) &&
        ozHexEq(pub, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    Serial.printf("[CRYPTO] selftest x25519-basepoint %s\n", pass ? "PASS" : "FAIL");
    ok &= pass;
  }

  // 3) Invite MAC — frozen vector (ftpos member_invite_test.dart §7.1).
  {
    uint8_t s0[32];
    for (int i = 0; i < 32; i++) s0[i] = i + 1; // 01..20 hex
    String issuer; for (int i = 0; i < 64; i++) issuer += 'a';
    String nonce; for (int i = 0; i < 16; i++) nonce += "42";
    uint8_t mac[32];
    bool pass = ozInviteMac(s0, 32, "ozk-a4cf12879da7", issuer, "member",
                            "Ba Ngoai", nonce, 1789000000u, mac) &&
        ozHexEq(mac, 32, "e7780baea8feef5674c0ffecd1b83f35dfd9198db50cea6d0735c7a43d268aac");
    Serial.printf("[CRYPTO] selftest invite-mac %s\n", pass ? "PASS" : "FAIL");
    ok &= pass;
  }

  // ozkey-06 §5 frozen envelope vectors. Fixed inputs, shared by legs 4–8.
  uint8_t ps[32];
  for (int i = 0; i < 32; i++) ps[i] = i; // 000102…1F
  const String devId = "ozl-00112233445566778899aabbccddeeff";
  const String appId = "app_00112233445566778899aabb";
  uint8_t kA2L[32], kL2A[32];
  bool haveKeys = ozEnvKey(ps, 32, devId, appId, true, kA2L) &&
                  ozEnvKey(ps, 32, devId, appId, false, kL2A);

  // 4) HKDF key derivation, both directions — ozkey-06 §5 header.
  {
    bool pass = haveKeys &&
        ozHexEq(kA2L, 32, "919e05d8dab046bd5f2721ffe7fae0fa039a2f0399024964f2c8fdac9c9e5ac8") &&
        ozHexEq(kL2A, 32, "ad1daf444b95706ae9d97286498b79ba372c1f04c8abb17b12c937c58b62e4d0");
    Serial.printf("[CRYPTO] selftest env-hkdf-keys %s\n", pass ? "PASS" : "FAIL");
    ok &= pass;
  }

  // 5) app→lock open — ozkey-06 §5.1. Must yield SAMPLE_ADD_TEMP_PIN_FRAME.
  {
    uint8_t env[64], pt[64];
    ozFromHex("020000000000000001aabbccdd00000000000000019e129265ad2537f37a1485e72b"
              "d9f36cb304876c8f777b1a12925a7e0f57788c84df29dd08a68a74e44458", env, 64);
    uint64_t ctr = 0;
    int n = haveKeys ? ozEnvOpen(kA2L, devId, env, 64, pt, sizeof(pt), &ctr) : -1;
    bool pass = n == 27 && ctr == 1 &&
        ozHexEq(pt, 27, "55aa0006001415000010000e3438323931356955b9006b36ec7f0c");
    Serial.printf("[CRYPTO] selftest env-open-5.1 %s (len=%d ctr=%llu)\n",
                  pass ? "PASS" : "FAIL", n, (unsigned long long)ctr);
    ok &= pass;
  }

  // 6) Seal must reproduce §5.1 byte-exactly given the vector's nonce prefix.
  {
    uint8_t prefix[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t pt[27], env[64];
    ozFromHex("55aa0006001415000010000e3438323931356955b9006b36ec7f0c", pt, 27);
    int n = haveKeys ? ozEnvSeal(kA2L, devId, 1, pt, 27, env, sizeof(env), prefix) : -1;
    bool pass = n == 64 &&
        ozHexEq(env, 64, "020000000000000001aabbccdd00000000000000019e129265ad2537f37a1485e72b"
                         "d9f36cb304876c8f777b1a12925a7e0f57788c84df29dd08a68a74e44458");
    Serial.printf("[CRYPTO] selftest env-seal-5.1 %s (len=%d)\n", pass ? "PASS" : "FAIL", n);
    ok &= pass;
  }

  // 7) lock→app open — ozkey-06 §5.2, the door-event log JSON (72 B plaintext).
  {
    uint8_t env[109], pt[80];
    ozFromHex("020000000000000007112233440000000000000007f87fd3dcaadc2dedeba91e5784"
              "be64b327bad1f9da838c4bb88e8c9e45392590bb3528448e101b7bcd8f1579fd5ae0a"
              "ab97129e20c7d100353a61e0dd8864a3951ecf9bc5926601c4ffbe431aad97cf046b7"
              "1f01928b2418", env, 109);
    uint64_t ctr = 0;
    int n = haveKeys ? ozEnvOpen(kL2A, devId, env, 109, pt, sizeof(pt), &ctr) : -1;
    bool pass = n == 72 && ctr == 7 &&
        memcmp(pt, "{\"result\":\"granted\",\"detail\":\"REMOTE UNLOCK COMMAND\","
                   "\"ts\":1767225600000}", 72) == 0;
    Serial.printf("[CRYPTO] selftest env-open-5.2 %s (len=%d ctr=%llu)\n",
                  pass ? "PASS" : "FAIL", n, (unsigned long long)ctr);
    ok &= pass;
  }

  // 8) Tamper — flip one ciphertext byte, GCM tag must reject.
  {
    uint8_t env[64], pt[64];
    ozFromHex("020000000000000001aabbccdd00000000000000019e129265ad2537f37a1485e72b"
              "d9f36cb304876c8f777b1a12925a7e0f57788c84df29dd08a68a74e44458", env, 64);
    env[OZ_ENV_HDR] ^= 0x01;
    bool pass = !haveKeys ? false
                          : ozEnvOpen(kA2L, devId, env, 64, pt, sizeof(pt), nullptr) < 0;
    Serial.printf("[CRYPTO] selftest env-tamper-reject %s\n", pass ? "PASS" : "FAIL");
    ok &= pass;
  }

  // 9) M3 invite QR codec — decode the SAME frozen invite leg 3 MACs, from its
  //    real `OZINV1:` wire form. Leg 3 proves the MAC given already-parsed
  //    fields; this proves we can get those fields off a phone screen. Together
  //    they cover the whole enrolment authenticator without a bench phone.
  {
    const char *body =
        "eyJ2IjoxLCJkIjoib3prLWE0Y2YxMjg3OWRhNyIsImkiOiJhYWFhYWFhYWFhYWFhYWFh"
        "YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhIiwi"
        "ciI6Im1lbWJlciIsImwiOiJCYSBOZ29haSIsIm4iOiI0MjQyNDI0MjQyNDI0MjQyNDI0"
        "MjQyNDI0MjQyNDI0MiIsImUiOjE3ODkwMDAwMDAsIm0iOiJlNzc4MGJhZWE4ZmVlZjU2"
        "NzRjMGZmZWNkMWI4M2YzNWRmZDkxOThkYjUwY2VhNmQwNzM1YzdhNDNkMjY4YWFjIn0=";
    const char *want =
        "{\"v\":1,\"d\":\"ozk-a4cf12879da7\",\"i\":\"aaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"r\":\"member\",\"l\":\"Ba "
        "Ngoai\",\"n\":\"42424242424242424242424242424242\",\"e\":1789000000,"
        "\"m\":\"e7780baea8feef5674c0ffecd1b83f35dfd9198db50cea6d0735c7a43d268aac\"}";
    uint8_t dec[300];
    int n = ozB64UrlDecode(body, strlen(body), dec, sizeof(dec));
    bool pass = n == (int)strlen(want) && memcmp(dec, want, n) == 0;
    // …and a malformed body must be refused, not silently truncated.
    pass &= ozB64UrlDecode("abc$def", 7, dec, sizeof(dec)) < 0;
    // …and a body larger than the caller's buffer must fail rather than run off it.
    pass &= ozB64UrlDecode(body, strlen(body), dec, 8) < 0;
    Serial.printf("[CRYPTO] selftest invite-b64url %s (len=%d)\n",
                  pass ? "PASS" : "FAIL", n);
    ok &= pass;
  }

  Serial.printf("[CRYPTO] selftest %s\n", ok ? "PASS (interoperable)" : "FAIL — do not build on this");
  return ok;
}
