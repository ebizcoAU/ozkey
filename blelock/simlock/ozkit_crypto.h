#pragma once
/**
 * ozkit_crypto.h — SIMLOCK device identity + sealed MQTT telemetry.
 *
 * Mirrors blelock/common/ozcrypto.h's X25519/HKDF/AES-GCM envelope scheme
 * byte-for-byte (same mbedtls calls, same field layout) rather than
 * reinventing one — this is the primitive doorlock's `provision_key`/sealed-
 * envelope model already proved on real hardware. See
 * ozkit/docs/simlock-dp-reconciliation.md and the session plan for why:
 * SIMLOCK's MQTT credentials were a single shared plaintext string baked
 * into every unit's binary (trivially dumped with `esptool read_flash` +
 * `strings`) — this replaces that with per-board X25519 identity, issued in
 * development mode by NEXUS (`~/Documents/Dev/nexus/api/src/routes/dev.js`,
 * `POST /api/v1/dev/lock-keys`) via `ozkit/server` acting as courier, the
 * same role doorlock's app plays.
 *
 * Not #including ozcrypto.h directly for the same reason tuya_frame.h
 * doesn't #include ozdoorlock_core.h — it needs per-board macros this bare
 * test-master doesn't have. Kept as its own small header so a mismatch
 * between this and the doorlock scheme would show up as a diff, not a
 * silent divergence.
 */

#include <Arduino.h>
#include <string.h>
#include "esp_random.h"
#define MBEDTLS_ALLOW_PRIVATE_ACCESS // ecp_point X/Y/Z are private in mbedTLS 3.x
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

// Same envelope shape as ozcrypto.h's OZ_ENV_* — deliberately, so the two
// sides (this firmware, ozkit/server's Node port of the same scheme) agree
// on wire format without sharing a library.
#define OZKIT_ENV_VER 0x02
#define OZKIT_ENV_HDR 21 // 1 ver + 8 counter + 12 nonce
#define OZKIT_ENV_TAG 16
#define OZKIT_ENV_MIN (OZKIT_ENV_HDR + OZKIT_ENV_TAG)
#define OZKIT_ENV_AAD_MAX 96 // 1 + 8 + board_id (board ids are short, e.g. "N8-0DAFBC")

static int ozkitRng(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static void ozkitClamp(uint8_t k[32]) {
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
}

// RFC 7748 X25519: out = scalar . u (all 32-byte little-endian).
static bool ozkitX25519(const uint8_t scalar[32], const uint8_t u[32], uint8_t out[32]) {
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
    ozkitClamp(s);
    if (mbedtls_mpi_read_binary_le(&d, s, 32) != 0) break;
    if (mbedtls_mpi_read_binary_le(&Q.MBEDTLS_PRIVATE(X), u, 32) != 0) break;
    if (mbedtls_mpi_lset(&Q.MBEDTLS_PRIVATE(Z), 1) != 0) break;
    if (mbedtls_ecp_mul(&grp, &R, &d, &Q, ozkitRng, nullptr) != 0) break;
    if (mbedtls_mpi_write_binary_le(&R.MBEDTLS_PRIVATE(X), out, 32) != 0) break;
    ok = true;
  } while (false);
  mbedtls_ecp_point_free(&R);
  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static bool ozkitX25519Base(const uint8_t scalar[32], uint8_t out[32]) {
  uint8_t base[32] = {9};
  return ozkitX25519(scalar, base, out);
}

static bool ozkitHkdfSha256(const uint8_t *ikm, size_t ikmLen, const uint8_t *salt,
                             size_t saltLen, const uint8_t *info, size_t infoLen,
                             uint8_t *out, size_t outLen) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return md && mbedtls_hkdf(md, salt, saltLen, ikm, ikmLen, info, infoLen, out, outLen) == 0;
}

