/* test_doorio — oz_doorio, the tamper switch, door sensor and exit button
 * (LOCK-IO-1). One row per rule, the PM's safe-exit rules first. */
#include "oz_test.h"
#include "oz_doorio.h"

static const oz_doorio_cfg bench = OZ_DOORIO_BENCH;
static uint32_t now;
static uint8_t sw[OZ_DIO_SW_N];

/* Run passes every 10 ms for ms, OR-ing what step() returned; counts EXIT/RELOCK. */
static int run(oz_doorio *d, uint32_t ms, int *exits, int *relocks)
{
    int all = 0;
    for (uint32_t t = 0; t < ms; t += 10) {
        now += 10;
        int r = oz_doorio_step(d, sw, now);
        if (exits && (r & OZ_DIO_EXIT_UNLOCK))
            (*exits)++;
        if (relocks && (r & OZ_DIO_RELOCK))
            (*relocks)++;
        all |= r;
    }
    return all;
}

static void fresh(oz_doorio *d, const oz_doorio_cfg *c)
{
    now = 1000;
    for (int i = 0; i < OZ_DIO_SW_N; i++)
        sw[i] = 0;
    oz_doorio_init(d, c);
    (void)run(d, 100, NULL, NULL);
}

static void exit_always(void)
{
    oz_doorio d;
    int exits = 0;
    fresh(&d, &bench);
    sw[OZ_DIO_SW_EXIT] = 1;
    (void)run(&d, 1000, &exits, NULL);
    sw[OZ_DIO_SW_EXIT] = 0;
    (void)run(&d, 200, &exits, NULL);
    expect_int("exit press: one unlock (held 1 s, no repeat)", exits, 1);

    /* PM: tamper never masks the exit. */
    sw[OZ_DIO_SW_TAMPER] = 1;
    int r = run(&d, 200, NULL, NULL);
    sw[OZ_DIO_SW_TAMPER] = 0;
    expect_int("tamper: alarm on", (r & OZ_DIO_TAMPER) && oz_doorio_alarm(&d), 1);
    exits = 0;
    sw[OZ_DIO_SW_EXIT] = 1;
    (void)run(&d, 200, &exits, NULL);
    sw[OZ_DIO_SW_EXIT] = 0;
    (void)run(&d, 200, &exits, NULL);
    expect_int("exit during the tamper alarm still unlocks (PM)", exits, 1);

    /* A 30 ms bounce is not a press. */
    exits = 0;
    sw[OZ_DIO_SW_EXIT] = 1;
    (void)run(&d, 30, &exits, NULL);
    sw[OZ_DIO_SW_EXIT] = 0;
    (void)run(&d, 200, &exits, NULL);
    expect_int("a 30 ms bounce: no unlock", exits, 0);
}

static void relock_rules(void)
{
    oz_doorio d;
    int relocks = 0;
    fresh(&d, &bench);
    oz_doorio_unlocked(&d, now);
    sw[OZ_DIO_SW_DOOR] = 1;                       /* the door opens */
    int r = run(&d, 200, NULL, &relocks);
    expect_int("door opened (confirms the unlock)", (r & OZ_DIO_DOOR_OPENED) != 0, 1);
    /* PM: never relock while the door reads open, however long. */
    r = run(&d, 40000, NULL, &relocks);
    expect_int("door open 40 s: no relock (PM)", relocks, 0);
    expect_int("…one left-open warning", (r & OZ_DIO_LEFT_OPEN) != 0, 1);
    sw[OZ_DIO_SW_DOOR] = 0;                       /* closed */
    r = run(&d, 4800, NULL, &relocks);
    expect_int("door closed", (r & OZ_DIO_DOOR_CLOSED) != 0, 1);
    expect_int("…no relock before 5 s", relocks, 0);
    (void)run(&d, 400, NULL, &relocks);
    expect_int("…relock at 5 s", relocks, 1);
    oz_doorio_locked(&d);
    (void)run(&d, 30000, NULL, &relocks);
    expect_int("locked: no more relocks", relocks, 1);

    /* Reopened inside the 5 s: the relock waits for the next close. */
    relocks = 0;
    oz_doorio_unlocked(&d, now);
    sw[OZ_DIO_SW_DOOR] = 1; (void)run(&d, 200, NULL, &relocks);
    sw[OZ_DIO_SW_DOOR] = 0; (void)run(&d, 3000, NULL, &relocks);
    sw[OZ_DIO_SW_DOOR] = 1; (void)run(&d, 10000, NULL, &relocks);
    expect_int("reopened before the relock: none while open", relocks, 0);
    sw[OZ_DIO_SW_DOOR] = 0; (void)run(&d, 5300, NULL, &relocks);
    expect_int("…relock 5 s after the second close", relocks, 1);
    oz_doorio_locked(&d);

    /* Unlocked, nobody opened the door: relock after 15 s. */
    relocks = 0;
    oz_doorio_unlocked(&d, now);
    (void)run(&d, 14800, NULL, &relocks);
    expect_int("unopened: no relock before 15 s", relocks, 0);
    (void)run(&d, 400, NULL, &relocks);
    expect_int("…relock at 15 s", relocks, 1);
}

static void tamper_alarm(void)
{
    oz_doorio d;
    fresh(&d, &bench);
    sw[OZ_DIO_SW_TAMPER] = 1;
    int r = run(&d, 200, NULL, NULL);
    sw[OZ_DIO_SW_TAMPER] = 0;
    expect_int("tamper reported once", (r & OZ_DIO_TAMPER) != 0, 1);
    r = run(&d, 9500, NULL, NULL);
    expect_int("alarm still on at 9.7 s", oz_doorio_alarm(&d) && !(r & OZ_DIO_ALARM_END), 1);
    r = run(&d, 600, NULL, NULL);
    expect_int("alarm over at 10 s", !oz_doorio_alarm(&d) && (r & OZ_DIO_ALARM_END), 1);

    /* Product polarity: the switch is held pressed while mounted. */
    oz_doorio_cfg prod = bench;
    prod.tamper_when_pressed = 0;
    sw[OZ_DIO_SW_TAMPER] = 1;                     /* mounted at boot */
    now = 1000;
    oz_doorio_init(&d, &prod);
    r = run(&d, 500, NULL, NULL);
    expect_int("mounted at boot: no tamper", (r & OZ_DIO_TAMPER) != 0, 0);
    sw[OZ_DIO_SW_TAMPER] = 0;                     /* pulled off */
    r = run(&d, 200, NULL, NULL);
    expect_int("released (pulled off): tamper", (r & OZ_DIO_TAMPER) != 0, 1);
}

static void boot_levels(void)
{
    oz_doorio d;
    sw[OZ_DIO_SW_TAMPER] = 0; sw[OZ_DIO_SW_EXIT] = 0;
    sw[OZ_DIO_SW_DOOR] = 1;                       /* door open at boot */
    now = 1000;
    oz_doorio_init(&d, &bench);
    int r = run(&d, 300, NULL, NULL);
    expect_int("door open at boot: no 'opened' edge", (r & OZ_DIO_DOOR_OPENED) != 0, 0);
    expect_int("…but it reads open", oz_doorio_door_open(&d), 1);
}

int main(void)
{
    exit_always();
    relock_rules();
    tamper_alarm();
    boot_levels();
    return report("doorio");
}
