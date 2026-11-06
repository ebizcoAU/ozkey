/* oz_motor — the bolt motor's state machine (ozkey-49 P5-C, built ahead of
 * the hardware spec, OZ-7, against a simulated motor).
 *
 * Portable and pure: the port calls oz_motor_step() from the OZKIE thread
 * with a millisecond clock and whatever it can sense, drives the motor with
 * m->out, and hands every finished move to oz_lock_actuated().
 *
 *   IDLE ──command──▶ DRIVING ──done──▶ BRAKING ──▶ RESTING ──▶ IDLE
 *
 *   - With a bolt sensor: the move is OZ_MOVE_OK when the sensor reads the
 *     target position, OZ_MOVE_JAMMED on a stall or after timeout_ms.
 *   - Without one: the motor runs for drive_ms and the move is
 *     OZ_MOVE_UNCONFIRMED (or OZ_MOVE_JAMMED on a stall, if the driver can
 *     sense one). Nothing claims a position it didn't see.
 *   - After every move: brake_ms of braking, then rest_ms with the motor off
 *     before the next move may start (thermal, OZ-7 1d).
 *   - A command while busy is kept (the latest wins) and starts after the
 *     rest. A command for where the motor is already driving is dropped.
 *   - On a jam: report only, no retry and no reverse, until OZ-7 6a rules.
 *
 * The output is OZ_MOTOR_OFF at init and whenever no move runs, so both
 * driver inputs stay off until a command (OZ-7 1a).
 */
#ifndef OZ_MOTOR_H
#define OZ_MOTOR_H

#include <stdint.h>

/* What to drive (m->out). */
enum { OZ_MOTOR_OFF = 0, OZ_MOTOR_TO_UNLOCK, OZ_MOTOR_TO_LOCK, OZ_MOTOR_BRAKE };
/* What the bolt sensor reads; OZ_BOLT_NONE when there is no sensor. */
enum { OZ_BOLT_NONE = 0, OZ_BOLT_BETWEEN, OZ_BOLT_THROWN, OZ_BOLT_RETRACTED };
/* How a move ended. */
enum { OZ_MOVE_NONE = 0, OZ_MOVE_OK, OZ_MOVE_UNCONFIRMED, OZ_MOVE_JAMMED };
enum { OZ_MOTOR_IDLE = 0, OZ_MOTOR_DRIVING, OZ_MOTOR_BRAKING, OZ_MOTOR_RESTING };

/* Placeholders until OZ-7 1b/1d give the real motor's numbers. */
#define OZ_MOTOR_DRIVE_MS   400u
#define OZ_MOTOR_TIMEOUT_MS 1500u
#define OZ_MOTOR_BRAKE_MS   50u
#define OZ_MOTOR_REST_MS    300u

typedef struct {
    uint16_t drive_ms;      /* no sensor: how long one move runs */
    uint16_t timeout_ms;    /* with a sensor: give up after this */
    uint16_t brake_ms;
    uint16_t rest_ms;
    uint8_t  has_sensor;
} oz_motor_cfg;

typedef struct {
    oz_motor_cfg cfg;
    uint8_t  phase;         /* OZ_MOTOR_IDLE … */
    uint8_t  out;           /* drive this now */
    uint8_t  unlock;        /* direction of the current move */
    uint8_t  pending;       /* 0 none, else 1 + unlock */
    uint32_t since_ms;      /* phase start */
} oz_motor;

/* cfg NULL: the placeholders, no sensor. */
void oz_motor_init(oz_motor *m, const oz_motor_cfg *cfg);
/* Ask for a move. Starts it at once when idle (m->out changes). */
void oz_motor_command(oz_motor *m, int unlock, uint32_t now_ms);
/* Call often while m->phase != OZ_MOTOR_IDLE (every ≤ 10 ms while driving).
 * bolt: OZ_BOLT_* (ignored without a sensor); stall: 1 if the driver reports
 * a stall or over-current. Returns OZ_MOVE_* when a move just ended, with
 * *unlock set to its direction; else OZ_MOVE_NONE. */
int  oz_motor_step(oz_motor *m, uint32_t now_ms, uint8_t bolt, int stall, int *unlock);

#endif