static void ozkitPutU64BE(uint64_t v, uint8_t out[8]) {
  for (int i = 7; i >= 0; i--) { out[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}
static uint64_t ozkitGetU64BE(const uint8_t in[8]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | in[i];
  return v;
}

// Direction-scoped key derivation — mirrors ozcrypto.h's ozEnvKey(): salt =
// deviceId+context, info = direction string. Here "context" is fixed to
// "ozkit" (no per-app_id concept for a test fixture) and direction is
// board->server (the only direction sealed today; see the header comment).
static bool ozkitEnvKey(const uint8_t sharedSecret[32], const String &boardId,
                         const char *direction, uint8_t outKey[32]) {
  String salt = boardId + "ozkit";
  return ozkitHkdfSha256(sharedSecret, 32, (const uint8_t *)salt.c_str(), salt.length(),
                          (const uint8_t *)direction, strlen(direction), outKey, 32);
}

static size_t ozkitEnvAad(const String &boardId, uint64_t counter, uint8_t out[OZKIT_ENV_AAD_MAX]) {
  size_t idLen = boardId.length();
  if (1 + 8 + idLen > OZKIT_ENV_AAD_MAX) return 0;
  out[0] = OZKIT_ENV_VER;
  ozkitPutU64BE(counter, out + 1);
  memcpy(out + 9, boardId.c_str(), idLen);
  return 9 + idLen;
}

// Seal [pt] as a board->server envelope. Returns envelope length, or -1 on
// failure. `counter` must be monotonic and NVS-persisted by the caller —
// this function does not track it, so nonce reuse is the caller's to avoid
// (mirrors the lock's own counter_floor discipline).
static int ozkitEnvSeal(const uint8_t key[32], const String &boardId, uint64_t counter,
                         const uint8_t *pt, size_t ptLen, uint8_t *out, size_t outCap) {
  if (outCap < OZKIT_ENV_HDR + ptLen + OZKIT_ENV_TAG) return -1;

  uint8_t nonce[12];
  if (ozkitRng(nullptr, nonce, 4) != 0) return -1;
  ozkitPutU64BE(counter, nonce + 4);

  uint8_t aad[OZKIT_ENV_AAD_MAX];
  size_t aadLen = ozkitEnvAad(boardId, counter, aad);
  if (!aadLen) return -1;

  out[0] = OZKIT_ENV_VER;
  ozkitPutU64BE(counter, out + 1);
  memcpy(out + 9, nonce, 12);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, ptLen, nonce, 12,
                                    aad, aadLen, pt, out + OZKIT_ENV_HDR,
                                    OZKIT_ENV_TAG, out + OZKIT_ENV_HDR + ptLen);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return -1;
  return (int)(OZKIT_ENV_HDR + ptLen + OZKIT_ENV_TAG);
}

// Open a server->board envelope (for a future sealed command channel — not
// used yet, kept symmetric with ozcrypto.h's ozEnvOpen() rather than only
// building the direction needed today).
static int ozkitEnvOpen(const uint8_t key[32], const String &boardId, const uint8_t *env,
                         size_t envLen, uint8_t *out, size_t outCap, uint64_t *counterOut) {
  if (envLen < OZKIT_ENV_MIN || env[0] != OZKIT_ENV_VER) return -1;
  size_t ctLen = envLen - OZKIT_ENV_HDR - OZKIT_ENV_TAG;
  if (ctLen > outCap) return -1;

  uint64_t counter = ozkitGetU64BE(env + 1);
  const uint8_t *nonce = env + 9;
  if (ozkitGetU64BE(nonce + 4) != counter) return -1;

  uint8_t aad[OZKIT_ENV_AAD_MAX];
  size_t aadLen = ozkitEnvAad(boardId, counter, aad);
  if (!aadLen) return -1;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, ctLen, nonce, 12, aad, aadLen,
                                   env + OZKIT_ENV_HDR + ctLen, OZKIT_ENV_TAG,
                                   env + OZKIT_ENV_HDR, out);
  }
  mbedtls_gcm_free(&gcm);
  if (rc != 0) return -1;
  if (counterOut) *counterOut = counter;
  return (int)ctLen;
}
