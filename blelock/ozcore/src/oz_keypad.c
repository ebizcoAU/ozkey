/* oz_keypad — see oz_keypad.h. */
#include <string.h>

#include "oz_crypto.h"
#include "oz_keypad.h"

void oz_keypad_init(oz_keypad *kp) { memset(kp, 0, sizeof *kp); }

void oz_keypad_done(oz_keypad *kp)
{
    oz_wipe(kp->entry, sizeof kp->entry);
    kp->len = 0;
}

void oz_keypad_pairing(oz_keypad *kp, int on)
{
    kp->pairing = on != 0;
    oz_keypad_done(kp);                 /* a PIN half-typed before is not a code */
}

int oz_keypad_tick(oz_keypad *kp, uint32_t now_ms)
{
    if (kp->len && now_ms - kp->last_ms >= OZ_KEYPAD_IDLE_MS) {
        oz_keypad_done(kp);
        return OZ_KP_CLEARED;
    }
    return OZ_KP_NONE;
}

int oz_keypad_key(oz_keypad *kp, char key, uint32_t now_ms, uint32_t *code)
{
    int dropped = oz_keypad_tick(kp, now_ms) == OZ_KP_CLEARED;
    kp->last_ms = now_ms;
    if (key >= '0' && key <= '9') {
        if (kp->len < sizeof kp->entry - 1) {
            kp->entry[kp->len++] = key;
            kp->entry[kp->len] = 0;
        }
        return OZ_KP_KEY;
    }
    if (key == '*') {
        int had = kp->len != 0;
        oz_keypad_done(kp);
        return had || dropped ? OZ_KP_CLEARED : OZ_KP_NONE;
    }
    if (key != '#' || kp->len == 0)
        return dropped ? OZ_KP_CLEARED : OZ_KP_NONE;
    if (!kp->pairing)
        return OZ_KP_PIN;
    if (kp->len != OZ_KEYPAD_PAIR_DIGITS) {
        oz_keypad_done(kp);
        return OZ_KP_CLEARED;
    }
    uint32_t v = 0;
    for (int i = 0; i < OZ_KEYPAD_PAIR_DIGITS; i++)
        v = v * 10u + (uint32_t)(kp->entry[i] - '0');
    oz_keypad_done(kp);
    *code = v;
    return OZ_KP_PAIR_CODE;
}
