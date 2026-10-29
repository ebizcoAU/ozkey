/* oz_limit — see oz_limit.h. */
#include <string.h>

#include "oz_limit.h"
#include "oz_port.h"

#define SAVED_VERSION 1

void oz_limit_init(oz_limit *l) { memset(l, 0, sizeof *l); }

int oz_limit_locked(const oz_limit *l, uint64_t now_ms) { return now_ms < l->until_ms; }

uint64_t oz_limit_remaining_ms(const oz_limit *l, uint64_t now_ms)
{
    return now_ms < l->until_ms ? l->until_ms - now_ms : 0;
}

/* Lockout number `level` (1-based): 60 s doubled level−1 times, capped. */
static uint32_t lockout_ms(uint8_t level)
{
    uint32_t ms = OZ_LIMIT_FIRST_MS;
    for (uint8_t i = 1; i < level && ms < OZ_LIMIT_MAX_MS; i++)
        ms *= 2;
    return ms < OZ_LIMIT_MAX_MS ? ms : OZ_LIMIT_MAX_MS;
}

uint32_t oz_limit_miss(oz_limit *l, uint64_t now_ms)
{
    if (oz_limit_locked(l, now_ms))
        return 0;                       /* the caller should not have checked */
    if (l->level == 0 && ++l->misses < OZ_LIMIT_FREE_MISSES)
        return 0;
    l->misses = 0;
    if (l->level < UINT8_MAX)
        l->level++;
    uint32_t ms = lockout_ms(l->level);
    l->until_ms = now_ms + ms;
    return ms;
}

int oz_limit_success(oz_limit *l)
{
    int changed = l->misses || l->level || l->until_ms;
    oz_limit_init(l);
    return changed;
}

void oz_limit_save(const oz_limit *l, uint64_t now_ms, uint8_t out[OZ_LIMIT_SAVED_LEN])
{
    uint64_t left = oz_limit_remaining_ms(l, now_ms);
    uint32_t r = left > OZ_LIMIT_MAX_MS ? OZ_LIMIT_MAX_MS : (uint32_t)left;
    out[0] = SAVED_VERSION;
    out[1] = l->misses;
    out[2] = l->level;
    out[3] = 0;
    out[4] = (uint8_t)(r >> 24);
    out[5] = (uint8_t)(r >> 16);
    out[6] = (uint8_t)(r >> 8);
    out[7] = (uint8_t)r;
}

int oz_limit_load(oz_limit *l, const uint8_t *in, size_t len, uint64_t now_ms)
{
    oz_limit_init(l);
    if (len != OZ_LIMIT_SAVED_LEN || in[0] != SAVED_VERSION || in[1] >= OZ_LIMIT_FREE_MISSES) {
        l->level = 6;                   /* lockout_ms(6) is the cap */
        l->until_ms = now_ms + OZ_LIMIT_MAX_MS;
        return OZ_E_ARG;
    }
    uint32_t r = (uint32_t)in[4] << 24 | (uint32_t)in[5] << 16 | (uint32_t)in[6] << 8 | in[7];
    l->misses = in[1];
    l->level = in[2];
    l->until_ms = r ? now_ms + (r > OZ_LIMIT_MAX_MS ? OZ_LIMIT_MAX_MS : r) : 0;
    return OZ_OK;
}
