/* oz_wdt — see include/oz_wdt.h. */
#include "oz_wdt.h"

#include <string.h>

void oz_wdt_init(oz_wdt *w)
{
    memset((void *)w, 0, sizeof *w);
}

int oz_wdt_add(oz_wdt *w, uint32_t period_ms, uint32_t now_ms)
{
    if (!w || period_ms == 0 || w->n >= OZ_WDT_MAX_TASKS)
        return -1;
    uint8_t id = w->n;
    w->period[id] = period_ms;
    w->last[id] = now_ms;
    w->n = (uint8_t)(id + 1u);  /* last: the feeding loop never sees a half-set task */
    return id;
}

void oz_wdt_checkin(oz_wdt *w, int id, uint32_t now_ms)
{
    if (w && id >= 0 && id < (int)w->n)
        w->last[id] = now_ms;
}

int oz_wdt_may_feed(const oz_wdt *w, uint32_t now_ms, int *late)
{
    if (late)
        *late = -1;
    if (!w || w->n == 0)
        return 0;
    uint8_t n = w->n;
    for (uint8_t i = 0; i < n; i++) {
        if ((uint32_t)(now_ms - w->last[i]) > w->period[i]) {
            if (late)
                *late = i;
            return 0;
        }
    }
    return 1;
}

int oz_bootguard_boot(uint8_t *count, int cause, int *dirty)
{
    uint8_t before = *count;
    if (cause == OZ_RST_POWER)
        *count = 0;
    else if (cause == OZ_RST_WDT && *count < 0xffu)
        *count = (uint8_t)(*count + 1u);
    if (dirty)
        *dirty = *count != before;
    return *count >= OZ_BOOTGUARD_LIMIT;
}

int oz_bootguard_stable(uint8_t *count, uint32_t uptime_ms, int safe)
{
    if (safe || *count == 0 || uptime_ms < OZ_BOOTGUARD_STABLE_MS)
        return 0;
    *count = 0;
    return 1;
}
