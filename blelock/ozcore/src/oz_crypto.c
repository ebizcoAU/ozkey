/* HMAC / HKDF / AES-128-CCM / helpers, built on the oz_port.h primitives. */
#include <string.h>
#include "oz_crypto.h"

int oz_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++)
        d |= a[i] ^ b[i];
    return d != 0;
}

void oz_wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--)
        *v++ = 0;
}

int oz_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    oz_sha256_ctx c;
    int rc = oz_port_sha256_init(&c);
    if (!rc) rc = oz_port_sha256_update(&c, data, len);
    if (!rc) rc = oz_port_sha256_final(&c, out);
    return rc;
}

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────────── */

typedef struct { oz_sha256_ctx inner, outer; } hmac_ctx;

static int hmac_init(hmac_ctx *h, const uint8_t *key, size_t key_len)
{
    uint8_t k[64] = { 0 }, pad[64];
    int rc = 0;
    if (key_len > 64)
        rc = oz_sha256(key, key_len, k);
    else
        memcpy(k, key, key_len);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    if (!rc) rc = oz_port_sha256_init(&h->inner);
    if (!rc) rc = oz_port_sha256_update(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    if (!rc) rc = oz_port_sha256_init(&h->outer);
    if (!rc) rc = oz_port_sha256_update(&h->outer, pad, 64);
    oz_wipe(k, sizeof k);
    oz_wipe(pad, sizeof pad);
    return rc;
}

static int hmac_final(hmac_ctx *h, uint8_t out[32])
{
    uint8_t ih[32];
    int rc = oz_port_sha256_final(&h->inner, ih);
    if (!rc) rc = oz_port_sha256_update(&h->outer, ih, 32);
    if (!rc) rc = oz_port_sha256_final(&h->outer, out);
    oz_wipe(ih, sizeof ih);
    oz_wipe(h, sizeof *h);
    return rc;
}

int oz_hmac_sha256(const uint8_t *key, size_t key_len,
                   const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    hmac_ctx h;
    int rc = hmac_init(&h, key, key_len);
    if (!rc) rc = oz_port_sha256_update(&h.inner, msg, msg_len);
    if (!rc) rc = hmac_final(&h, out);
    return rc;
}

/* ── HKDF-SHA256 (RFC 5869) ──────────────────────────────────────────────── */

int oz_hkdf_extract(const uint8_t *salt, size_t salt_len,
                    const uint8_t *ikm, size_t ikm_len, uint8_t prk[32])
{
    static const uint8_t zeros[32];
    if (!salt || !salt_len) {
        salt = zeros;
        salt_len = sizeof zeros;
    }
    return oz_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

int oz_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                   uint8_t *okm, size_t okm_len)
{
    uint8_t t[32];
    size_t t_len = 0;
    if (okm_len > 255 * 32)
        return OZ_E_ARG;
    for (uint8_t ctr = 1; okm_len; ctr++) {
        hmac_ctx h;
        int rc = hmac_init(&h, prk, 32);
        if (!rc) rc = oz_port_sha256_update(&h.inner, t, t_len);
        if (!rc && info_len) rc = oz_port_sha256_update(&h.inner, info, info_len);
        if (!rc) rc = oz_port_sha256_update(&h.inner, &ctr, 1);
        if (!rc) rc = hmac_final(&h, t);
        if (rc) {
            oz_wipe(t, sizeof t);
            return rc;
        }
        t_len = 32;
        size_t n = okm_len < 32 ? okm_len : 32;
        memcpy(okm, t, n);
        okm += n;
        okm_len -= n;
    }
    oz_wipe(t, sizeof t);
    return OZ_OK;
}

/* ── AES-128-CCM (RFC 3610) ──────────────────────────────────────────────── */

static int ccm_check(size_t nonce_len, size_t tag_len, size_t pt_len)
{
    if (nonce_len < 7 || nonce_len > 13)
        return OZ_E_ARG;
    if (tag_len < 4 || tag_len > 16 || (tag_len & 1))
        return OZ_E_ARG;
    size_t L = 15 - nonce_len;
    if (L < sizeof(size_t) && (pt_len >> (8 * L)))
        return OZ_E_ARG;                     /* length doesn't fit in L bytes */
    return OZ_OK;
}

/* CBC-MAC over B0 ‖ encoded(aad) ‖ pt, giving the unencrypted tag T. */
static int ccm_mac(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *pt, size_t pt_len, size_t tag_len, uint8_t x[16])
{
    size_t L = 15 - nonce_len;
    uint8_t b[16];
    int rc;

    b[0] = (uint8_t)((aad_len ? 0x40 : 0) | (((tag_len - 2) / 2) << 3) | (L - 1));
    memcpy(b + 1, nonce, nonce_len);
    for (size_t i = 0, n = pt_len; i < L; i++, n >>= 8)
        b[15 - i] = (uint8_t)n;
    if ((rc = oz_port_aes128_encrypt_block(key, b, x)))
        return rc;

    if (aad_len) {
        /* Length prefix, then aad, packed into 16-byte blocks. */
        uint8_t hdr[10];
        size_t hl;
        if (aad_len < 0xFF00) {
            hdr[0] = (uint8_t)(aad_len >> 8);
            hdr[1] = (uint8_t)aad_len;
            hl = 2;
        } else if ((uint64_t)aad_len <= 0xFFFFFFFFu) {
            hdr[0] = 0xFF; hdr[1] = 0xFE;
            for (int i = 0; i < 4; i++) hdr[2 + i] = (uint8_t)((uint64_t)aad_len >> (24 - 8 * i));
            hl = 6;
        } else {
            return OZ_E_ARG;
        }
        size_t used = hl, ai = 0;
        memset(b, 0, 16);
        memcpy(b, hdr, hl);
        for (;;) {
            while (used < 16 && ai < aad_len) b[used++] = aad[ai++];
            for (int i = 0; i < 16; i++) x[i] ^= b[i];   /* last block zero-padded */
            if ((rc = oz_port_aes128_encrypt_block(key, x, x)))
                return rc;
            if (ai >= aad_len) break;
            used = 0;
            memset(b, 0, 16);
        }
    }

    for (size_t off = 0; off < pt_len; off += 16) {
        size_t n = pt_len - off < 16 ? pt_len - off : 16;
        for (size_t i = 0; i < n; i++) x[i] ^= pt[off + i];
        if ((rc = oz_port_aes128_encrypt_block(key, x, x)))
            return rc;
    }
    return OZ_OK;
}

/* CTR keystream: A_i = flags(L-1) ‖ nonce ‖ i. Block 0 encrypts the tag. */
static int ccm_ctr(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
                   const uint8_t *in, uint8_t *out, size_t len, uint8_t s0[16])
{
    size_t L = 15 - nonce_len;
    uint8_t a[16], s[16];
    int rc;
    a[0] = (uint8_t)(L - 1);
    memcpy(a + 1, nonce, nonce_len);
    memset(a + 1 + nonce_len, 0, L);
    if ((rc = oz_port_aes128_encrypt_block(key, a, s0)))
        return rc;
    for (size_t off = 0, ctr = 1; off < len; off += 16, ctr++) {
        for (size_t i = 0, n = ctr; i < L; i++, n >>= 8)
            a[15 - i] = (uint8_t)n;
        if ((rc = oz_port_aes128_encrypt_block(key, a, s)))
            return rc;
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; i++) out[off + i] = in[off + i] ^ s[i];
    }
    oz_wipe(s, sizeof s);
    return OZ_OK;
}

