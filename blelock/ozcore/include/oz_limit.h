/* oz_limit — the wrong-guess limit for credentials typed or shown at the door
 * (ozkey-49 P5-B; policy: PM nexus-a8, 2026-10-29). One per credential kind:
 * keypad PINs now, RFID cards in P5-E.
 *
 *   - 5 misses in a row lock it out for 60 s.
 *   - After that, every further miss locks it out again, each time twice as
 *     long (2, 4, 8 … min), capped at 30 min.
 *   - Only a success resets it: a right credential, or an owner unlock from
 *     the app.
 *   - While locked out nothing is checked at all; the caller beeps and drops
 *     the attempt, and the attempt is not a miss.
 *
 * Portable and pure: time comes in as a monotonic ms value, and the caller
 * persists the 8-byte form (oz_limit_save) after every change, so pulling the
 * battery doesn't clear a lockout. A monotonic clock restarts at boot, so a
 * lockout is saved as the time it still has to run and restarts in full from
 * the next boot: a reboot can only lengthen it, never shorten it.
 */
#ifndef OZ_LIMIT_H
#define OZ_LIMIT_H

#include <stddef.h>
#include <stdint.h>

#define OZ_LIMIT_FREE_MISSES 5
#define OZ_LIMIT_FIRST_MS    60000u
#define OZ_LIMIT_MAX_MS      (30u * 60u * 1000u)
#define OZ_LIMIT_SAVED_LEN   8

typedef struct {
    uint8_t  misses;        /* in a row, before the first lockout */
    uint8_t  level;         /* lockouts since the last success, 0 = none */
    uint64_t until_ms;      /* locked out while now < until_ms */
} oz_limit;

void oz_limit_init(oz_limit *l);
/* 1 while locked out: check nothing. */
int  oz_limit_locked(const oz_limit *l, uint64_t now_ms);
/* Time left in the lockout, 0 when not locked out. */
uint64_t oz_limit_remaining_ms(const oz_limit *l, uint64_t now_ms);
/* Record a miss. Returns the lockout it started, in ms, or 0. */
uint32_t oz_limit_miss(oz_limit *l, uint64_t now_ms);
/* A success. Returns 1 if that changed anything (so the caller saves). */
int  oz_limit_success(oz_limit *l);

/* Persisted form: version ‖ misses ‖ level ‖ 0 ‖ remaining ms (u32 BE). */
void oz_limit_save(const oz_limit *l, uint64_t now_ms, uint8_t out[OZ_LIMIT_SAVED_LEN]);
/* Restore at boot. A malformed record is treated as a fresh lockout at the
 * cap (fail closed), never as no limit. Returns 0 or OZ_E_ARG (malformed). */
int  oz_limit_load(oz_limit *l, const uint8_t *in, size_t len, uint64_t now_ms);

#endif
