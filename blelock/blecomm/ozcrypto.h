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

  Serial.printf("[CRYPTO] selftest %s\n", ok ? "PASS (interoperable)" : "FAIL — do not build on this");
  return ok;
}
