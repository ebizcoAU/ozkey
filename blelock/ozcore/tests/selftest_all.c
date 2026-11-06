/* All the P2 suites in one run (oz_selftest.h). test_kv_lfs stays host-only:
 * it simulates power cuts on a RAM flash, and silicon has the real one. */
#include <stdio.h>

#include "oz_selftest.h"

int oz_t_crypto(void);
int oz_t_cbor(void);
int oz_t_edhoc(void);
int oz_t_oscore(void);
int oz_t_invite(void);
int oz_t_frame(void);
int oz_t_adv(void);
int oz_t_keypad(void);
int oz_t_motor(void);
int oz_t_events(void);
int oz_t_card(void);
int oz_t_power(void);
int oz_t_wdt(void);
int oz_t_lock(void);

static const struct {
    const char *name;
    int (*run)(void);
} suites[] = {
    { "crypto", oz_t_crypto },
    { "cbor",   oz_t_cbor },
    { "edhoc",  oz_t_edhoc },
    { "oscore", oz_t_oscore },
    { "invite", oz_t_invite },
    { "frame",  oz_t_frame },
    { "adv",    oz_t_adv },
    { "keypad", oz_t_keypad },
    { "motor",  oz_t_motor },
    { "events", oz_t_events },
    { "card",   oz_t_card },
    { "power",  oz_t_power },
    { "wdt",    oz_t_wdt },
    { "lock",   oz_t_lock },
};

int oz_selftest_suites(void)
{
    int failed = 0;
    for (unsigned i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        printf("== %s\r\n", suites[i].name);
        int rc = suites[i].run();
        printf("== %s %s\r\n", suites[i].name, rc ? "FAIL" : "PASS");
        failed += rc != 0;
    }
    return failed;
}

#ifdef OZ_SELFTEST_MAIN
int main(void)
{
    int failed = oz_selftest_suites();
    printf("selftest_all: %d suite(s) failed\n", failed);
    return failed ? 1 : 0;
}
#endif
