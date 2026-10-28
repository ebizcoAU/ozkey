/* port/ak1037 — Anyka AK1037 (RISC-V, RT-Thread 3.1) implementation of oz_port.h.
 *
 * Built with the SDK's own include paths (ozkey-49 P3). Status per primitive:
 *   SHA-256  software. The SDK's hal_hash.h enables only SM3; SHA-256 in the
 *            hash engine is unconfirmed, so it stays in software until the
 *            bench proves it (P4).
 *   AES-128  SKE engine, ECB, one block per call. A known-answer
 *            self-test on first use falls back to software on mismatch.
 *   X25519   software (Monocypher); no curve API in the SDK.
 *   Random   TRNG, read one word per DRDY and conditioned through SHA-256
 *            (the SDK's burst read takes several words per ready flag).
 *   kv       NOT IMPLEMENTED: needs the flash partition plan (P4).
 *
 * Every SKE use runs with interrupts off: the driver keeps its state in
 * file-scope statics and is not re-entrant.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "oz_port.h"
#include "oz_sw.h"
#include "oz_crypto.h"
#include "monocypher.h"

#include "rthw.h"
#include "hal_ske.h"
#include "hal_trng.h"
#include "hal_sys.h"

/* ── SHA-256 ─────────────────────────────────────────────────────────────── */

int oz_port_sha256_init(oz_sha256_ctx *ctx) { oz_sw_sha256_init(ctx); return OZ_OK; }
int oz_port_sha256_update(oz_sha256_ctx *ctx, const uint8_t *d, size_t n)
{
    oz_sw_sha256_update(ctx, d, n);
    return OZ_OK;
}
int oz_port_sha256_final(oz_sha256_ctx *ctx, uint8_t out[32]) { oz_sw_sha256_final(ctx, out); return OZ_OK; }

/* ── AES-128 ─────────────────────────────────────────────────────────────── */

static int ske_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    static const uint8_t zero_key[16];
    uint8_t k[16], b[16];
    uint32_t rc;

    memcpy(k, key, 16);             /* the driver takes non-const pointers */
    memcpy(b, in, 16);
    rt_base_t level = rt_hw_interrupt_disable();
    rc = hal_ske_init(SKE_ALG_AES_128, SKE_MODE_ECB, SKE_CRYPTO_ENCRYPT, k, 0, NULL);
    if (rc == SKE_SUCCESS)
        rc = hal_ske_update_blocks(b, b, 16);
    /* hal_ske_final() leaves the key in the key registers; overwrite it. */
    (void)hal_ske_init(SKE_ALG_AES_128, SKE_MODE_ECB, SKE_CRYPTO_ENCRYPT,
                       (uint8_t *)zero_key, 0, NULL);
    (void)hal_ske_final();
    rt_hw_interrupt_enable(level);

    if (rc == SKE_SUCCESS) memcpy(out, b, 16);
    oz_wipe(k, sizeof k);
    oz_wipe(b, sizeof b);
    return rc == SKE_SUCCESS ? OZ_OK : OZ_E_HW;
}

/* 0 = untested, 1 = engine passed FIPS-197 C.1, -1 = use software. */
static int ske_state;

static void ske_selftest(void)
{
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    static const uint8_t pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const uint8_t ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    uint8_t out[16];

    ske_state = (ske_block(key, pt, out) == OZ_OK && oz_ct_equal(out, ct, 16)) ? 1 : -1;
    if (ske_state < 0)
        oz_port_log("aes: SKE self-test failed, using software AES");
}

int oz_port_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (ske_state == 0) ske_selftest();
    if (ske_state > 0) return ske_block(key, in, out);
    oz_sw_aes128_encrypt_block(key, in, out);
    return OZ_OK;
}

/* ── X25519 ──────────────────────────────────────────────────────────────── */

int oz_port_x25519_public_key(uint8_t pub[32], const uint8_t priv[32])
{
    crypto_x25519_public_key(pub, priv);
    return OZ_OK;
}

int oz_port_x25519(uint8_t shared[32], const uint8_t priv[32], const uint8_t peer_pub[32])
{
    static const uint8_t zero[32];
    crypto_x25519(shared, priv, peer_pub);
    /* RFC 7748 §6.1: reject the all-zero output from a low-order peer key. */
    return crypto_verify32(shared, zero) == 0 ? OZ_E_ARG : OZ_OK;
}

/* ── Random ──────────────────────────────────────────────────────────────── */

#define TRNG_WORDS_PER_BLOCK 16     /* 64 raw bytes in, 32 out */

static int trng_ready;

/* One word per call: hal_trng_get_randbuff_bytes() waits for DRDY, then reads.
 * A non-zero return is a timeout. Two equal consecutive words fail the
 * stuck-output check (the continuous test of FIPS 140-2 §4.9.2). */
static int trng_word(uint32_t *w, uint32_t *prev, int *have_prev)
{
    if (hal_trng_get_randbuff_bytes((uint8_t *)w, 4) != 0) return OZ_E_HW;
    if (*have_prev && *w == *prev) return OZ_E_HW;
    *prev = *w;
    *have_prev = 1;
    return OZ_OK;
}

int oz_port_random(uint8_t *out, size_t len)
{
    static uint32_t counter;
    uint32_t raw[TRNG_WORDS_PER_BLOCK], prev = 0;
    int have_prev = 0, rc = OZ_OK;
    uint8_t block[32];

    if (!trng_ready) { hal_trng_init(); trng_ready = 1; }

    while (len) {
        for (int i = 0; i < TRNG_WORDS_PER_BLOCK; i++)
            if ((rc = trng_word(&raw[i], &prev, &have_prev)) != OZ_OK) goto done;
        /* Condition: SHA-256(counter || 64 raw bytes). */
        oz_sha256_ctx h;
        counter++;
        oz_port_sha256_init(&h);
        oz_port_sha256_update(&h, (const uint8_t *)&counter, sizeof counter);
        oz_port_sha256_update(&h, (const uint8_t *)raw, sizeof raw);
        oz_port_sha256_final(&h, block);
        oz_wipe(&h, sizeof h);
        size_t n = len < sizeof block ? len : sizeof block;
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
done:
    oz_wipe(raw, sizeof raw);
    oz_wipe(block, sizeof block);
    if (rc != OZ_OK) oz_port_log("random: TRNG failed");
    return rc;
}

/* ── kv (P4) ─────────────────────────────────────────────────────────────── */

int oz_port_kv_get(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)key; (void)buf; (void)cap; (void)len;
    return OZ_E_HW;
}

int oz_port_kv_set(const char *key, const uint8_t *buf, size_t len)
{
    (void)key; (void)buf; (void)len;
    return OZ_E_HW;
}

int oz_port_kv_erase(const char *key)
{
    (void)key;
    return OZ_E_HW;
}

/* ── Time ────────────────────────────────────────────────────────────────── */

/* get_ms_tick_func() is 32-bit and wraps every 49.7 days. Extending it is
 * correct as long as this is called at least once per wrap; the core calls
 * it on every message and timer tick. */
uint64_t oz_port_monotonic_ms(void)
{
    static uint32_t last;
    static uint64_t high;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)get_ms_tick_func();
    if (now < last) high += (uint64_t)1 << 32;
    last = now;
    uint64_t t = high | now;
    rt_hw_interrupt_enable(level);
    return t;
}

/* ── Log ─────────────────────────────────────────────────────────────────── */

void oz_port_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("[oz] %s\r\n", line);
}
