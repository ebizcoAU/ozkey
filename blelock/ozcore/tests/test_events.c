/* test_events — oz_events, the event ring (ozkey-48 §5.1, ozkey-49 P5-E0):
 * order, overflow with a gap marker, readers ahead of the lock, and the
 * 32-bit sequence wrap. The wire side is in test_lock. */
#include "oz_test.h"
#include "oz_events.h"

static void basics(void)
{
    oz_events e;
    oz_event ev;
    uint32_t cur = 0;
    oz_events_init(&e, 0);
    expect_int("empty: nothing", oz_events_next(&e, &cur, &ev), 0);
    expect_int("empty: oldest = next", oz_events_oldest(&e), 0);
    expect_int("first seq", oz_events_add(&e, OZ_EV_KEYPAD_LOCKOUT, OZ_EV_NO_ACTOR, 1790000000, 1, 60), 0);
    expect_int("second seq", oz_events_add(&e, OZ_EV_KEYPAD_LOCKOUT, 3, 0, 2, 120), 1);
    expect_int("first out", oz_events_next(&e, &cur, &ev), 1);
    expect_int("…seq", ev.seq, 0);
    expect_int("…type", ev.type, OZ_EV_KEYPAD_LOCKOUT);
    expect_int("…no actor", ev.actor, OZ_EV_NO_ACTOR);
    expect_int("…utc", (long long)ev.utc, 1790000000);
    expect_int("…data", ev.a == 1 && ev.b == 60, 1);
    expect_int("second out", oz_events_next(&e, &cur, &ev), 1);
    expect_int("…seq, actor, data", ev.seq == 1 && ev.actor == 3 && ev.utc == 0 && ev.a == 2 && ev.b == 120, 1);
    expect_int("up to date", oz_events_next(&e, &cur, &ev), 0);
    expect_int("cursor", cur, 2);

    /* A reader ahead of the lock (a cursor from before a reboot) waits. */
    uint32_t ahead = 50;
    expect_int("ahead: nothing", oz_events_next(&e, &ahead, &ev), 0);
    expect_int("…cursor untouched", ahead, 50);
}

static void overflow(void)
{
    oz_events e;
    oz_event ev;
    uint32_t slow = 100, fast = 100;
    oz_events_init(&e, 100);
    for (uint32_t i = 0; i < 23; i++) {
        oz_events_add(&e, OZ_EV_KEYPAD_LOCKOUT, OZ_EV_NO_ACTOR, 0, i, 0);
        if (i < 5)
            (void)oz_events_next(&e, &fast, &ev);
    }
    expect_int("ring holds 16", e.count, OZ_EVENTS_RING);
    expect_int("oldest held", oz_events_oldest(&e), 107);

    expect_int("slow reader: a gap first", oz_events_next(&e, &slow, &ev), 1);
    expect_int("…gap type", ev.type, OZ_EV_GAP);
    expect_int("…starting at its cursor", ev.seq, 100);
    expect_int("…7 missed", ev.a, 7);
    expect_int("…then from the oldest", slow, 107);
    uint32_t want = 107;
    int in_order = 1, n = 0;
    while (oz_events_next(&e, &slow, &ev)) {
        in_order &= ev.seq == want && ev.a == want - 100 && ev.type == OZ_EV_KEYPAD_LOCKOUT;
        want++;
        n++;
    }
    expect_int("then all 16 held, in order", n == 16 && in_order, 1);

    /* The other reader read 5, then fell 2 behind the ring. */
    expect_int("second reader: own gap", oz_events_next(&e, &fast, &ev) && ev.type == OZ_EV_GAP, 1);
    expect_int("…of 2", ev.seq == 105 && ev.a == 2, 1);

    /* Exactly at the oldest: no gap. */
    uint32_t edge = 107;
    expect_int("at the oldest: an event", oz_events_next(&e, &edge, &ev) && ev.type == OZ_EV_KEYPAD_LOCKOUT, 1);
}

static void wrap(void)
{
    oz_events e;
    oz_event ev;
    uint32_t cur = 0xfffffff0u;
    oz_events_init(&e, 0xfffffff0u);
    for (int i = 0; i < 40; i++)
        oz_events_add(&e, OZ_EV_KEYPAD_LOCKOUT, OZ_EV_NO_ACTOR, 0, 0, 0);
    expect_int("wrapped", e.next_seq, 24);
    expect_int("gap across the wrap", oz_events_next(&e, &cur, &ev) && ev.type == OZ_EV_GAP, 1);
    expect_int("…24 missed", (long long)ev.a, 24);
    expect_int("…resumes at 8", cur, 8);
    int n = 0;
    while (oz_events_next(&e, &cur, &ev))
        n++;
    expect_int("16 after it", n, 16);
}

int main(void)
{
    basics();
    overflow();
    wrap();
    return report("events");
}
