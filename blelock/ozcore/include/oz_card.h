/* oz_card — one tap per card at the RFID reader (ozkey-49 P5-E).
 *
 * The reader reports a card every poll while it stays in the field. Without
 * this filter one wrong card left on the reader would be five misses in under
 * a second, and a lockout. A read of the same identity within
 * OZ_CARD_HOLD_MS of the last read of it is the same tap (and keeps the tap
 * alive); anything else is a new tap for oz_lock_card(). Portable and pure:
 * the port calls it on the OZKIE thread with a millisecond clock.
 */
#ifndef OZ_CARD_H
#define OZ_CARD_H

#include <stddef.h>
#include <stdint.h>

#define OZ_CARD_HOLD_MS 1500u
#define OZ_CARD_ID_MAX  16      /* = OZ_LOCK_CRED_SECRET */

typedef struct {
    uint8_t  id[OZ_CARD_ID_MAX];
    uint8_t  len;               /* 0 = none seen */
    uint32_t last_ms;
} oz_card;

void oz_card_init(oz_card *c);
/* 1: a new tap, pass it to the lock; 0: the same card still held, or an
 * identity of 0 or more than OZ_CARD_ID_MAX bytes (dropped). */
int  oz_card_seen(oz_card *c, const uint8_t *id, size_t len, uint32_t now_ms);

#endif
