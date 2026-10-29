/* test_power — oz_power, the battery policy (ozkey-49 P5-F): levels with
 * hysteresis, sag under motor load, and the rule that unlocking is never
 * refused. Thresholds are the placeholders in oz_power.h. */
#include "oz_test.h"
#include "oz_power.h"

static void levels(void)
{
    oz_power p;
    oz_power_init(&p);
    expect_int("no reading: ok", p.level, OZ_BATT_OK);
    expect_int("…100 %", p.percent, 100);
    expect_int("…may lock", oz_power_may_move(&p, 0), 1);
    expect_int("a 0 reading (ADC error) is ignored", oz_power_rest(&p, 0), 0);
    expect_int("…nothing recorded", p.rest_mv, 0);

    expect_int("5900 mV: no change", oz_power_rest(&p, 5900), 0);
    expect_int("…93 %", p.percent, 93);
    expect_int("4790: low", oz_power_rest(&p, 4790) && p.level == OZ_BATT_LOW, 1);
    expect_int("…low still locks", oz_power_may_move(&p, 0), 1);
    expect_int("4850: just over, stays low", oz_power_rest(&p, 4850) == 0 && p.level == OZ_BATT_LOW, 1);
    expect_int("4950: over with margin, ok", oz_power_rest(&p, 4950) && p.level == OZ_BATT_OK, 1);

    expect_int("4490: straight to critical", oz_power_rest(&p, 4490) && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("…no lock move", oz_power_may_move(&p, 0), 0);
    expect_int("…unlock always", oz_power_may_move(&p, 1), 1);
    expect_int("4600: just over, stays critical", oz_power_rest(&p, 4600) == 0 && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("4700: low with margin", oz_power_rest(&p, 4700) && p.level == OZ_BATT_LOW, 1);
    expect_int("6000: ok", oz_power_rest(&p, 6000) && p.level == OZ_BATT_OK, 1);
}

static void sag(void)
{
    oz_power p;
    oz_power_init(&p);
    oz_power_rest(&p, 5200);
    expect_int("4100 under load: no change", oz_power_load(&p, 4100), 0);
    expect_int("a 0 load reading is ignored", oz_power_load(&p, 0), 0);
    expect_int("3900 under load: critical", oz_power_load(&p, 3900) && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("…no lock move", oz_power_may_move(&p, 0), 0);
    expect_int("…unlock always", oz_power_may_move(&p, 1), 1);
    /* It recovers at rest; that doesn't mean it can drive the motor. */
    expect_int("5200 at rest after the sag: still critical",
               oz_power_rest(&p, 5200) == 0 && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("5599: not fresh cells, still critical",
               oz_power_rest(&p, 5599) == 0 && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("an unlock that doesn't sag (4300) clears it", oz_power_load(&p, 4300) && p.level == OZ_BATT_OK, 1);
    expect_int("…sag cleared", p.sagged, 0);
    expect_int("sags again", oz_power_load(&p, 3999) && p.level == OZ_BATT_CRITICAL, 1);
    expect_int("new batteries (5600): ok", oz_power_rest(&p, 5600) && p.level == OZ_BATT_OK, 1);
    expect_int("…sag cleared", p.sagged, 0);
    /* A good move with a low rest reading settles to that level. */
    oz_power_rest(&p, 4700);
    oz_power_load(&p, 3900);
    expect_int("good move after a sag at 4700 rest: low", oz_power_load(&p, 4200) && p.level == OZ_BATT_LOW, 1);
}

static void percent(void)
{
    oz_power p;
    static const struct { uint16_t mv; int pct; } t[] = {
        { 3000, 0 }, { 4400, 0 }, { 4416, 1 }, { 5200, 50 }, { 5999, 99 }, { 6000, 100 }, { 7000, 100 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        char name[32];
        oz_power_init(&p);
        oz_power_rest(&p, t[i].mv);
        snprintf(name, sizeof name, "%u mV", (unsigned)t[i].mv);
        expect_int(name, p.percent, t[i].pct);
    }
}

int main(void)
{
    levels();
    sag();
    percent();
    return report("power");
}
