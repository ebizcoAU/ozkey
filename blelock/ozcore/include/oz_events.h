/* oz_events — the lock's event ring (ozkey-48 §5.1, ozkey-49 P5-E0; PM
 * nexus-a8 2026-10-29: admin first, built so the service bond's audit trail
 * slots in later without a wire change).
 *
 * Portable and pure. Every event gets the next sequence number. The ring holds
 * the last OZ_EVENTS_RING of them in RAM; when it is full the oldest is
 * dropped. A reader keeps a cursor (the next sequence number it wants) and
 * takes events one at a time with oz_events_next(). If its cursor points
 * before the oldest event still held, it first gets one OZ_EV_GAP saying
 * how many it missed, then carries on from the oldest held. So a slow or absent
 * reader loses the oldest events, never the newest, and always knows it did.
 *
 * Sequence numbers are 32-bit and compared modulo 2^32. The engine persists a
 * reserved upper bound (oz_lock.c), so they never repeat across reboots; a
 * reboot loses the RAM ring and shows as a jump.
 */
#ifndef OZ_EVENTS_H
#define OZ_EVENTS_H

#include <stdint.h>

#define OZ_EVENTS_RING 16       /* 16 × 24 B = 384 B of RAM */

/* Event types. The ozkey-48 §5.1 list (access, alarm, duress, battery,
 * doorbell, bolt) takes the next values as each is built. */
enum { OZ_EV_GAP = 0, OZ_EV_KEYPAD_LOCKOUT = 1, OZ_EV_CARD_LOCKOUT = 2, OZ_EV_BATTERY = 3,
       OZ_EV_SAFE_MODE = 4 };

#define OZ_EV_NO_ACTOR 0xff     /* not done by a bond (keypad, sensors) */

typedef struct {
    uint32_t seq;
    uint8_t  type;
    uint8_t  actor;             /* bond slot, or OZ_EV_NO_ACTOR */
    int64_t  utc;               /* unix s; 0 = the clock was not set */
    uint32_t a, b;              /* per type: lockouts: level, seconds; battery: level, percent; gap: lost count */
} oz_event;

typedef struct {
    oz_event ring[OZ_EVENTS_RING];
    uint32_t next_seq;          /* what the next event gets */
    uint8_t  count;             /* held, ≤ OZ_EVENTS_RING */
} oz_events;

void     oz_events_init(oz_events *e, uint32_t next_seq);
/* Append; returns its sequence number. */
uint32_t oz_events_add(oz_events *e, uint8_t type, uint8_t actor, int64_t utc, uint32_t a, uint32_t b);
/* The oldest sequence number held (= next_seq when empty). */
uint32_t oz_events_oldest(const oz_events *e);
/* The next item for a reader at *cursor: 1 and *ev (an event, or an
 * OZ_EV_GAP with seq = the first missed and a = how many) and the cursor
 * moved on; 0 when it is up to date. A cursor ahead of next_seq (from before
 * a reboot, or invented) is treated as up to date. */
int      oz_events_next(const oz_events *e, uint32_t *cursor, oz_event *ev);

#endif
