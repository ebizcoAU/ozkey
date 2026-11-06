/* oz_wdt_ak1037 — the AK1037 core watchdog under the oz_wdt policy
 * (F-AK-WDT-1). Replaces the SDK main.c habit we inherited: hal_wdt_stop()
 * after boot unless SUPP_WATCHDOG, which nothing defines, so the watchdog
 * was off for the whole run.
 *
 *   oz_ak1037_wdt_boot()     once, early in the main thread: reads the
 *                            reset cause, runs the boot guard on the count
 *                            persisted in kv ("bg"), keeps the core watchdog
 *                            armed (the SDK armed it at 12.8 s in
 *                            hal_init_total) and registers the main loop.
 *   oz_ak1037_wdt_add()      a task registers its check-in period.
 *   oz_ak1037_wdt_checkin()  from that task's loop.
 *   oz_ak1037_wdt_service()  from the main loop: checks the main loop in,
 *                            feeds only while every task is alive, and
 *                            clears the boot count after a stable run.
 *   oz_ak1037_wdt_safe()     1 = this boot is SAFE: the lock keeps BLE and the
 *                            owner but never drives the motor.
 *
 * UNVERIFIED ON SILICON (the kit isn't here): the reset-status mapping, that
 * hal_wdt_feed() in our loop is enough with the BLE stack's own timing, and
 * the SDK drivers that start/feed the watchdog themselves (touch keys arm it
 * at 25.6 s in hal_tk.c, the fingerprint, UART OTA and crystal-trim code feed
 * it during long operations). The always-on watchdog stays the SDK's: its
 * low-power entry arms it at 32 s (lowpower.c) and we don't arm it here.
 */
#ifndef OZ_WDT_AK1037_H
#define OZ_WDT_AK1037_H

#include <stdint.h>

/* Main loop check-in period. The hardware watchdog is 12.8 s; a task is
 * "late" well before that, so a late task stops the feeds and the hardware
 * resets about 12.8 s later. */
#define OZ_AK1037_WDT_MAIN_MS  2000u

int  oz_ak1037_wdt_boot(void);
int  oz_ak1037_wdt_add(uint32_t period_ms);
void oz_ak1037_wdt_checkin(int id);
void oz_ak1037_wdt_service(void);
int  oz_ak1037_wdt_safe(void);

#endif
