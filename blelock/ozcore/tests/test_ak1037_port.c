/* test_ak1037_port — port/ak1037/oz_port_ak1037.c on the host, against the SDK
 * mocks in port/ak1037/hostmock (ozkey-49 P5-A).
 *
 *   test_ak1037_port good | wrong | error | blank | nopart | badpart
 *
 * The SKE known-answer test runs once per boot, so each engine behaviour is
 * its own process (its own ctest):
 *   good     the engine is right: it must be used, and the key must not be
 *            left in its registers
 *   wrong    the engine flips a bit: the KAT must reject it and AES must come
 *            from software (the 6cc74ba fail-open regression: with the
 *            oz_ct_equal() test inverted, `good` and `wrong` both fail)
 *   error    the engine returns an error: same as wrong
 * kv runs on a mock NOR flash, through the partition table, and mounts once
 * per boot, so these are processes too:
 *   good     a filesystem written by plain littlefs at the partition offset
 *            is read back, not formatted, and the other way round;
 *            set/get/erase; a failed program is recovered by the next call
 *   blank    a factory-blank partition is formatted on first use
 *   nopart / badpart   no OZKV partition, or one not sector-aligned: kv fails
 * `good` also runs the TRNG (good, stuck, timeout) and the 32-bit ms wrap.
 */
#include <stdlib.h>

#include "oz_test.h"
#include "oz_port.h"
#include "oz_sw.h"
#include "oz_port_ak1037.h"
#include "lfs.h"
#include "rthw.h"
#include "hal_sfc_norflash.h"
#include "mock_sdk.h"

/* ── SKE ───────────────────────────────────────────────────────────────── */

static int key_regs_clear(void)
{
    static const uint8_t zero[16];
    return !memcmp(mock_ske_key_reg, zero, 16);
}

static void aes_matches_software(const char *what, int engine_used)
{
    int same = 1, regs_clear = 1;
    long before = mock_ske_blocks;
    for (int i = 0; i < 64; i++) {
        uint8_t key[16], in[16], hw[16], sw[16];
        oz_port_random(key, sizeof key);
        oz_port_random(in, sizeof in);
        expect_int("aes rc", oz_port_aes128_encrypt_block(key, in, hw), OZ_OK);
        oz_sw_aes128_encrypt_block(key, in, sw);
        same &= !memcmp(hw, sw, 16);
        regs_clear &= key_regs_clear();
    }
    char name[64];
    snprintf(name, sizeof name, "%s: AES equals software", what);
    expect_int(name, same, 1);
    snprintf(name, sizeof name, "%s: blocks through the engine", what);
    expect_int(name, mock_ske_blocks - before, engine_used ? 64 : 0);
    snprintf(name, sizeof name, "%s: key registers wiped", what);
    expect_int(name, regs_clear, 1);
}

static void ske(int mode)
{
    mock_ske_mode = mode;
    int want = mode == MOCK_SKE_GOOD ? 1 : -1;
    expect_int("ske state after the KAT", oz_ak1037_ske_state(), want);
    expect_int("ske KAT ran once", mock_ske_blocks, mode == MOCK_SKE_ERROR ? 0 : 1);
    expect_int("ske KAT left no key", key_regs_clear(), 1);
    aes_matches_software(mode == MOCK_SKE_GOOD ? "good engine" : "bad engine", mode == MOCK_SKE_GOOD);

    /* The self-test image's direct path never falls back. */
    uint8_t key[16] = { 1 }, in[16] = { 2 }, hw[16], sw[16];
    oz_sw_aes128_encrypt_block(key, in, sw);
    int rc = oz_ak1037_ske_block(key, in, hw);
    expect_int("ske_block rc", rc, mode == MOCK_SKE_ERROR ? OZ_E_HW : OZ_OK);
    if (mode == MOCK_SKE_WRONG)
        expect_int("ske_block shows the wrong engine", !memcmp(hw, sw, 16), 0);
    if (mode == MOCK_SKE_GOOD)
        expect_int("ske_block equals software", !memcmp(hw, sw, 16), 1);

    expect_int("SKE only with interrupts off", mock_irq_violations, 0);
    expect_int("interrupts back on", rt_hw_interrupt_disable(), 0);
    rt_hw_interrupt_enable(0);
}

/* ── TRNG ──────────────────────────────────────────────────────────────── */

