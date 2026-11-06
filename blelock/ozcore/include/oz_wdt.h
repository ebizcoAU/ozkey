/* oz_wdt — the watchdog policy (F-AK-WDT-1, PM nexus-6e 2026-11-06).
 *
 * Portable and pure; the port owns the hardware watchdog and the clock.
 *
 * Supervisor: each long-running task registers a period and checks in from
 * its own loop. The port feeds the hardware watchdog only while EVERY task
 * has checked in within its period, so a hung task (not just a hung main
 * loop) ends in a watchdog reset. A blind feed from an idle loop proves
 * nothing and is what this replaces.
 *
 * Boot guard: counts consecutive watchdog resets across reboots (the port
 * persists the count). At OZ_BOOTGUARD_LIMIT the port boots SAFE: BLE and
 * the owner only, motor off, the fault logged. Instead of a reset loop that
 * drains the battery and never answers anyone.
 *   - A watchdog reset (core or always-on) adds one.
 *   - A power-on reset clears it: a person swapped the batteries or
 *     power-cycled the lock, so it gets a fresh chance.
 *   - A software reset (our own reboot after a wipe or an update) and any
 *     other cause leave it as it is.
 *   - OZ_BOOTGUARD_STABLE_MS of uptime in NORMAL mode clears it (a single
 *     watchdog reset a week apart is not a loop). SAFE mode never clears it
 *     by itself: only a power cycle does, so the lock does not go back into
 *     the loop on its own.
 *
 * Times are uint32 ms from the port's monotonic clock and may wrap: only
 * differences are used.
 */
#ifndef OZ_WDT_H
#define OZ_WDT_H

#include <stdint.h>

#define OZ_WDT_MAX_TASKS        4
#define OZ_BOOTGUARD_LIMIT      3u
#define OZ_BOOTGUARD_STABLE_MS  (2u * 60u * 1000u)

/* Reset causes, mapped by the port from the chip's reset status. */
enum { OZ_RST_POWER = 0, OZ_RST_WDT = 1, OZ_RST_SOFT = 2, OZ_RST_OTHER = 3 };

typedef struct {
    /* Written by the owning task (last) and by registration; read by the
     * feeding loop. 32-bit stores, so a stale read only delays a feed. */
    volatile uint32_t last[OZ_WDT_MAX_TASKS];
    uint32_t          period[OZ_WDT_MAX_TASKS];
    volatile uint8_t  n;
} oz_wdt;

void oz_wdt_init(oz_wdt *w);
/* Register a task that must check in at least every period_ms (> 0). Counts
 * as checked in at now_ms. Returns its id, or -1 when full or period_ms is 0. */
int  oz_wdt_add(oz_wdt *w, uint32_t period_ms, uint32_t now_ms);
/* From the task's own loop. An unknown id is ignored. */
void oz_wdt_checkin(oz_wdt *w, int id, uint32_t now_ms);
/* 1 = every task checked in within its period: feed the hardware watchdog.
 * 0 = at least one is late: don't. With no tasks registered, 0 (nothing
 * proves the system is alive). If late is not NULL it gets the first late
 * task's id, or -1. */
int  oz_wdt_may_feed(const oz_wdt *w, uint32_t now_ms, int *late);

/* At boot, with the persisted count and the reset cause (OZ_RST_*). Updates
 * *count; *dirty = 1 when it changed and must be persisted. Returns 1 for a
 * SAFE boot, 0 for NORMAL. */
int  oz_bootguard_boot(uint8_t *count, int cause, int *dirty);
/* Now and then while running. Clears *count after OZ_BOOTGUARD_STABLE_MS of
 * uptime in NORMAL mode; returns 1 when it did (persist it), else 0. */
int  oz_bootguard_stable(uint8_t *count, uint32_t uptime_ms, int safe);

#endif
