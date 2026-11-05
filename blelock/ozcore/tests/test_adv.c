/* test_adv — advertising policy and packet bytes (CONTRACT.md, [XF-47]). */
#include "oz_test.h"
#include "oz_adv.h"
#include <stdlib.h>

/* Wanted bytes written out by hand from the spec strings, not from the code. */
#define UUID_LE "01 00 00 00 00 00 4b 43 4f 4c 01 00 32 4b 5a 4f"

static void packets(void)
{
    uint8_t b[OZ_ADV_MAX];
    size_t n = oz_adv_data(b);
    expect("adv: flags, uuid list, name", b, n,
           "02 01 06  11 07 " UUID_LE "  07 09 4f 5a 4c 4f 43 4b");
    expect_int("adv: 29 of 31 bytes", (long long)n, 29);

    /* Short id = SHA-256(device_id)[0..3] (ozkey-48 §2.2 test vectors,
     * computed with Python hashlib, not with this code). */
    n = oz_adv_scan_rsp(b, 0, "ozk-acebe63acab8");
    expect("scan rsp: free", b, n, "16 21 " UUID_LE " 00 2b 9c 2d 79");
    expect_int("scan rsp: 23 of 31 bytes", (long long)n, 23);
    n = oz_adv_scan_rsp(b, 1, "ozk-acebe63acab8");
    expect("scan rsp: busy", b, n, "16 21 " UUID_LE " 01 2b 9c 2d 79");
    n = oz_adv_scan_rsp(b, 0, "ozk-ACEBE63ACAB8");
    expect("no case folding: the bytes as-is", b, n, "16 21 " UUID_LE " 00 45 21 2d 8d");
    n = oz_adv_scan_rsp(b, 0, "x");
    expect("any maker's id format (no dash, no hex needed)", b, n, "16 21 " UUID_LE " 00 2d 71 16 42");
    expect_int("empty id", (long long)oz_adv_scan_rsp(b, 0, ""), 0);
    expect_int("40-byte id ok", (long long)oz_adv_scan_rsp(b, 0, "0123456789012345678901234567890123456789"), 23);
    expect_int("41-byte id refused", (long long)oz_adv_scan_rsp(b, 0, "01234567890123456789012345678901234567890"), 0);
    expect_int("null id", (long long)oz_adv_scan_rsp(b, 0, NULL), 0);
}

static void policy(uint32_t t0)
{
    char name[64];
    oz_adv a;
#define AT(ms) (t0 + (uint32_t)(ms))
#define MODE(claimed, ms, want, what)                                          \
    do {                                                                       \
        snprintf(name, sizeof name, "t0=%08x %s", (unsigned)t0, what);         \
        expect_int(name, oz_adv_mode_at(&a, claimed, AT(ms)), want);           \
    } while (0)

    /* Unclaimed: the post-boot grace, then dark until a touch. */
    oz_adv_init(&a, 0, t0);
    MODE(0, 0, OZ_ADV_OPEN, "unclaimed at boot");
    MODE(0, 119999, OZ_ADV_OPEN, "unclaimed, end of grace");
    MODE(0, 120000, OZ_ADV_OFF, "unclaimed, grace over");
    oz_adv_touch(&a, AT(200000));
    MODE(0, 200000, OZ_ADV_OPEN, "touch opens the window");
    MODE(0, 259999, OZ_ADV_OPEN, "window, last ms");
    MODE(0, 260000, OZ_ADV_OFF, "window closed");

    /* Claimed: dark from boot; only a touch opens it. */
    oz_adv_init(&a, 0, t0);
    MODE(1, 0, OZ_ADV_OFF, "claimed lock is dark at boot");
    oz_adv_touch(&a, AT(1000));
    MODE(1, 30000, OZ_ADV_OPEN, "claimed, touched");
    oz_adv_touch(&a, AT(50000));                    /* every touch re-arms */
    MODE(1, 100000, OZ_ADV_OPEN, "re-armed window");
    MODE(1, 110000, OZ_ADV_OFF, "re-armed window closed");

    /* A link: busy for its whole length, even past the window. */
    oz_adv_touch(&a, AT(200000));
    oz_adv_link(&a, 1);
    MODE(1, 210000, OZ_ADV_BUSY, "connected = busy");
    MODE(1, 400000, OZ_ADV_BUSY, "window expired mid-link, still busy");
    oz_adv_link(&a, 0);
    MODE(1, 400001, OZ_ADV_OFF, "expired window ends at disconnect");

    oz_adv_touch(&a, AT(500000));
    oz_adv_link(&a, 1);
    oz_adv_link(&a, 0);
    MODE(1, 510000, OZ_ADV_OPEN, "disconnect inside window: open again");
    oz_adv_link(&a, 0);                             /* a stray extra disconnect */
    oz_adv_link(&a, 1);
    MODE(1, 520000, OZ_ADV_BUSY, "stray disconnect did not go negative");
    oz_adv_link(&a, 0);

    /* Bridge-reachable lock: always advertising. */
    oz_adv_init(&a, 1, t0);
    MODE(1, 0, OZ_ADV_OPEN, "always: claimed at boot");
    MODE(1, 86400000, OZ_ADV_OPEN, "always: a day later");
    oz_adv_link(&a, 1);
    MODE(1, 86400001, OZ_ADV_BUSY, "always: busy while connected");

    /* The grace never comes back after the millisecond clock wraps. */
    oz_adv_init(&a, 0, t0);
    MODE(0, 120000, OZ_ADV_OFF, "grace over");
    MODE(0, 0x100000000ull - 1000u, OZ_ADV_OFF, "just before the clock returns to t0");
    MODE(0, 0x100000000ull + 1000u, OZ_ADV_OFF, "after a full wrap");
#undef MODE
#undef AT
}

