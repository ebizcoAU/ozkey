/* oz_motor — see oz_motor.h. */
#include <string.h>

#include "oz_motor.h"

void oz_motor_init(oz_motor *m, const oz_motor_cfg *cfg)
{
    static const oz_motor_cfg dflt = {
        OZ_MOTOR_DRIVE_MS, OZ_MOTOR_TIMEOUT_MS, OZ_MOTOR_BRAKE_MS, OZ_MOTOR_REST_MS, 0
    };
    memset(m, 0, sizeof *m);
    m->cfg = cfg ? *cfg : dflt;
}

static void start(oz_motor *m, int unlock, uint32_t now_ms)
{
    m->phase = OZ_MOTOR_DRIVING;
    m->unlock = unlock != 0;
    m->out = unlock ? OZ_MOTOR_TO_UNLOCK : OZ_MOTOR_TO_LOCK;
    m->since_ms = now_ms;
}

void oz_motor_command(oz_motor *m, int unlock, uint32_t now_ms)
{
    unlock = unlock != 0;
    if (m->phase == OZ_MOTOR_IDLE) {
        start(m, unlock, now_ms);
        return;
    }
    if (m->phase == OZ_MOTOR_DRIVING && m->unlock == unlock) {
        m->pending = 0;                 /* already on its way there */
        return;
    }
    m->pending = (uint8_t)(1 + unlock);
}

static int finish(oz_motor *m, uint32_t now_ms, int result, int *unlock)
{
    m->phase = OZ_MOTOR_BRAKING;
    m->out = OZ_MOTOR_BRAKE;
    m->since_ms = now_ms;
    *unlock = m->unlock;
    return result;
}

int oz_motor_step(oz_motor *m, uint32_t now_ms, uint8_t bolt, int stall, int *unlock)
{
    uint32_t t = now_ms - m->since_ms;
    switch (m->phase) {
    case OZ_MOTOR_DRIVING:
        if (m->cfg.has_sensor && bolt == (m->unlock ? OZ_BOLT_RETRACTED : OZ_BOLT_THROWN))
            return finish(m, now_ms, OZ_MOVE_OK, unlock);
        if (stall)
            return finish(m, now_ms, OZ_MOVE_JAMMED, unlock);
        if (m->cfg.has_sensor && t >= m->cfg.timeout_ms)
            return finish(m, now_ms, OZ_MOVE_JAMMED, unlock);
        if (!m->cfg.has_sensor && t >= m->cfg.drive_ms)
            return finish(m, now_ms, OZ_MOVE_UNCONFIRMED, unlock);
        break;
    case OZ_MOTOR_BRAKING:
        if (t >= m->cfg.brake_ms) {
            m->phase = OZ_MOTOR_RESTING;
            m->out = OZ_MOTOR_OFF;
            m->since_ms = now_ms;
        }
        break;
    case OZ_MOTOR_RESTING:
        if (t >= m->cfg.rest_ms) {
            m->phase = OZ_MOTOR_IDLE;
            if (m->pending) {
                int u = m->pending - 1;
                m->pending = 0;
                start(m, u, now_ms);
            }
        }
        break;
    default:
        break;
    }
    return OZ_MOVE_NONE;
}
