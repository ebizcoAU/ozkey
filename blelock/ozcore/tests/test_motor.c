/* test_motor — oz_motor (the bolt motor's state machine, ozkey-49 P5-C)
 * against a simulated bolt, and oz_lock_actuated (what the lock state does
 * with a finished move). The real motor's numbers wait on OZ-7. */
#include <string.h>

#include "oz_test.h"
#include "oz_motor.h"
#include "oz_lock.h"

/* The simulated bolt: 0 = thrown (locked) … 100 = retracted, moving one step
 * every `ms_per_step` ms of drive. It sticks at `jam_at` (−1 = never), and the
 * driver reports a stall there if `stall_sense`. */
typedef struct {
    int pos, ms_per_step, jam_at, stall_sense, sensor;
    int acc;
    uint32_t driven_ms;     /* longest single drive, ms */
    uint32_t run_ms;
} bolt_sim;

static uint8_t bolt_read(const bolt_sim *b)
{
    if (!b->sensor)
        return OZ_BOLT_NONE;
    return b->pos <= 0 ? OZ_BOLT_THROWN : b->pos >= 100 ? OZ_BOLT_RETRACTED : OZ_BOLT_BETWEEN;
}

static void bolt_ms(bolt_sim *b, uint8_t out)
{
    if (out != OZ_MOTOR_TO_UNLOCK && out != OZ_MOTOR_TO_LOCK) {
        b->run_ms = 0;
        return;
    }
    if (++b->run_ms > b->driven_ms)
        b->driven_ms = b->run_ms;
    if (b->pos == b->jam_at || ++b->acc < b->ms_per_step)
        return;
    b->acc = 0;
    if (out == OZ_MOTOR_TO_UNLOCK && b->pos < 100)
        b->pos++;
    else if (out == OZ_MOTOR_TO_LOCK && b->pos > 0)
        b->pos--;
}

typedef struct {
    int result[8], unlock[8], n;
    uint32_t at[8];
    int out_bad;            /* drive on outside DRIVING */
} log_t;

/* Run ms by ms for `ms`, stepping the motor each ms like the OZKIE thread. */
static void run(oz_motor *m, bolt_sim *b, uint32_t *now, uint32_t ms, log_t *lg)
{
    for (uint32_t i = 0; i < ms; i++) {
        int u = -1;
        int stall = b->stall_sense && b->pos == b->jam_at &&
                    (m->out == OZ_MOTOR_TO_UNLOCK || m->out == OZ_MOTOR_TO_LOCK);
        int r = oz_motor_step(m, *now, bolt_read(b), stall, &u);
        if (r != OZ_MOVE_NONE && lg->n < 8) {
            lg->result[lg->n] = r;
            lg->unlock[lg->n] = u;
            lg->at[lg->n++] = *now;
        }
        if ((m->out == OZ_MOTOR_TO_UNLOCK || m->out == OZ_MOTOR_TO_LOCK) != (m->phase == OZ_MOTOR_DRIVING))
            lg->out_bad++;
        bolt_ms(b, m->out);
        (*now)++;
    }
}