int oz_aes128_ccm_seal(const uint8_t key[16],
                       const uint8_t *nonce, size_t nonce_len,
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *pt, size_t pt_len,
                       size_t tag_len, uint8_t *out, size_t out_cap)
{
    uint8_t x[16], s0[16];
    int rc = ccm_check(nonce_len, tag_len, pt_len);
    if (rc) return rc;
    if (out_cap < pt_len + tag_len) return OZ_E_NO_SPACE;
    /* MAC first: out may alias pt. */
    if ((rc = ccm_mac(key, nonce, nonce_len, aad, aad_len, pt, pt_len, tag_len, x)))
        goto done;
    if ((rc = ccm_ctr(key, nonce, nonce_len, pt, out, pt_len, s0)))
        goto done;
    for (size_t i = 0; i < tag_len; i++) out[pt_len + i] = x[i] ^ s0[i];
done:
    oz_wipe(x, sizeof x);
    oz_wipe(s0, sizeof s0);
    return rc;
}

int oz_aes128_ccm_open(const uint8_t key[16],
                       const uint8_t *nonce, size_t nonce_len,
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *in, size_t in_len,
                       size_t tag_len, uint8_t *out, size_t out_cap)
{
    uint8_t x[16], s0[16], tag[16];
    if (in_len < tag_len) return OZ_E_ARG;
    size_t ct_len = in_len - tag_len;
    int rc = ccm_check(nonce_len, tag_len, ct_len);
    if (rc) return rc;
    if (out_cap < ct_len) return OZ_E_NO_SPACE;
    memcpy(tag, in + ct_len, tag_len);       /* before out may overwrite in */
    if ((rc = ccm_ctr(key, nonce, nonce_len, in, out, ct_len, s0)))
        goto done;
    if ((rc = ccm_mac(key, nonce, nonce_len, aad, aad_len, out, ct_len, tag_len, x)))
        goto done;
    for (size_t i = 0; i < tag_len; i++) x[i] ^= s0[i];
    if (oz_ct_equal(x, tag, tag_len)) {
        oz_wipe(out, ct_len);
        rc = OZ_E_AUTH;
    }
done:
    if (rc && rc != OZ_E_AUTH) oz_wipe(out, ct_len);
    oz_wipe(x, sizeof x);
    oz_wipe(s0, sizeof s0);
    return rc;
}
