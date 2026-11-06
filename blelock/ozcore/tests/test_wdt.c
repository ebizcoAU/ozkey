/* test_wdt — oz_wdt (F-AK-WDT-1): the check-in supervisor (feed only while
 * every task is alive, wrap-safe) and the boot guard (count watchdog resets,
 * SAFE at the limit, cleared by a power cycle or a stable NORMAL run). */
#include "oz_test.h"
#include "oz_wdt.h"

static void supervisor(void)
{
    oz_wdt w;
    int late;
    oz_wdt_init(&w);
    expect_int("no tasks: never feed", oz_wdt_may_feed(&w, 0, &late), 0);
    expect_int("…no late id", late, -1);
    expect_int("period 0 refused", oz_wdt_add(&w, 0, 0), -1);

    int main_id = oz_wdt_add(&w, 1000, 0);
    int oz_id = oz_wdt_add(&w, 500, 0);
    expect_int("main id", main_id, 0);
    expect_int("ozkie id", oz_id, 1);
    expect_int("fresh: feed", oz_wdt_may_feed(&w, 400, &late), 1);
    expect_int("at the period exactly: still feed", oz_wdt_may_feed(&w, 500, NULL), 1);
    expect_int("ozkie 1 ms late: no feed", oz_wdt_may_feed(&w, 501, &late), 0);
    expect_int("…late is ozkie", late, oz_id);

    oz_wdt_checkin(&w, oz_id, 501);
    expect_int("ozkie back: feed", oz_wdt_may_feed(&w, 900, NULL), 1);
    expect_int("main late (only the main loop hung)", oz_wdt_may_feed(&w, 1001, &late), 0);
    expect_int("…late is main", late, main_id);
    oz_wdt_checkin(&w, main_id, 1001);
    expect_int("both in: feed", oz_wdt_may_feed(&w, 1001, NULL), 1);

    oz_wdt_checkin(&w, 7, 99999);   /* unknown ids are ignored */
    oz_wdt_checkin(&w, -1, 99999);
    expect_int("bad ids change nothing", oz_wdt_may_feed(&w, 1001, NULL), 1);

    expect_int("3rd task", oz_wdt_add(&w, 100, 1001), 2);
    expect_int("4th task", oz_wdt_add(&w, 100, 1001), 3);
    expect_int("5th refused (full)", oz_wdt_add(&w, 100, 1001), -1);

    /* The clock wraps: only differences count. */
    oz_wdt_init(&w);
    uint32_t t0 = 0xffffff00u;
    int a = oz_wdt_add(&w, 1000, t0);
    expect_int("across the wrap, in time", oz_wdt_may_feed(&w, t0 + 900u, NULL), 1);
    expect_int("across the wrap, late", oz_wdt_may_feed(&w, t0 + 1001u, NULL), 0);
    oz_wdt_checkin(&w, a, t0 + 1001u);
    expect_int("…after a check-in past the wrap", oz_wdt_may_feed(&w, t0 + 1500u, NULL), 1);
}

static void bootguard(void)
{
    uint8_t c = 0;
    int dirty;
    expect_int("power-on: normal", oz_bootguard_boot(&c, OZ_RST_POWER, &dirty), 0);
    expect_int("…nothing to persist", dirty, 0);
    expect_int("1st wdt reset: normal", oz_bootguard_boot(&c, OZ_RST_WDT, &dirty), 0);
    expect_int("…count 1, persist", c == 1 && dirty, 1);
    expect_int("a soft reset keeps the count", oz_bootguard_boot(&c, OZ_RST_SOFT, &dirty) == 0 && c == 1 && !dirty, 1);
    expect_int("other causes keep it", oz_bootguard_boot(&c, OZ_RST_OTHER, &dirty) == 0 && c == 1 && !dirty, 1);
    expect_int("2nd wdt reset: normal", oz_bootguard_boot(&c, OZ_RST_WDT, &dirty), 0);
    expect_int("3rd wdt reset: SAFE", oz_bootguard_boot(&c, OZ_RST_WDT, &dirty), 1);
    expect_int("…count 3", c, 3);
    expect_int("SAFE survives a soft reset", oz_bootguard_boot(&c, OZ_RST_SOFT, &dirty), 1);
    expect_int("SAFE never clears itself", oz_bootguard_stable(&c, 10u * OZ_BOOTGUARD_STABLE_MS, 1), 0);
    expect_int("…still 3", c, 3);
    expect_int("a power cycle: normal again", oz_bootguard_boot(&c, OZ_RST_POWER, &dirty), 0);
    expect_int("…count 0, persist", c == 0 && dirty, 1);

    /* A stable NORMAL run forgives an old watchdog reset. */
    c = 0;
    (void)oz_bootguard_boot(&c, OZ_RST_WDT, &dirty);
    (void)oz_bootguard_boot(&c, OZ_RST_WDT, &dirty);
    expect_int("not stable yet", oz_bootguard_stable(&c, OZ_BOOTGUARD_STABLE_MS - 1u, 0), 0);
    expect_int("…count 2", c, 2);
    expect_int("stable: cleared, persist", oz_bootguard_stable(&c, OZ_BOOTGUARD_STABLE_MS, 0), 1);
    expect_int("…count 0", c, 0);
    expect_int("nothing to clear twice", oz_bootguard_stable(&c, OZ_BOOTGUARD_STABLE_MS, 0), 0);
    expect_int("so the next wdt reset is the 1st again", oz_bootguard_boot(&c, OZ_RST_WDT, &dirty) == 0 && c == 1, 1);

    /* The count saturates rather than wrapping back to NORMAL. */
    c = 0xff;
    expect_int("saturated count stays SAFE", oz_bootguard_boot(&c, OZ_RST_WDT, &dirty) == 1 && c == 0xff && !dirty, 1);
    /* A corrupt persisted value (any non-zero) is still a count: >= limit is SAFE. */
    c = 200;
    expect_int("a large stored count boots SAFE", oz_bootguard_boot(&c, OZ_RST_SOFT, &dirty), 1);
    expect_int("NULL dirty is allowed", oz_bootguard_boot(&c, OZ_RST_POWER, NULL) == 0 && c == 0, 1);
}

int main(void)
{
    supervisor();
    bootguard();
    return report("wdt");
}
