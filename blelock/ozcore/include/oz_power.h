/* oz_power — the battery policy (ozkey-49 P5-F, built ahead of OZ-7 5a/5b
 * against a mocked ADC; PM nexus-a8 2026-10-29).
 *
 * Portable and pure. The port feeds it battery readings in mV: at rest (the
 * motor off) now and then, and the lowest reading while the motor drove. It
 * keeps a level (OK, LOW, CRITICAL) and a percentage, and answers whether a
 * bolt move may start.
 *
 *   - 🔴 Never lock anyone out: an UNLOCK move is always allowed, at any
 *     level, with or without readings.
 *   - A LOCK move is refused at CRITICAL (OZ_POWER_LOCK_AT_CRITICAL 0). A
 *     bolt thrown on a battery that may not retract it again is the lock-out.
 *     The cost is a door left unlocked until the battery is changed; that
 *     trade-off is the operator's (OZ-7 5a, with the jam policy 6a).
 *   - Levels get worse at once and better only with OZ_POWER_HYST_MV of
 *     margin, so a reading near a threshold doesn't flap.
 *   - A battery that sagged below OZ_POWER_SAG_MV under motor load is
 *     CRITICAL however well it reads at rest (a weak cell recovers at rest and
 *     sags again on the next move), until a later move stays above
 *     OZ_POWER_SAG_MV (unlock moves are always allowed, so one can prove it)
 *     or a rest reading looks like fresh cells (OZ_POWER_FRESH_MV).
 *   - No reading yet, or a reading of 0: no opinion; everything is allowed.
 *
 * Every threshold is a placeholder for a 4×AA alkaline pack until OZ-7 gives
 * the real cells, the divider and the brown-out point (5a/5b).
 */
#ifndef OZ_POWER_H
#define OZ_POWER_H

#include <stdint.h>

#define OZ_POWER_FULL_MV     6000u   /* 100 % */
#define OZ_POWER_EMPTY_MV    4400u   /* 0 % */
#define OZ_POWER_LOW_MV      4800u   /* below: LOW (warn, everything works) */
#define OZ_POWER_CRITICAL_MV 4500u   /* below: CRITICAL (no LOCK moves) */
#define OZ_POWER_SAG_MV      4000u   /* under motor load, below: CRITICAL */
#define OZ_POWER_FRESH_MV    5600u   /* at rest after a sag: new batteries */
#define OZ_POWER_HYST_MV     150u

#ifndef OZ_POWER_LOCK_AT_CRITICAL
#define OZ_POWER_LOCK_AT_CRITICAL 0
#endif

enum { OZ_BATT_OK = 0, OZ_BATT_LOW = 1, OZ_BATT_CRITICAL = 2 };

typedef struct {
    uint16_t rest_mv;       /* last reading at rest, 0 = none yet */
    uint8_t  level;         /* OZ_BATT_* */
    uint8_t  percent;       /* 100 until the first reading */
    uint8_t  sagged;        /* collapsed under load; see above for what clears it */
} oz_power;

void oz_power_init(oz_power *p);
/* A reading with the motor off. Returns 1 when the level changed. */
int  oz_power_rest(oz_power *p, uint16_t mv);
/* The lowest reading during one motor move. Returns 1 when the level changed. */
int  oz_power_load(oz_power *p, uint16_t min_mv);
/* May a bolt move start now? Unlocking always may. */
int  oz_power_may_move(const oz_power *p, int unlock);

#endif