/* Gate (PM ruling 2026-11-05): a synthetic 100K-lock site, every board an
 * Espressif part, ids "ozk-" + BT MAC. The old short id (the MAC's first 4
 * bytes = 3 OUI bytes + 1) gave at most 256 distinct values per OUI. SHA-256
 * must behave like a random 32-bit value: n^2 / 2^33 = 1.16 expected colliding
 * pairs for n = 100000, and even buckets. */
#ifndef OZ_SELFTEST_TARGET
/* Host only: 400 KB of ids would not fit the AK1037's 218 KiB SRAM. On the
 * chip, packets()'s fixed vectors check the short id hash; this run checks the
 * hash's statistics, which don't depend on the CPU. */
#define SITE 100000
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void site_collisions(void)
{
    static const uint8_t oui[][3] = {            /* Espressif OUIs, incl. the bench's */
        {0xac, 0xeb, 0xe6}, {0x98, 0xa3, 0x16}, {0x20, 0x6e, 0xf1}, {0xb0, 0xa6, 0x04},
        {0x24, 0x0a, 0xc4}, {0x30, 0xae, 0xa4}, {0x7c, 0xdf, 0xa1}, {0x84, 0xf7, 0x03} };
    static uint32_t sid[SITE];
    uint32_t bucket[256] = {0}, x = 0x4f5a4b32u;  /* fixed LCG seed: reproducible */
    char id[32];
    uint8_t b[OZ_ADV_MAX];
    int bad = 0, old_distinct = 0;
    for (int i = 0; i < SITE; i++) {
        const uint8_t *o = oui[i % 8];
        x = x * 1664525u + 1013904223u;
        /* distinct MACs: i picks OUI and the counter's low bits, the LCG the rest */
        uint32_t lo = ((uint32_t)(i / 8) << 8 | (x >> 24)) & 0xffffffu;
        snprintf(id, sizeof id, "ozk-%02x%02x%02x%02x%02x%02x", o[0], o[1], o[2],
                 (unsigned)(lo >> 16), (unsigned)(lo >> 8 & 0xff), (unsigned)(lo & 0xff));
        if (oz_adv_scan_rsp(b, 0, id) != 23) { bad++; continue; }
        sid[i] = (uint32_t)b[19] << 24 | (uint32_t)b[20] << 16 | (uint32_t)b[21] << 8 | b[22];
        bucket[b[19]]++;
    }
    expect_int("site: every id has a short id", bad, 0);
    /* the raw-MAC scheme on the same ids: 3 OUI bytes + 1 byte per OUI */
    {
        static uint8_t seen[8][256];
        memset(seen, 0, sizeof seen);
        x = 0x4f5a4b32u;
        for (int i = 0; i < SITE; i++) {
            x = x * 1664525u + 1013904223u;
            uint32_t lo = ((uint32_t)(i / 8) << 8 | (x >> 24)) & 0xffffffu;
            if (!seen[i % 8][lo >> 16]++) old_distinct++;
        }
    }
    qsort(sid, SITE, sizeof sid[0], cmp_u32);
    int pairs = 0, maxb = 0;
    for (int i = 1; i < SITE; i++) pairs += sid[i] == sid[i - 1];
    for (int i = 0; i < 256; i++) if ((int)bucket[i] > maxb) maxb = (int)bucket[i];
    printf("  site: 100000 Espressif ids: %d colliding short ids (expected ~1.16); "
           "old raw-MAC scheme: %d distinct values; busiest first-byte bucket %d (mean 390.6)\n",
           pairs, old_distinct, maxb);
    expect_int("site: <= 6 colliding pairs (P(>6) < 1e-3 for random 32-bit)", pairs <= 6, 1);
    expect_int("site: buckets even (max < 1.25 x mean)", maxb < 488, 1);
    expect_int("site: the raw-MAC scheme really was the problem (<= 8 x 256 values)", old_distinct <= 2048, 1);
}
#endif

int main(void)
{
    packets();
#ifndef OZ_SELFTEST_TARGET
    site_collisions();
#endif
    policy(0);
    policy(0xfffff000u);                /* every deadline straddles the wrap */
    policy(0x7fffffffu);
    return report("adv");
}
