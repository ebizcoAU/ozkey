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
 *   kv       littlefs on the OZKV partition via port/common/oz_kv_lfs.c
 *            (P4 code; power-cut tested on the host, not yet on silicon).
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
#include "hal_sfc_norflash.h"
#include "hal_partition.h"
#include "lfs.h"
#include "oz_kv_lfs.h"
#include "oz_port_ak1037.h"

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

    /* oz_ct_equal() is 0 when equal. */
    ske_state = (ske_block(key, pt, out) == OZ_OK && oz_ct_equal(out, ct, 16) == 0) ? 1 : -1;
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

int oz_ak1037_ske_state(void)
{
    if (ske_state == 0) ske_selftest();
    return ske_state;
}

int oz_ak1037_ske_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    return ske_block(key, in, out);
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

int oz_ak1037_trng_word(uint32_t *w)
{
    if (!trng_ready) { hal_trng_init(); trng_ready = 1; }
    return hal_trng_get_randbuff_bytes((uint8_t *)w, 4) != 0 ? OZ_E_HW : OZ_OK;
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

/* ── kv (P4): littlefs on our own "OZKV" partition ───────────────────────────
 *
 * The SDK's ak_lfs wrapper isn't used. It has no remove/rename, it never
 * formats, and it reads through the XIP window straight after programming
 * with the cache flush commented out, so a read-back can see stale data. This
 * block device reads with spi_flash_read() instead. The kv logic itself is
 * port/common/oz_kv_lfs.c, the same code the host power-cut test runs.
 *
 * UNVERIFIED until the dev kit (P4): the partition has to exist in the burn
 * config, spi_flash_write/erase must be safe while code runs XIP from the
 * same chip (the SDK's own lfs relies on it), and the read buffers have no
 * known alignment rule. */

#define OZ_KV_PARTITION "OZKV"

static lfs_t kv_fs;
static struct lfs_config kv_cfg;
static uint32_t kv_start_page;
static int kv_state;                    /* 0 not tried, 1 mounted, <0 failed */
static uint64_t kv_retry_at;            /* after a failure: no new attempt before this */
#ifdef LFS_THREADSAFE
static struct rt_mutex kv_mutex;
#endif

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n)
{
    uint32_t page = kv_start_page + b * SPIFLASH_PAGE_PER_SECTOR + off / SPIFLASH_PAGE_SIZE;
    (void)c;
    return spi_flash_read(page, (T_U8 *)buf, n / SPIFLASH_PAGE_SIZE) == AK_TRUE ? 0 : LFS_ERR_IO;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n)
{
    uint32_t page = kv_start_page + b * SPIFLASH_PAGE_PER_SECTOR + off / SPIFLASH_PAGE_SIZE;
    (void)c;
    return spi_flash_write(page, (T_U8 *)buf, n / SPIFLASH_PAGE_SIZE) == AK_TRUE ? 0 : LFS_ERR_IO;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;
    return spi_flash_sector_erase(kv_start_page / SPIFLASH_PAGE_PER_SECTOR + b) == AK_TRUE ? 0 : LFS_ERR_IO;
}

static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

#ifdef LFS_THREADSAFE
static int bd_lock(const struct lfs_config *c) { (void)c; return rt_mutex_take(&kv_mutex, RT_WAITING_FOREVER) == RT_EOK ? 0 : LFS_ERR_IO; }
static int bd_unlock(const struct lfs_config *c) { (void)c; rt_mutex_release(&kv_mutex); return 0; }
#endif

