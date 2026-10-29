/* oz_keypad — the door keypad's entry state machine (ozkey-49 P5-B).
 *
 * Keys: '0'..'9', '*' clears the entry, '#' enters it. Portable and pure: the
 * port feeds it keys and a millisecond clock from the OZKIE thread, and acts
 * on what it returns. It never checks anything itself:
 *   OZ_KP_PIN        call oz_lock_keypad_pin(L, kp->entry), then
 *                    oz_keypad_done(kp)
 *   OZ_KP_PAIR_CODE  the owner typed the 6-digit pairing code (*code)
 *   OZ_KP_CLEARED    a half-typed entry was dropped ('*', a pause, or a
 *                    pairing entry that isn't 6 digits)
 *   OZ_KP_KEY        a digit was taken (beep)
 *   OZ_KP_NONE       nothing to do
 *
 * An entry keeps up to OZ_LOCK_PIN_ENTRY_MAX + 1 digits, so one too long
 * still reaches the lock, which refuses it and counts it as a miss; further
 * digits are dropped. '#' on an empty entry does nothing. A pause of
 * OZ_KEYPAD_IDLE_MS drops the entry, so the next visitor doesn't inherit it.
 */
#ifndef OZ_KEYPAD_H
#define OZ_KEYPAD_H

#include <stdint.h>

#include "oz_lock.h"

#define OZ_KEYPAD_IDLE_MS 10000u
#define OZ_KEYPAD_PAIR_DIGITS 6

enum { OZ_KP_NONE = 0, OZ_KP_KEY, OZ_KP_CLEARED, OZ_KP_PIN, OZ_KP_PAIR_CODE };

typedef struct {
    char     entry[OZ_LOCK_PIN_ENTRY_MAX + 2];  /* digits, NUL-terminated */
    uint8_t  len;
    uint8_t  pairing;       /* the lock asked for the pairing code */
    uint32_t last_ms;
} oz_keypad;

void oz_keypad_init(oz_keypad *kp);
/* The lock's pair_prompt hook: entries go to the pairing code while on. */
void oz_keypad_pairing(oz_keypad *kp, int on);
int  oz_keypad_key(oz_keypad *kp, char key, uint32_t now_ms, uint32_t *code);
/* Call periodically: OZ_KP_CLEARED if an idle entry was dropped. */
int  oz_keypad_tick(oz_keypad *kp, uint32_t now_ms);
/* After OZ_KP_PIN: wipe the entry. */
void oz_keypad_done(oz_keypad *kp);

#endif
