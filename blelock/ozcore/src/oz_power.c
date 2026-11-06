/* oz_power — see oz_power.h. */
#include <string.h>

#include "oz_power.h"

void oz_power_init(oz_power *p)
{
    memset(p, 0, sizeof *p);
    p->level = OZ_BATT_OK;
    p->percent = 100;
}

static uint8_t classify(uint32_t mv)
{
    return mv < OZ_POWER_CRITICAL_MV ? OZ_BATT_CRITICAL : mv < OZ_POWER_LOW_MV ? OZ_BATT_LOW : OZ_BATT_OK;
}

static uint8_t percent_of(uint32_t mv)
{
    if (mv <= OZ_POWER_EMPTY_MV)
        return 0;
    if (mv >= OZ_POWER_FULL_MV)
        return 100;
    return (uint8_t)((mv - OZ_POWER_EMPTY_MV) * 100u / (OZ_POWER_FULL_MV - OZ_POWER_EMPTY_MV));
}

/* The level a rest reading earns, from where we are: worse at once, better
 * only with HYST of margin, and never better than CRITICAL while sagged. */
static uint8_t settle(const oz_power *p, uint16_t mv)
{
    uint8_t now = classify(mv);
    if (p->sagged)
        return OZ_BATT_CRITICAL;
    if (now >= p->level)
        return now;
    uint8_t with_margin = classify(mv > OZ_POWER_HYST_MV ? mv - OZ_POWER_HYST_MV : 0);
    return with_margin < p->level ? with_margin : p->level;
}

int oz_power_rest(oz_power *p, uint16_t mv)
{
    if (mv == 0)
        return 0;
    uint8_t was = p->level;
    p->rest_mv = mv;
    p->percent = percent_of(mv);
    if (p->sagged && mv >= OZ_POWER_FRESH_MV)
        p->sagged = 0;                          /* new batteries */
    p->level = settle(p, mv);
    return p->level != was;
}

int oz_power_load(oz_power *p, uint16_t min_mv)
{
    if (min_mv == 0)
        return 0;
    uint8_t was = p->level;
    if (min_mv < OZ_POWER_SAG_MV) {
        p->sagged = 1;
        p->level = OZ_BATT_CRITICAL;
    } else if (p->sagged) {
        p->sagged = 0;                          /* it drove the motor without sagging */
        if (p->rest_mv)
            p->level = settle(p, p->rest_mv);
    }
    return p->level != was;
}

int oz_power_may_move(const oz_power *p, int unlock)
{
    if (unlock)
        return 1;
    return OZ_POWER_LOCK_AT_CRITICAL || p->level != OZ_BATT_CRITICAL;
}
