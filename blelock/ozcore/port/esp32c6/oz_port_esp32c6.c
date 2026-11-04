/* oz_port_esp32c6 — ozcore's platform layer on ESP32-C6 (ozkey-55 F5).
 *
 * The second ozcore port after AK1037, on the bench's existing C6 lock boards
 * (dlch584m_19/147), so OZKIE v2 runs lock → bridge → server → dashboard
 * before the AK1037 kit. Same engine, same tests; only this layer differs.
 *
 *   SHA-256   software (oz_sw), as on the host
 *   AES-128   mbedTLS, which uses the C6's AES accelerator
 *   X25519    software (Monocypher)
 *   random    esp_fill_random(): a true RNG while the radio (BLE) is on.
 *             oz_esp32c6_init() must run after BLE is up, or bootloader_random
 *             must be enabled, before the lock makes keys.
 *   kv        NVS, namespace "ozkie". NVS writes an entry fully before it
 *             retires the old one, so a power cut leaves old or new, never a
 *             mix: the atomicity oz_port.h asks for. Keys are ≤ 15 chars (NVS).
 *   time      esp_timer, µs since boot
 *   log       printf with a "[OZ] " prefix and a newline
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/aes.h"

#include "oz_port.h"
#include "oz_sw.h"
#include "oz_port_esp32c6.h"
#include "monocypher.h"

#define NVS_NS     "ozkie"
#define NVS_KEYMAX 15

static nvs_handle_t nvs;
static int nvs_ready;

int oz_esp32c6_init(void)
{
    if (nvs_ready)
        return OZ_OK;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition this NVS version can't use. Only then is it erased. */
        oz_port_log("kv: NVS unusable (%d), erasing it", (int)e);
        if (nvs_flash_erase() != ESP_OK)
            return OZ_E_HW;
        e = nvs_flash_init();
    }
    if (e != ESP_OK || nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK)
        return OZ_E_HW;
    nvs_ready = 1;
    return OZ_OK;
}

int oz_esp32c6_kv_wipe(void)
{
    if (!nvs_ready && oz_esp32c6_init() != OZ_OK)
        return OZ_E_HW;
    if (nvs_erase_all(nvs) != ESP_OK || nvs_commit(nvs) != ESP_OK)
        return OZ_E_HW;
    return OZ_OK;
}

/* ── crypto ──────────────────────────────────────────────────────────────── */

int oz_port_sha256_init(oz_sha256_ctx *ctx) { oz_sw_sha256_init(ctx); return OZ_OK; }

int oz_port_sha256_update(oz_sha256_ctx *ctx, const uint8_t *d, size_t n)
{
    oz_sw_sha256_update(ctx, d, n);
    return OZ_OK;
}

int oz_port_sha256_final(oz_sha256_ctx *ctx, uint8_t out[32]) { oz_sw_sha256_final(ctx, out); return OZ_OK; }

int oz_port_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    int r = mbedtls_aes_setkey_enc(&c, key, 128);
    if (!r)
        r = mbedtls_aes_crypt_ecb(&c, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&c);           /* zeroises the key schedule */
    return r ? OZ_E_HW : OZ_OK;
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
    /* An all-zero result means a low-order peer key: refuse it (as the host). */
    return crypto_verify32(shared, zero) == 0 ? OZ_E_ARG : OZ_OK;
}

int oz_port_random(uint8_t *out, size_t len)
{
    if (!out && len)
        return OZ_E_ARG;
    esp_fill_random(out, len);
    return OZ_OK;
}

/* ── kv on NVS ───────────────────────────────────────────────────────────── */

static int key_ok(const char *key) { return key && key[0] && strlen(key) <= NVS_KEYMAX; }

int oz_port_kv_get(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    if (!key_ok(key) || !len)
        return OZ_E_ARG;
    if (!nvs_ready && oz_esp32c6_init() != OZ_OK)
        return OZ_E_HW;
    size_t n = 0;
    esp_err_t e = nvs_get_blob(nvs, key, NULL, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return OZ_E_NOT_FOUND;
    if (e != ESP_OK)
        return OZ_E_HW;
    if (n > cap)
        return OZ_E_NO_SPACE;
    if (n && nvs_get_blob(nvs, key, buf, &n) != ESP_OK)
        return OZ_E_HW;
    *len = n;
    return OZ_OK;
}

int oz_port_kv_set(const char *key, const uint8_t *buf, size_t len)
{
    if (!key_ok(key) || (!buf && len))
        return OZ_E_ARG;
    if (!nvs_ready && oz_esp32c6_init() != OZ_OK)
        return OZ_E_HW;
    static const uint8_t empty[1];
    esp_err_t e = nvs_set_blob(nvs, key, len ? buf : empty, len);
    if (e == ESP_ERR_NVS_NOT_ENOUGH_SPACE)
        return OZ_E_NO_SPACE;
    if (e != ESP_OK || nvs_commit(nvs) != ESP_OK)
        return OZ_E_HW;
    return OZ_OK;
}

int oz_port_kv_erase(const char *key)
{
    if (!key_ok(key))
        return OZ_E_ARG;
    if (!nvs_ready && oz_esp32c6_init() != OZ_OK)
        return OZ_E_HW;
    esp_err_t e = nvs_erase_key(nvs, key);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return OZ_OK;               /* erasing an absent key is not an error */
    if (e != ESP_OK || nvs_commit(nvs) != ESP_OK)
        return OZ_E_HW;
    return OZ_OK;
}

/* ── time, log ───────────────────────────────────────────────────────────── */

uint64_t oz_port_monotonic_ms(void) { return (uint64_t)esp_timer_get_time() / 1000u; }

void oz_port_log(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("[OZ] %s\n", line);
}
