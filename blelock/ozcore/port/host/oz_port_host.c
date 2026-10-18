/* port/host — macOS / Linux implementation of oz_port.h.
 *
 * For unit tests, fuzzing and the host "lock" used in interop tests. Crypto
 * forwards to the portable software code; kv is an in-memory table (tests
 * call oz_host_kv_reset() between cases).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <stdlib.h>                 /* arc4random_buf */
#else
#include <sys/random.h>             /* getrandom */
#endif

#include "oz_port.h"
#include "oz_sw.h"
#include "oz_port_host.h"
#include "monocypher.h"

int oz_port_sha256_init(oz_sha256_ctx *ctx) { oz_sw_sha256_init(ctx); return OZ_OK; }
int oz_port_sha256_update(oz_sha256_ctx *ctx, const uint8_t *d, size_t n)
{
    oz_sw_sha256_update(ctx, d, n);
    return OZ_OK;
}
int oz_port_sha256_final(oz_sha256_ctx *ctx, uint8_t out[32]) { oz_sw_sha256_final(ctx, out); return OZ_OK; }

int oz_port_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    oz_sw_aes128_encrypt_block(key, in, out);
    return OZ_OK;
}

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

int oz_port_random(uint8_t *out, size_t len)
{
#if defined(__APPLE__)
    arc4random_buf(out, len);
    return OZ_OK;
#else
    while (len) {
        ssize_t n = getrandom(out, len, 0);
        if (n <= 0) return OZ_E_HW;
        out += n;
        len -= (size_t)n;
    }
    return OZ_OK;
#endif
}

/* ── In-memory kv ────────────────────────────────────────────────────────── */

#define KV_SLOTS     32
#define KV_KEY_MAX   32
#define KV_VALUE_MAX 1024

static struct {
    char    key[KV_KEY_MAX];
    uint8_t val[KV_VALUE_MAX];
    size_t  len;
    int     used;
} kv[KV_SLOTS];

void oz_host_kv_reset(void) { memset(kv, 0, sizeof kv); }

static int kv_find(const char *key)
{
    for (int i = 0; i < KV_SLOTS; i++)
        if (kv[i].used && !strcmp(kv[i].key, key)) return i;
    return -1;
}

int oz_port_kv_get(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    int i = kv_find(key);
    if (i < 0) return OZ_E_NOT_FOUND;
    if (kv[i].len > cap) return OZ_E_NO_SPACE;
    memcpy(buf, kv[i].val, kv[i].len);
    *len = kv[i].len;
    return OZ_OK;
}

int oz_port_kv_set(const char *key, const uint8_t *buf, size_t len)
{
    if (strlen(key) >= KV_KEY_MAX || len > KV_VALUE_MAX) return OZ_E_ARG;
    int i = kv_find(key);
    for (int j = 0; i < 0 && j < KV_SLOTS; j++)
        if (!kv[j].used) i = j;
    if (i < 0) return OZ_E_NO_SPACE;
    strcpy(kv[i].key, key);
    memcpy(kv[i].val, buf, len);
    kv[i].len = len;
    kv[i].used = 1;
    return OZ_OK;
}

int oz_port_kv_erase(const char *key)
{
    int i = kv_find(key);
    if (i < 0) return OZ_E_NOT_FOUND;
    memset(&kv[i], 0, sizeof kv[i]);
    return OZ_OK;
}

uint64_t oz_port_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void oz_port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[oz] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