static void trng(void)
{
    uint8_t a[100], b[100];
    uint32_t w;
    expect_int("random a", oz_port_random(a, sizeof a), OZ_OK);
    expect_int("random b", oz_port_random(b, sizeof b), OZ_OK);
    expect_int("random differs", !memcmp(a, b, sizeof a), 0);
    expect_int("trng word", oz_ak1037_trng_word(&w), OZ_OK);
    expect_int("trng initialised once", mock_trng_inits, 1);

    mock_trng_mode = MOCK_TRNG_STUCK;
    expect_int("stuck trng refused", oz_port_random(a, 32), OZ_E_HW);
    mock_trng_mode = MOCK_TRNG_TIMEOUT;
    expect_int("trng timeout refused", oz_port_random(a, 32), OZ_E_HW);
    expect_int("trng word timeout", oz_ak1037_trng_word(&w), OZ_E_HW);
    mock_trng_mode = MOCK_TRNG_GOOD;
    expect_int("trng recovers", oz_port_random(a, 32), OZ_OK);
}

/* ── time ──────────────────────────────────────────────────────────────── */

static void time_wrap(void)
{
    mock_ms = 0xffffff00u;
    uint64_t t1 = oz_port_monotonic_ms();
    mock_ms = 0x100u;
    uint64_t t2 = oz_port_monotonic_ms();
    expect_int("ms tick wraps forward", (long long)(t2 - t1), 0x200);
    mock_ms = 0x180u;
    expect_int("then keeps counting", (long long)(oz_port_monotonic_ms() - t1), 0x280);
}

/* ── kv ────────────────────────────────────────────────────────────────── */

/* Plain littlefs straight on the mock flash at the partition's offset: the
 * geometry the port must use, written down independently. */
static uint8_t *part(void) { return mock_flash + (size_t)mock_part_start_page * SPIFLASH_PAGE_SIZE; }

static int raw_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n)
{
    (void)c;
    memcpy(buf, part() + b * SPIFLASH_SECTOR_SIZE + off, n);
    return 0;
}

static int raw_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n)
{
    (void)c;
    uint8_t *p = part() + b * SPIFLASH_SECTOR_SIZE + off;
    const uint8_t *s = buf;
    for (lfs_size_t i = 0; i < n; i++)
        p[i] &= s[i];
    return 0;
}

static int raw_erase(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;
    memset(part() + b * SPIFLASH_SECTOR_SIZE, 0xff, SPIFLASH_SECTOR_SIZE);
    return 0;
}

static int raw_sync(const struct lfs_config *c) { (void)c; return 0; }

static struct lfs_config raw_cfg(void)
{
    struct lfs_config c;
    memset(&c, 0, sizeof c);
    c.read = raw_read;
    c.prog = raw_prog;
    c.erase = raw_erase;
    c.sync = raw_sync;
    c.read_size = c.prog_size = c.cache_size = SPIFLASH_PAGE_SIZE;
    c.block_size = SPIFLASH_SECTOR_SIZE;
    c.block_count = mock_part_size / SPIFLASH_SECTOR_SIZE;
    c.lookahead_size = 16;
    c.block_cycles = 500;
    return c;
}

static int raw_file(int write, const char *path, char *buf, size_t cap)
{
    struct lfs_config c = raw_cfg();
    lfs_t fs;
    lfs_file_t f;
    int n = -1;
    if (write ? lfs_format(&fs, &c) || lfs_mount(&fs, &c) : lfs_mount(&fs, &c))
        return -1;
    if (!lfs_file_open(&fs, &f, path, write ? LFS_O_WRONLY | LFS_O_CREAT : LFS_O_RDONLY)) {
        n = write ? (int)lfs_file_write(&fs, &f, buf, (lfs_size_t)strlen(buf))
                  : (int)lfs_file_read(&fs, &f, buf, (lfs_size_t)cap);
        lfs_file_close(&fs, &f);
    }
    lfs_unmount(&fs);
    return n;
}

