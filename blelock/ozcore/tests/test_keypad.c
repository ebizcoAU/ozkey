/* test_keypad — oz_limit (the wrong-guess policy, PM nexus-a8 2026-10-29) and
 * oz_keypad (entry state machine), both pure, ozkey-49 P5-B. The lock-level
 * behaviour (anti-peep, persistence, owner reset) is in test_lock. */
#include "oz_test.h"
#include "oz_limit.h"
#include "oz_keypad.h"
#include "oz_port.h"

static void limit_policy(void)
{
    oz_limit l;
    uint64_t t = 1000;
    oz_limit_init(&l);
    for (int i = 1; i <= 4; i++)
        expect_int("misses 1-4: no lockout", (long long)oz_limit_miss(&l, t), 0);
    expect_int("not locked", oz_limit_locked(&l, t), 0);
    expect_int("5th miss: 60 s", (long long)oz_limit_miss(&l, t), 60000);
    expect_int("locked", oz_limit_locked(&l, t + 59999), 1);
    expect_int("remaining", (long long)oz_limit_remaining_ms(&l, t + 10000), 50000);
    expect_int("a miss while locked changes nothing", (long long)oz_limit_miss(&l, t + 1), 0);
    expect_int("…the lockout end didn't move", (long long)l.until_ms, (long long)(t + 60000));
    t += 60000;
    expect_int("ends on time", oz_limit_locked(&l, t), 0);

    /* Each further miss: double, to the 30 min cap. */
    static const long long want[] = { 120000, 240000, 480000, 960000, 1800000, 1800000, 1800000 };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        char name[48];
        snprintf(name, sizeof name, "further miss %u", i + 1);
        expect_int(name, (long long)oz_limit_miss(&l, t), want[i]);
        t += (uint64_t)want[i];
    }
    /* 300 more: the level saturates, the cap holds. */
    for (int i = 0; i < 300; i++) {
        uint32_t ms = oz_limit_miss(&l, t);
        t += ms;
        if (ms != OZ_LIMIT_MAX_MS) {
            expect_int("cap holds", ms, OZ_LIMIT_MAX_MS);
            break;
        }
    }
    expect_int("success resets", oz_limit_success(&l), 1);
    expect_int("…to nothing", l.misses || l.level || l.until_ms, 0);
    expect_int("success again: no change to save", oz_limit_success(&l), 0);
    for (int i = 1; i <= 4; i++)
        (void)oz_limit_miss(&l, t);
    expect_int("after a reset: 5 free misses again", (long long)oz_limit_miss(&l, t), 60000);
}

static void limit_persist(void)
{
    oz_limit l, r;
    uint8_t b[OZ_LIMIT_SAVED_LEN];
    oz_limit_init(&l);
    (void)oz_limit_miss(&l, 0);
    (void)oz_limit_miss(&l, 0);
    oz_limit_save(&l, 0, b);
    expect("2 misses saved", b, sizeof b, "0102000000000000");
    expect_int("load", oz_limit_load(&r, b, sizeof b, 500), OZ_OK);
    expect_int("misses restored", r.misses, 2);
    expect_int("no lockout restored", oz_limit_locked(&r, 500), 0);

    for (int i = 0; i < 3; i++)
        (void)oz_limit_miss(&l, 1000);
    oz_limit_save(&l, 1000 + 15000, b);   /* saved 15 s into the 60 s */
    expect("lockout saved", b, sizeof b, "010001000000afc8");    /* 45,000 ms left */
    expect_int("load after a reboot", oz_limit_load(&r, b, sizeof b, 7), OZ_OK);
    expect_int("the rest restarts from boot", (long long)oz_limit_remaining_ms(&r, 7), 45000);
    expect_int("level kept: the next miss doubles", (long long)(r.until_ms = 0, oz_limit_miss(&r, 7)), 120000);

    /* Anything malformed fails closed at the cap. */
    static const char *bad[] = { "01", "0200000000000000", "0105000000000000", "01000000000000000000" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t x[16];
        size_t n = unhex(bad[i], x, sizeof x);
        expect_int("malformed refused", oz_limit_load(&r, x, n, 100), OZ_E_ARG);
        expect_int("…and locked out 30 min", (long long)oz_limit_remaining_ms(&r, 100), OZ_LIMIT_MAX_MS);
        expect_int("…the next miss stays at the cap", (r.until_ms = 0, (long long)oz_limit_miss(&r, 100)),
                   OZ_LIMIT_MAX_MS);
    }
    /* A remaining time over the cap is clamped. */
    expect_int("load over-cap", oz_limit_load(&r, (const uint8_t *)"\x01\x00\x03\x00\xff\xff\xff\xff", 8, 0),
               OZ_OK);
    expect_int("clamped to 30 min", (long long)oz_limit_remaining_ms(&r, 0), OZ_LIMIT_MAX_MS);
}

