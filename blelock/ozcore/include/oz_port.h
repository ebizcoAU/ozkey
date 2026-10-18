/* oz_port.h — the only interface ozcore needs from a platform (ozkey-49 §2).
 *
 * ozcore is portable C11: no OS calls, no vendor headers, no malloc. Each
 * platform (port/host, port/ak1037, port/esp32) implements every function
 * below. Portable software fallbacks for the crypto primitives live in
 * oz_sw.h, so a port without a hardware block can simply forward to them.
 *
 * Return convention: 0 = success, negative = OZ_E_* error.
 */
#ifndef OZ_PORT_H
#define OZ_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OZ_OK          = 0,
    OZ_E_ARG       = -1,   /* bad argument / length */
    OZ_E_AUTH      = -2,   /* AEAD tag or MAC mismatch */
    OZ_E_NOT_FOUND = -3,   /* kv key absent */
    OZ_E_NO_SPACE  = -4,   /* kv full, or output buffer too small */
    OZ_E_HW        = -5,   /* platform / hardware failure */
};

/* ── SHA-256 (AK1037: hal_hash) ────────────────────────────────────────────
 * The context is opaque storage; a port may keep its own state in it as long
 * as it fits. The software implementation needs 108 bytes. */
#define OZ_SHA256_CTX_SIZE 128
typedef struct { _Alignas(8) uint8_t opaque[OZ_SHA256_CTX_SIZE]; } oz_sha256_ctx;

int  oz_port_sha256_init(oz_sha256_ctx *ctx);
int  oz_port_sha256_update(oz_sha256_ctx *ctx, const uint8_t *data, size_t len);
int  oz_port_sha256_final(oz_sha256_ctx *ctx, uint8_t out[32]);

/* ── AES-128, forward cipher only (AK1037: hal_ske ECB) ────────────────────
 * CCM uses only the encrypt direction, for both CTR and CBC-MAC. */
int  oz_port_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16],
                                  uint8_t out[16]);

/* ── X25519 (AK1037: software until Anyka exposes the PKE curve API) ─────── */
int  oz_port_x25519_public_key(uint8_t pub[32], const uint8_t priv[32]);
int  oz_port_x25519(uint8_t shared[32], const uint8_t priv[32],
                    const uint8_t peer_pub[32]);

/* ── Random (AK1037: hal_trng). Must be cryptographic. ──────────────────── */
int  oz_port_random(uint8_t *out, size_t len);

/* ── Key-value storage. A set must be atomic: after power loss the key holds
 *    either the old value or the new one, never a mix. ─────────────────── */
int  oz_port_kv_get(const char *key, uint8_t *buf, size_t cap, size_t *len);
int  oz_port_kv_set(const char *key, const uint8_t *buf, size_t len);
int  oz_port_kv_erase(const char *key);

/* ── Time ──────────────────────────────────────────────────────────────────
 * Monotonic ms since boot. UTC is whatever the core was last told
 * (CONTRACT.md "monotonic-forward only" rule is enforced in the core). */
uint64_t oz_port_monotonic_ms(void);

/* ── Log ─────────────────────────────────────────────────────────────────── */
void oz_port_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}
#endif
#endif /* OZ_PORT_H */
