/* oz_crypto.h — crypto built in the core on top of oz_port.h primitives.
 *
 * OZKIE v2 suite (ozkey-48): EDHOC cipher suite 0 =
 *   AEAD AES-CCM-16-64-128 (COSE alg 10: 16-byte key, 13-byte nonce, 8-byte tag)
 *   hash SHA-256, ECDH X25519.
 */
#ifndef OZ_CRYPTO_H
#define OZ_CRYPTO_H

#include "oz_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot SHA-256 via the port. */
int  oz_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* HMAC-SHA256 (RFC 2104). */
int  oz_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* HKDF-SHA256 (RFC 5869). salt may be NULL/0 (= 32 zero bytes).
 * okm_len ≤ 255*32. */
int  oz_hkdf_extract(const uint8_t *salt, size_t salt_len,
                     const uint8_t *ikm, size_t ikm_len, uint8_t prk[32]);
int  oz_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len);

/* AES-128-CCM (RFC 3610 / NIST SP 800-38C), generic parameters:
 *   nonce_len 7..13 (L = 15 - nonce_len), tag_len 4..16 even.
 * seal: out = ciphertext ‖ tag, out_cap ≥ pt_len + tag_len.
 * open: in = ciphertext ‖ tag; on OZ_E_AUTH out is zeroed.
 * out may alias the plaintext/ciphertext input. */
int  oz_aes128_ccm_seal(const uint8_t key[16],
                        const uint8_t *nonce, size_t nonce_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t pt_len,
                        size_t tag_len, uint8_t *out, size_t out_cap);
int  oz_aes128_ccm_open(const uint8_t key[16],
                        const uint8_t *nonce, size_t nonce_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t in_len,
                        size_t tag_len, uint8_t *out, size_t out_cap);

/* Suite-0 shorthands: 13-byte nonce, 8-byte tag. */
#define OZ_CCM16_64_NONCE 13
#define OZ_CCM16_64_TAG    8

/* Constant-time compare (0 = equal) and a wipe the compiler won't drop. */
int  oz_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
void oz_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* OZ_CRYPTO_H */