static void no_sensor(void)
{
    oz_motor m;
    bolt_sim b = { 0, 3, -1, 0, 0, 0, 0, 0 };
    log_t lg = { {0}, {0}, 0, {0}, 0 };
    uint32_t now = 0xfffffe00u;         /* the ms clock wraps mid-move */
    oz_motor_init(&m, NULL);
    expect_int("off at init", m.out, OZ_MOTOR_OFF);
    expect_int("idle at init", m.phase, OZ_MOTOR_IDLE);
    run(&m, &b, &now, 50, &lg);
    expect_int("idle stays off", m.out, OZ_MOTOR_OFF);

    uint32_t t0 = now;
    oz_motor_command(&m, 1, now);
    expect_int("command drives at once", m.out, OZ_MOTOR_TO_UNLOCK);
    run(&m, &b, &now, OZ_MOTOR_DRIVE_MS, &lg);
    expect_int("still driving at drive_ms - 1", lg.n, 0);
    run(&m, &b, &now, 1, &lg);
    expect_int("one move", lg.n, 1);
    expect_int("…unconfirmed: nothing sensed it", lg.result[0], OZ_MOVE_UNCONFIRMED);
    expect_int("…unlocking", lg.unlock[0], 1);
    expect_int("…after drive_ms, across the wrap", (long long)(lg.at[0] - t0), OZ_MOTOR_DRIVE_MS);
    expect_int("drove drive_ms, no more", (long long)b.driven_ms, OZ_MOTOR_DRIVE_MS);
    expect_int("brakes", m.out, OZ_MOTOR_BRAKE);
    run(&m, &b, &now, OZ_MOTOR_BRAKE_MS, &lg);
    expect_int("then off", m.out, OZ_MOTOR_OFF);
    expect_int("resting", m.phase, OZ_MOTOR_RESTING);
    run(&m, &b, &now, OZ_MOTOR_REST_MS, &lg);
    expect_int("then idle", m.phase, OZ_MOTOR_IDLE);
    expect_int("drive only while driving", lg.out_bad, 0);

    /* With no sensor a position reading is ignored: it still runs drive_ms. */
    oz_motor_init(&m, NULL);
    lg.n = 0;
    b.sensor = 1;
    b.pos = 0;
    oz_motor_command(&m, 0, now);       /* lock, and the bolt already reads thrown */
    run(&m, &b, &now, OZ_MOTOR_DRIVE_MS + 1, &lg);
    expect_int("no sensor: a reading doesn't end it early", (long long)(lg.n == 1 ? lg.result[0] : -1),
               OZ_MOVE_UNCONFIRMED);
}

static const oz_motor_cfg SENSED = { OZ_MOTOR_DRIVE_MS, OZ_MOTOR_TIMEOUT_MS,
                                     OZ_MOTOR_BRAKE_MS, OZ_MOTOR_REST_MS, 1 };

static void sensor(void)
{
    oz_motor m;
    bolt_sim b = { 0, 3, -1, 0, 1, 0, 0, 0 };   /* 300 ms end to end */
    log_t lg = { {0}, {0}, 0, {0}, 0 };
    uint32_t now = 1000;
    oz_motor_init(&m, &SENSED);

    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 1000, &lg);
    expect_int("ok when the sensor sees it", lg.n == 1 ? lg.result[0] : -1, OZ_MOVE_OK);
    expect_int("…retracted", b.pos, 100);
    expect_int("…stopped there, ~300 ms", b.driven_ms >= 299 && b.driven_ms <= 302, 1);

    /* Already there: ends on the first step, barely driven. */
    lg.n = 0;
    b.driven_ms = 0;
    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 500, &lg);
    expect_int("already retracted: ok", lg.n == 1 ? lg.result[0] : -1, OZ_MOVE_OK);
    expect_int("…without driving", (long long)b.driven_ms, 0);

    /* Stuck halfway, no stall sense: the timeout calls it. */
    lg.n = 0;
    b.driven_ms = 0;
    b.jam_at = 50;
    oz_motor_command(&m, 0, now);
    run(&m, &b, &now, 3000, &lg);
    expect_int("stuck: jammed", lg.n == 1 ? lg.result[0] : -1, OZ_MOVE_JAMMED);
    expect_int("…while locking", lg.unlock[0], 0);
    expect_int("…after timeout_ms, no longer", (long long)b.driven_ms, OZ_MOTOR_TIMEOUT_MS);
    expect_int("no retry, no reverse: stays put", b.pos, 50);
    expect_int("…and off", m.out, OZ_MOTOR_OFF);

    /* Stuck with stall sense: stops at once. */
    lg.n = 0;
    b.driven_ms = 0;
    b.stall_sense = 1;
    b.pos = 0;
    b.jam_at = 20;
    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 3000, &lg);
    expect_int("stall: jammed", lg.n == 1 ? lg.result[0] : -1, OZ_MOVE_JAMMED);
    expect_int("…stopped right away", b.driven_ms <= 62, 1);
    expect_int("drive only while driving", lg.out_bad, 0);

    /* A stall stops a sensorless move too. */
    oz_motor_init(&m, NULL);
    lg.n = 0;
    b.sensor = 0;
    b.pos = 0;
    b.driven_ms = 0;
    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 1000, &lg);
    expect_int("no sensor, stall: jammed", lg.n == 1 ? lg.result[0] : -1, OZ_MOVE_JAMMED);
    expect_int("…early", b.driven_ms < OZ_MOTOR_DRIVE_MS, 1);
}

