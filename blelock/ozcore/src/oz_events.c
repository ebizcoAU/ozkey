/* oz_events — see oz_events.h. */
#include <string.h>

#include "oz_events.h"

void oz_events_init(oz_events *e, uint32_t next_seq)
{
    memset(e, 0, sizeof *e);
    e->next_seq = next_seq;
}

uint32_t oz_events_oldest(const oz_events *e) { return e->next_seq - e->count; }

uint32_t oz_events_add(oz_events *e, uint8_t type, uint8_t actor, int64_t utc, uint32_t a, uint32_t b)
{
    oz_event *ev = &e->ring[e->next_seq % OZ_EVENTS_RING];
    ev->seq = e->next_seq;
    ev->type = type;
    ev->actor = actor;
    ev->utc = utc;
    ev->a = a;
    ev->b = b;
    if (e->count < OZ_EVENTS_RING)
        e->count++;
    return e->next_seq++;
}

int oz_events_next(const oz_events *e, uint32_t *cursor, oz_event *ev)
{
    uint32_t oldest = oz_events_oldest(e);
    /* Distances modulo 2^32: behind = how far the cursor is behind next_seq. */
    uint32_t behind = e->next_seq - *cursor;
    if (behind == 0 || behind > UINT32_MAX / 2)
        return 0;                       /* up to date, or ahead of us */
    if (behind > e->count) {
        memset(ev, 0, sizeof *ev);
        ev->type = OZ_EV_GAP;
        ev->actor = OZ_EV_NO_ACTOR;
        ev->seq = *cursor;
        ev->a = oldest - *cursor;
        *cursor = oldest;
        return 1;
    }
    *ev = e->ring[*cursor % OZ_EVENTS_RING];
    (*cursor)++;
    return 1;
}
