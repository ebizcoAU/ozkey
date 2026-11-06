/* test_ak1037_wdt — oz_wdt_ak1037 (F-AK-WDT-1) on the SDK mocks: the reset
 * cause mapping, the boot count in kv, the watchdog kept on, feeds only while
 * every task checks in, and the stable-run clear. One boot per process (the
 * port keeps per-boot state), so each scenario is its own ctest run:
 *
 *   por      power-on with a stored count of 2: cleared, NORMAL
 *   wdt      a core watchdog reset with none stored: count 1 saved, NORMAL
 *   aowdt    an always-on watchdog reset with 2 stored: count 3, SAFE
 *   soft     a software reset with 2 stored: unchanged, NORMAL
 *   wake     a wake-up (boot state != 0) after a watchdog: not counted
 *   feed     feeds while every task checks in, none once one is late
 *   stable   a stable NORMAL run clears the count in kv; SAFE never does
 *   selftest the self-test owns kv: the clear is not written
 */
#include <stdlib.h>

#include "oz_test.h"
#include "oz_port.h"
#include "oz_wdt.h"
#include "oz_port_ak1037.h"
#include "oz_wdt_ak1037.h"
#include "hal_watchdog.h"
#include "hal_sysclock.h"
#include "mock_sdk.h"

static void preset(int have, uint8_t count)
{
    mock_flash_blank();
    if (have)
        expect_int("preset count", oz_port_kv_set("bg", &count, 1), OZ_OK);
}

static int stored(void)
{
    uint8_t v = 0xee;
    size_t len = 0;
    if (oz_port_kv_get("bg", &v, 1, &len) != OZ_OK || len != 1)
        return -1;
    return v;
}

static int boot_with(int status, int boot_state)
{
    mock_reset_status = status;
    mock_boot_state = boot_state;
    mock_ms = 1000;
    return oz_ak1037_wdt_boot();
}

static void common_after_boot(void)
{
    expect_int("watchdog on", mock_wdt_enabled, 1);
    expect_int("…at 12.8 s", mock_wdt_period, WATCHDOG_12P8S);
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "";

    if (!strcmp(m, "por")) {
        preset(1, 2);
        expect_int("power-on: NORMAL", boot_with(HAL_AO_POR_RST, 0), 0);
        expect_int("count cleared in kv", stored(), 0);
        common_after_boot();
    } else if (!strcmp(m, "wdt")) {
        preset(0, 0);
        expect_int("1st watchdog reset: NORMAL", boot_with(HAL_CORE_WDT_RST, 0), 0);
        expect_int("count 1 saved", stored(), 1);
        common_after_boot();
    } else if (!strcmp(m, "aowdt")) {
        preset(1, 2);
        expect_int("3rd (always-on) watchdog reset: SAFE", boot_with(HAL_AO_WDT_RST, 0), 1);
        expect_int("count 3 saved", stored(), 3);
        expect_int("safe() agrees", oz_ak1037_wdt_safe(), 1);
        expect_int("a second boot call changes nothing", oz_ak1037_wdt_boot() == 1 && stored() == 3, 1);
        common_after_boot();
    } else if (!strcmp(m, "soft")) {
        preset(1, 2);
        expect_int("soft reset: NORMAL", boot_with(HAL_SOFT_RESET_RST, 0), 0);
        expect_int("count kept", stored(), 2);
    } else if (!strcmp(m, "wake")) {
        preset(1, 2);
        expect_int("wake-up: NORMAL", boot_with(HAL_CORE_WDT_RST, 1), 0);
        expect_int("a wake-up is not a reset: count kept", stored(), 2);
    } else if (!strcmp(m, "feed")) {
        preset(0, 0);
        oz_ak1037_wdt_service();
        expect_int("no feed before boot", mock_wdt_feeds, 0);
        (void)boot_with(HAL_AO_POR_RST, 0);
        long f0 = mock_wdt_feeds;
        int task = oz_ak1037_wdt_add(500);
        expect_int("task registered", task >= 0, 1);
        for (int i = 0; i < 10; i++) {
            mock_ms += 100;
            oz_ak1037_wdt_checkin(task);
            oz_ak1037_wdt_service();
        }
        expect_int("fed every pass while alive", mock_wdt_feeds - f0, 10);
        long f1 = mock_wdt_feeds;
        for (int i = 0; i < 10; i++) {   /* the task hangs; the main loop runs on */
            mock_ms += 100;
            oz_ak1037_wdt_service();
        }
        expect_int("fed until the task is 500 ms late, then never", mock_wdt_feeds - f1, 5);
        oz_ak1037_wdt_checkin(task);
        oz_ak1037_wdt_service();
        expect_int("feeds again once it checks in", mock_wdt_feeds - f1, 6);
        /* The main loop itself stalls past its period: no feed on its next pass
         * would be wrong (it is back), so the main loop checks itself in. */
        mock_ms += 3 * OZ_AK1037_WDT_MAIN_MS;
        oz_ak1037_wdt_checkin(task);
        long f2 = mock_wdt_feeds;
        oz_ak1037_wdt_service();
        expect_int("main loop back: feeds", mock_wdt_feeds - f2, 1);
        int fill = 0;
        while (oz_ak1037_wdt_add(1000) >= 0 && fill < 10)
            fill++;
        expect_int("table holds main + 3 tasks", fill, OZ_WDT_MAX_TASKS - 2);
    } else if (!strcmp(m, "stable")) {
        preset(1, 1);
        expect_int("2nd watchdog reset: NORMAL", boot_with(HAL_CORE_WDT_RST, 0), 0);
        expect_int("count 2 saved", stored(), 2);
        mock_ms += OZ_BOOTGUARD_STABLE_MS - 10;
        oz_ak1037_wdt_service();
        expect_int("not yet stable", stored(), 2);
        mock_ms += 20;
        oz_ak1037_wdt_service();
        expect_int("stable: cleared in kv", stored(), 0);
    } else if (!strcmp(m, "safestay")) {
        preset(1, 5);
        expect_int("SAFE", boot_with(HAL_SOFT_RESET_RST, 0), 1);
        mock_ms += 10 * OZ_BOOTGUARD_STABLE_MS;
        oz_ak1037_wdt_service();
        expect_int("SAFE never clears itself", stored(), 5);
    } else if (!strcmp(m, "selftest")) {
        preset(1, 1);
        (void)boot_with(HAL_CORE_WDT_RST, 0);
        expect_int("count 2 saved", stored(), 2);
        expect_int("self-test claims kv", oz_ak1037_kv_claim(OZ_AK1037_KV_SELFTEST), OZ_OK);
        mock_ms += OZ_BOOTGUARD_STABLE_MS + 1;
        oz_ak1037_wdt_service();
        expect_int("clear not written under the self-test", stored(), 2);
    } else {
        printf("usage: test_ak1037_wdt por|wdt|aowdt|soft|wake|feed|stable|safestay|selftest\n");
        return 2;
    }
    expect_int("flash stayed in OZKV", mock_flash_out_of_part, 0);
    return report(m);
}
