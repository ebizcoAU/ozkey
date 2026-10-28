/* test_adv — advertising policy and packet bytes (CONTRACT.md, [XF-47]). */
#include "oz_test.h"
#include "oz_adv.h"

/* Wanted bytes written out by hand from the spec strings, not from the code. */
#define UUID_LE "01 00 00 00 00 00 4b 43 4f 4c 01 00 32 4b 5a 4f"

static void packets(void)
{
    uint8_t b[OZ_ADV_MAX];
    size_t n = oz_adv_data(b);
    expect("adv: flags, uuid list, name", b, n,
           "02 01 06  11 07 " UUID_LE "  07 09 4f 5a 4c 4f 43 4b");
    expect_int("adv: 29 of 31 bytes", (long long)n, 29);

    n = oz_adv_scan_rsp(b, 0, "ozk-acebe63acab8");
    expect("scan rsp: free", b, n, "16 21 " UUID_LE " 00 ac eb e6 3a");
    n = oz_adv_scan_rsp(b, 1, "ozk-ACEBE63ACAB8");
    expect("scan rsp: busy, upper-case id", b, n, "16 21 " UUID_LE " 01 ac eb e6 3a");
    expect_int("scan rsp: 23 of 31 bytes", (long long)n, 23);
    expect_int("no dash", (long long)oz_adv_scan_rsp(b, 0, "acebe63acab8"), 0);
    expect_int("short id", (long long)oz_adv_scan_rsp(b, 0, "ozk-aceb"), 0);
    expect_int("not hex", (long long)oz_adv_scan_rsp(b, 0, "ozk-acebx63a"), 0);
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

int main(void)
{
    packets();
    policy(0);
    policy(0xfffff000u);                /* every deadline straddles the wrap */
    policy(0x7fffffffu);
    return report("adv");
}
