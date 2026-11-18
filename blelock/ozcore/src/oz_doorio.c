/* oz_doorio — see oz_doorio.h. */
#include <string.h>

#include "oz_doorio.h"

void oz_doorio_init(oz_doorio *d, const oz_doorio_cfg *cfg)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
}

void oz_doorio_unlocked(oz_doorio *d, uint32_t now_ms)
{
    d->unlocked = 1;
    d->unlocked_at = now_ms;
    d->opened_since_unlock = (uint8_t)(oz_doorio_door_open(d) != 0);
    d->closed_at = now_ms;
}

void oz_doorio_locked(oz_doorio *d)
{
    d->unlocked = 0;
}

int oz_doorio_door_open(const oz_doorio *d)
{
    uint8_t p = d->stable[OZ_DIO_SW_DOOR];
    return d->cfg.door_open_when_pressed ? p : !p;
}

int oz_doorio_alarm(const oz_doorio *d)
{
    return d->alarm;
}

static int tampered(const oz_doorio *d)
{
    uint8_t p = d->stable[OZ_DIO_SW_TAMPER];
    return d->cfg.tamper_when_pressed ? p : !p;
}

int oz_doorio_step(oz_doorio *d, const uint8_t pressed[OZ_DIO_SW_N], uint32_t now)
{
    int out = 0;
    uint8_t was[OZ_DIO_SW_N];
    memcpy(was, d->stable, sizeof was);

    /* Debounce: a new raw level must hold debounce_ms. The first pass takes
     * the levels as they are, without edges (the door may be open at boot). */
    for (int i = 0; i < OZ_DIO_SW_N; i++) {
        uint8_t p = (uint8_t)(pressed[i] != 0);
        if (!d->started) {
            d->raw[i] = d->stable[i] = p;
            d->raw_since[i] = now;
            continue;
        }
        if (p != d->raw[i]) {
            d->raw[i] = p;
            d->raw_since[i] = now;
        } else if (p != d->stable[i] && now - d->raw_since[i] >= d->cfg.debounce_ms) {
            d->stable[i] = p;
        }
    }
    if (!d->started) {
        d->started = 1;
        if (oz_doorio_door_open(d))
            d->opened_at = now;
        return 0;
    }

    /* Exit: on the press, always (no lockout, tamper or alarm check: PM). */
    if (d->stable[OZ_DIO_SW_EXIT] && !was[OZ_DIO_SW_EXIT])
        out |= OZ_DIO_EXIT_UNLOCK;

    /* Tamper: on the edge into "tampered". */
    {
        int prev = d->cfg.tamper_when_pressed ? was[OZ_DIO_SW_TAMPER] : !was[OZ_DIO_SW_TAMPER];
        if (tampered(d) && !prev) {
            out |= OZ_DIO_TAMPER;
            d->alarm = 1;
            d->alarm_until = now + d->cfg.alarm_ms;
        }
    }
    if (d->alarm && (int32_t)(now - d->alarm_until) >= 0) {
        d->alarm = 0;
        out |= OZ_DIO_ALARM_END;
    }

    /* Door. */
    {
        int prev = d->cfg.door_open_when_pressed ? was[OZ_DIO_SW_DOOR] : !was[OZ_DIO_SW_DOOR];
        int open = oz_doorio_door_open(d);
        if (open && !prev) {
            out |= OZ_DIO_DOOR_OPENED;
            d->opened_at = now;
            d->warned = 0;
            if (d->unlocked)
                d->opened_since_unlock = 1;
        } else if (!open && prev) {
            out |= OZ_DIO_DOOR_CLOSED;
            d->closed_at = now;
        }
        if (open && !d->warned && now - d->opened_at >= d->cfg.left_open_ms) {
            d->warned = 1;
            out |= OZ_DIO_LEFT_OPEN;
        }
        /* Auto-relock, never while the door reads open (PM). */
        if (d->unlocked && !open) {
            uint32_t wait = d->opened_since_unlock ? d->cfg.relock_ms : d->cfg.unopened_relock_ms;
            uint32_t from = d->opened_since_unlock ? d->closed_at : d->unlocked_at;
            if (now - from >= wait) {
                out |= OZ_DIO_RELOCK;
                d->unlocked = 0;        /* asked once; locked() confirms */
            }
        }
    }
    return out;
}
