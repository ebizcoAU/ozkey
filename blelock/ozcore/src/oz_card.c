/* oz_card — see oz_card.h. */
#include <string.h>

#include "oz_card.h"

void oz_card_init(oz_card *c) { memset(c, 0, sizeof *c); }

int oz_card_seen(oz_card *c, const uint8_t *id, size_t len, uint32_t now_ms)
{
    if (len == 0 || len > OZ_CARD_ID_MAX)
        return 0;
    int same = c->len == len && !memcmp(c->id, id, len) && now_ms - c->last_ms < OZ_CARD_HOLD_MS;
    memcpy(c->id, id, len);
    c->len = (uint8_t)len;
    c->last_ms = now_ms;
    return !same;
}