/* One mount attempt: OZ_OK, or OZ_E_HW with nothing left mounted. */
static int kv_mount(void)
{
    T_SPI_FILE_INFO info;
    memset(&info, 0, sizeof info);
    memcpy(info.file_name, OZ_KV_PARTITION, sizeof OZ_KV_PARTITION);
    if (partition_get_bin_info(&info) != AK_TRUE) {
        oz_port_log("kv: no %s partition", OZ_KV_PARTITION);
        return OZ_E_HW;
    }
    if (info.file_start_page % SPIFLASH_PAGE_PER_SECTOR ||
        info.file_partition_size < 4 * SPIFLASH_SECTOR_SIZE ||
        info.file_partition_size % SPIFLASH_SECTOR_SIZE) {
        oz_port_log("kv: %s partition not sector-aligned", OZ_KV_PARTITION);
        return OZ_E_HW;
    }
    kv_start_page = info.file_start_page;

    memset(&kv_cfg, 0, sizeof kv_cfg);
    kv_cfg.read = bd_read;
    kv_cfg.prog = bd_prog;
    kv_cfg.erase = bd_erase;
    kv_cfg.sync = bd_sync;
#ifdef LFS_THREADSAFE
    static int mutex_ready;
    if (!mutex_ready) {
        rt_mutex_init(&kv_mutex, "ozkv", RT_IPC_FLAG_FIFO);
        mutex_ready = 1;
    }
    kv_cfg.lock = bd_lock;
    kv_cfg.unlock = bd_unlock;
#endif
    kv_cfg.read_size = SPIFLASH_PAGE_SIZE;
    kv_cfg.prog_size = SPIFLASH_PAGE_SIZE;
    kv_cfg.block_size = SPIFLASH_SECTOR_SIZE;
    kv_cfg.block_count = info.file_partition_size / SPIFLASH_SECTOR_SIZE;
    kv_cfg.cache_size = SPIFLASH_PAGE_SIZE;
    kv_cfg.lookahead_size = 16;
    kv_cfg.block_cycles = 500;

    if (lfs_mount(&kv_fs, &kv_cfg) != 0) {
        /* First boot (blank partition), or a superblock beyond repair. Either
         * way the lock starts unpaired, the same as a factory reset. */
        oz_port_log("kv: formatting %s (%u KiB)", OZ_KV_PARTITION,
                    (unsigned)(info.file_partition_size / 1024));
        if (lfs_format(&kv_fs, &kv_cfg) != 0 || lfs_mount(&kv_fs, &kv_cfg) != 0) {
            oz_port_log("kv: format failed");
            return OZ_E_HW;
        }
    }
    if (oz_kv_lfs_attach(&kv_fs) != OZ_OK) {
        oz_port_log("kv: attach failed");
        (void)lfs_unmount(&kv_fs);
        return OZ_E_HW;
    }
    return OZ_OK;
}

/* Mount on first use. A failed mount is tried again on a later call, at most
 * once per OZ_KV_RETRY_MS, so a dead flash can't keep the OZKIE thread busy
 * (PM ruling 2026-10-29). A lock without kv has no identity, so giving up for
 * the whole boot is not an option. */
#define OZ_KV_RETRY_MS 5000u

static int kv_ready(void)
{
    if (kv_state > 0)
        return OZ_OK;
    uint64_t now = oz_port_monotonic_ms();
    if (kv_state < 0 && now < kv_retry_at)
        return OZ_E_HW;
    if (kv_mount() == OZ_OK) {
        if (kv_state < 0)
            oz_port_log("kv: mounted on retry");
        kv_state = 1;
        return OZ_OK;
    }
    kv_state = -1;
    kv_retry_at = now + OZ_KV_RETRY_MS;
    return OZ_E_HW;
}

int oz_ak1037_kv_mount(void) { return kv_ready(); }

int oz_ak1037_kv_claim(int owner)
{
    static int kv_owner;
    rt_base_t level = rt_hw_interrupt_disable();
    int ok = kv_owner == 0 || kv_owner == owner;
    if (ok) kv_owner = owner;
    rt_hw_interrupt_enable(level);
    return ok ? OZ_OK : OZ_E_ARG;
}

int oz_port_kv_get(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    int rc = kv_ready();
    return rc ? rc : oz_kv_lfs_get(key, buf, cap, len);
}

int oz_port_kv_set(const char *key, const uint8_t *buf, size_t len)
{
    int rc = kv_ready();
    return rc ? rc : oz_kv_lfs_set(key, buf, len);
}

int oz_port_kv_erase(const char *key)
{
    int rc = kv_ready();
    return rc ? rc : oz_kv_lfs_erase(key);
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
