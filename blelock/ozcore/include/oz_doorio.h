/* oz_doorio — the door's own inputs: tamper switch, door sensor, inside exit
 * button (LOCK-IO-1, operator + PM nexus-46 2026-11-19). Portable and pure: the
 * port samples the three switches every pass and acts on what step() returns.
 *
 * Safe-exit rules (PM, hard): the exit button ALWAYS asks for an unlock, also
 * during a keypad/card lockout and a tamper alarm (this module never looks at
 * lockouts, and tamper never masks the exit); auto-relock never fires while the
 * door reads open.
 *
 * Every policy number is in oz_doorio_cfg (ONE place). OZ_DOORIO_BENCH holds
 * the bench defaults; the product values are the operator's settings, later.
 */
#ifndef OZ_DOORIO_H
#define OZ_DOORIO_H

#include <stdint.h>

typedef struct {
    uint32_t debounce_ms;       /* a level must hold this long to count */
    uint32_t relock_ms;         /* door closed again after an unlock: relock after */
    uint32_t unopened_relock_ms;/* unlocked but the door never opened: relock after */
    uint32_t left_open_ms;      /* door open this long: one warning */
    uint32_t alarm_ms;          /* a tamper sounds the alarm this long */
    uint8_t  door_open_when_pressed;  /* bench: a momentary switch, pressed = open */
    uint8_t  tamper_when_pressed;     /* bench: pressing the switch = tamper */
} oz_doorio_cfg;

/* Bench defaults (3 momentary switches, operator 2026-11-19). */
#define OZ_DOORIO_BENCH { 50u, 5000u, 15000u, 30000u, 10000u, 1u, 1u }

/* step() results, a bit mask. */
enum {
    OZ_DIO_EXIT_UNLOCK = 1,     /* the exit button: unlock now (no credential) */
    OZ_DIO_RELOCK      = 2,     /* auto-relock now */
    OZ_DIO_TAMPER      = 4,     /* a tamper started: record it, sound the alarm */
    OZ_DIO_LEFT_OPEN   = 8,     /* the door has been open left_open_ms */
    OZ_DIO_DOOR_OPENED = 16,    /* the door opened (confirms an unlock) */
    OZ_DIO_DOOR_CLOSED = 32,
    OZ_DIO_ALARM_END   = 64     /* the tamper alarm time is over */
};

/* Raw switch levels as read: 1 = pressed (pin LOW with the pull-up). */
enum { OZ_DIO_SW_TAMPER = 0, OZ_DIO_SW_DOOR, OZ_DIO_SW_EXIT, OZ_DIO_SW_N };

typedef struct {
    oz_doorio_cfg cfg;
    uint8_t  stable[OZ_DIO_SW_N];   /* debounced pressed state */
    uint8_t  raw[OZ_DIO_SW_N];
    uint32_t raw_since[OZ_DIO_SW_N];
    uint8_t  started;
    uint8_t  unlocked;          /* the lock was told to unlock and not relocked */
    uint8_t  opened_since_unlock;
    uint8_t  warned;            /* left-open warning given for this opening */
    uint8_t  alarm;             /* the tamper alarm is sounding */
    uint32_t unlocked_at, closed_at, opened_at, alarm_until;
} oz_doorio;

void oz_doorio_init(oz_doorio *d, const oz_doorio_cfg *cfg);
/* The lock unlocked / locked (any path: app, PIN, card, exit, relock). */
void oz_doorio_unlocked(oz_doorio *d, uint32_t now_ms);
void oz_doorio_locked(oz_doorio *d);
/* One pass: the three raw levels (1 = pressed). Returns OZ_DIO_* bits. */
int  oz_doorio_step(oz_doorio *d, const uint8_t pressed[OZ_DIO_SW_N], uint32_t now_ms);
int  oz_doorio_door_open(const oz_doorio *d);
int  oz_doorio_alarm(const oz_doorio *d);

#endif