static void kv(void)
{
    uint8_t buf[64];
    size_t n = 0;

    /* A filesystem already on flash (as after a reboot) is used, not formatted. */
    mock_flash_blank();
    char pre[16] = "before";
    expect_int("raw lfs wrote oz.pre", raw_file(1, "oz.pre", pre, 0), 6);
    long erases = mock_flash_erases;
    expect_int("port reads oz.pre", oz_port_kv_get("pre", buf, sizeof buf, &n), OZ_OK);
    expect("oz.pre value", buf, n, "6265666f7265");
    expect_int("mount did not format", mock_flash_erases, erases);

    expect_int("set", oz_port_kv_set("k1", (const uint8_t *)"\x01\x02\x03", 3), OZ_OK);
    expect_int("get", oz_port_kv_get("k1", buf, sizeof buf, &n), OZ_OK);
    expect("k1 value", buf, n, "010203");
    expect_int("missing key", oz_port_kv_get("nope", buf, sizeof buf, &n), OZ_E_NOT_FOUND);

    /* A failed program: this call fails, the next ones work, values intact. */
    mock_flash_fail_prog_at = 1;
    expect_int("set with a failed program", oz_port_kv_set("k1", (const uint8_t *)"\x09", 1), OZ_E_HW);
    expect_int("get after the failure", oz_port_kv_get("k1", buf, sizeof buf, &n), OZ_OK);
    expect("old value kept", buf, n, "010203");
    expect_int("set again", oz_port_kv_set("k1", (const uint8_t *)"\x04\x05", 2), OZ_OK);
    expect_int("get new", oz_port_kv_get("k1", buf, sizeof buf, &n), OZ_OK);
    expect("k1 new value", buf, n, "0405");
    expect_int("erase", oz_port_kv_erase("k1"), OZ_OK);
    expect_int("erased", oz_port_kv_get("k1", buf, sizeof buf, &n), OZ_E_NOT_FOUND);
    expect_int("set k2", oz_port_kv_set("k2", (const uint8_t *)"after", 5), OZ_OK);

    /* What the port wrote, plain littlefs reads at the partition offset. */
    char got[16] = { 0 };
    expect_int("raw lfs reads oz.k2", raw_file(0, "oz.k2", got, sizeof got - 1), 5);
    expect_int("oz.k2 value", strcmp(got, "after"), 0);

    expect_int("no access outside OZKV", mock_flash_out_of_part, 0);
    expect_int("NOR rule kept", mock_nor_violations, 0);

    /* One kv owner per boot. */
    expect_int("claim selftest", oz_ak1037_kv_claim(OZ_AK1037_KV_SELFTEST), OZ_OK);
    expect_int("claim selftest again", oz_ak1037_kv_claim(OZ_AK1037_KV_SELFTEST), OZ_OK);
    expect_int("lock refused", oz_ak1037_kv_claim(OZ_AK1037_KV_LOCK), OZ_E_ARG);
}

/* Blank flash formats on first use (a factory-fresh lock). */
static void kv_blank(void)
{
    uint8_t buf[8];
    size_t n;
    mock_flash_blank();
    expect_int("blank: mount formats", oz_ak1037_kv_mount(), OZ_OK);
    expect_int("blank: erased something", mock_flash_erases > 0, 1);
    expect_int("blank: empty", oz_port_kv_get("k", buf, sizeof buf, &n), OZ_E_NOT_FOUND);
}

static void kv_unusable(const char *what)
{
    uint8_t buf[8];
    size_t n;
    mock_flash_blank();
    expect_int(what, oz_port_kv_get("k", buf, sizeof buf, &n), OZ_E_HW);
    expect_int("set fails too", oz_port_kv_set("k", buf, 1), OZ_E_HW);
    expect_int("nothing written", mock_flash_progs + mock_flash_erases, 0);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "good";

    mock_flash_blank();
    if (!strcmp(mode, "good")) {
        ske(MOCK_SKE_GOOD);
        trng();
        time_wrap();
        kv();
    } else if (!strcmp(mode, "blank")) {
        kv_blank();
    } else if (!strcmp(mode, "wrong")) {
        ske(MOCK_SKE_WRONG);
    } else if (!strcmp(mode, "error")) {
        ske(MOCK_SKE_ERROR);
    } else if (!strcmp(mode, "nopart")) {
        mock_part_present = 0;
        kv_unusable("no OZKV partition");
    } else if (!strcmp(mode, "badpart")) {
        mock_part_start_page += 1;          /* not on a sector boundary */
        kv_unusable("unaligned OZKV partition");
    } else {
        printf("unknown mode %s\n", mode);
        return 2;
    }
    char suite[48];
    snprintf(suite, sizeof suite, "ak1037_port %s", mode);
    return report(suite);
}
