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
static bool ozInviteMac(const uint8_t *s0, size_t s0Len, const String &deviceId,
                        const String &issuerAppId, const String &roleWire,
                        const String &label, const String &nonceHex,
                        uint32_t expires, uint8_t out[32]) {
  String salt = deviceId + issuerAppId;
  uint8_t macKey[32];
  if (!ozHkdfSha256(s0, s0Len, (const uint8_t *)salt.c_str(), salt.length(),
                    (const uint8_t *)"ozkey/invite-v1", 15, macKey, 32))
    return false;
  String canonical = "1|" + deviceId + "|" + issuerAppId + "|" + roleWire +
                     "|" + label + "|" + nonceHex + "|" + String(expires);
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

  Serial.printf("[CRYPTO] selftest %s\n", ok ? "PASS (interoperable)" : "FAIL — do not build on this");
  return ok;
}