static int type(oz_keypad *kp, const char *keys, uint32_t *now, uint32_t *code)
{
    int last = OZ_KP_NONE;
    for (; *keys; keys++)
        last = oz_keypad_key(kp, *keys, (*now += 300), code);
    return last;
}

static void keypad(void)
{
    oz_keypad kp;
    uint32_t now = 0xfffff000u, code = 0;   /* the ms clock wraps mid-test */
    oz_keypad_init(&kp);

    expect_int("digit beeps", oz_keypad_key(&kp, '1', now, &code), OZ_KP_KEY);
    expect_int("enter", type(&kp, "234#", &now, &code), OZ_KP_PIN);
    expect("entry", (const uint8_t *)kp.entry, kp.len, "31323334");
    oz_keypad_done(&kp);
    expect_int("done wipes", kp.len == 0 && kp.entry[0] == 0, 1);
    expect_int("# on nothing", oz_keypad_key(&kp, '#', now, &code), OZ_KP_NONE);
    expect_int("* clears", type(&kp, "99*", &now, &code), OZ_KP_CLEARED);
    expect_int("…and enters nothing", oz_keypad_key(&kp, '#', now, &code), OZ_KP_NONE);
    expect_int("* on nothing", oz_keypad_key(&kp, '*', now, &code), OZ_KP_NONE);
    expect_int("other keys ignored", oz_keypad_key(&kp, 'A', now, &code), OZ_KP_NONE);

    /* Too long: 17 digits reach the lock (which refuses them), no more. */
    type(&kp, "123456789012345678901", &now, &code);
    expect_int("kept 17", kp.len, OZ_LOCK_PIN_ENTRY_MAX + 1);
    expect_int("enter", oz_keypad_key(&kp, '#', now, &code), OZ_KP_PIN);
    oz_keypad_done(&kp);

    /* A pause drops the entry, across the clock wrap. */
    type(&kp, "55", &now, &code);
    expect_int("9.9 s: kept", oz_keypad_tick(&kp, now + OZ_KEYPAD_IDLE_MS - 1), OZ_KP_NONE);
    expect_int("10 s: dropped", oz_keypad_tick(&kp, now + OZ_KEYPAD_IDLE_MS), OZ_KP_CLEARED);
    type(&kp, "77", &now, &code);
    now += OZ_KEYPAD_IDLE_MS;
    expect_int("a key after a pause starts afresh", oz_keypad_key(&kp, '8', now, &code), OZ_KP_KEY);
    expect("only the new digit", (const uint8_t *)kp.entry, kp.len, "38");
    type(&kp, "*", &now, &code);

    /* Pairing: exactly 6 digits, leading zeros kept. */
    type(&kp, "12", &now, &code);
    oz_keypad_pairing(&kp, 1);
    expect_int("prompt drops a half-typed PIN", kp.len, 0);
    expect_int("pair code", type(&kp, "012345#", &now, &code), OZ_KP_PAIR_CODE);
    expect_int("value", code, 12345);
    expect_int("wiped", kp.len, 0);
    expect_int("5 digits: dropped", type(&kp, "12345#", &now, &code), OZ_KP_CLEARED);
    expect_int("7 digits: dropped", type(&kp, "1234567#", &now, &code), OZ_KP_CLEARED);
    oz_keypad_pairing(&kp, 0);
    expect_int("prompt off: PINs again", type(&kp, "123456#", &now, &code), OZ_KP_PIN);
    oz_keypad_done(&kp);
}

int main(void)
{
    limit_policy();
    limit_persist();
    keypad();
    return report("keypad");
}