static void busy(void)
{
    oz_motor m;
    bolt_sim b = { 0, 3, -1, 0, 1, 0, 0, 0 };
    log_t lg = { {0}, {0}, 0, {0}, 0 };
    uint32_t now = 0;
    oz_motor_init(&m, &SENSED);

    /* Unlock, then lock while it moves, then unlock again: the latest wins,
     * and it's the move already under way, so the lock is dropped. */
    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 100, &lg);
    oz_motor_command(&m, 0, now);
    expect_int("queued, not reversed mid-move", m.out, OZ_MOTOR_TO_UNLOCK);
    oz_motor_command(&m, 1, now);
    run(&m, &b, &now, 3000, &lg);
    expect_int("one move only", lg.n, 1);
    expect_int("…the unlock", lg.unlock[0], 1);

    /* Lock during the rest: waits for brake + rest, then runs. */
    lg.n = 0;
    oz_motor_command(&m, 0, now);
    run(&m, &b, &now, 200, &lg);        /* well into the move */
    expect_int("lock running", m.out, OZ_MOTOR_TO_LOCK);
    run(&m, &b, &now, 200, &lg);        /* done (~300 ms), now braking/resting */
    uint32_t done = lg.at[0];
    oz_motor_command(&m, 1, now);
    expect_int("not during the rest", m.out == OZ_MOTOR_TO_UNLOCK, 0);
    run(&m, &b, &now, 2000, &lg);
    expect_int("two moves", lg.n, 2);
    expect_int("…lock then unlock", lg.unlock[0] == 0 && lg.unlock[1] == 1, 1);
    expect_int("…the second after brake + rest",
               (long long)(lg.at[1] - done) >= OZ_MOTOR_BRAKE_MS + OZ_MOTOR_REST_MS, 1);
    expect_int("drive only while driving", lg.out_bad, 0);
}

static void lock_state(void)
{
    static oz_lock L;
    memset(&L, 0, sizeof L);
    L.lock_state = OZ_LOCKSTATE_UNLOCKED;   /* the actuate hook set the command */
    oz_lock_actuated(&L, 1, OZ_MOVE_JAMMED);
    expect_int("jam: not fully locked", L.lock_state, OZ_LOCKSTATE_NOT_FULLY_LOCKED);
    expect_int("…recorded", L.last_move, OZ_MOVE_JAMMED);
    oz_lock_actuated(&L, 0, OZ_MOVE_UNCONFIRMED);
    expect_int("unconfirmed lock: locked", L.lock_state, OZ_LOCKSTATE_LOCKED);
    expect_int("…recorded", L.last_move, OZ_MOVE_UNCONFIRMED);
    oz_lock_actuated(&L, 1, OZ_MOVE_OK);
    expect_int("ok unlock: unlocked", L.lock_state, OZ_LOCKSTATE_UNLOCKED);
    oz_lock_actuated(&L, 0, 99);
    expect_int("unknown result: state kept", L.lock_state, OZ_LOCKSTATE_UNLOCKED);
}

int main(void)
{
    no_sensor();
    sensor();
    busy();
    lock_state();
    return report("motor");
}
