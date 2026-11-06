/* test_card — oz_card, one tap per card at the reader (ozkey-49 P5-E). The
 * lock's side (matching, the wrong-card limit) is in test_lock. */
#include "oz_test.h"
#include "oz_card.h"

static const uint8_t A[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6 };
static const uint8_t B[4] = { 0x11, 0x22, 0x33, 0x44 };

int main(void)
{
    oz_card c;
    uint32_t t = 0xfffff000u;           /* the ms clock wraps mid-test */
    oz_card_init(&c);

    expect_int("first read: a tap", oz_card_seen(&c, A, sizeof A, t), 1);
    /* Held on the reader, read every 100 ms for 5 s: still one tap. */
    int taps = 0;
    for (int i = 1; i <= 50; i++)
        taps += oz_card_seen(&c, A, sizeof A, t + 100u * (uint32_t)i);
    expect_int("held 5 s, across the wrap: no new tap", taps, 0);
    t += 5000;

    expect_int("lifted 1.4 s, back: same tap", oz_card_seen(&c, A, sizeof A, t + OZ_CARD_HOLD_MS - 1), 0);
    t += OZ_CARD_HOLD_MS - 1;
    expect_int("lifted 1.5 s, back: a new tap", oz_card_seen(&c, A, sizeof A, t + OZ_CARD_HOLD_MS), 1);
    t += OZ_CARD_HOLD_MS;

    expect_int("another card at once: a tap", oz_card_seen(&c, B, sizeof B, t + 1), 1);
    expect_int("the first again at once: a tap", oz_card_seen(&c, A, sizeof A, t + 2), 1);
    /* A UID that is a prefix of the last one is a different card. */
    expect_int("a prefix: a tap", oz_card_seen(&c, A, 4, t + 3), 1);

    expect_int("empty id dropped", oz_card_seen(&c, A, 0, t + 4), 0);
    uint8_t big[OZ_CARD_ID_MAX + 1] = { 0 };
    expect_int("17 bytes dropped", oz_card_seen(&c, big, sizeof big, t + 5), 0);
    expect_int("16 bytes taken", oz_card_seen(&c, big, OZ_CARD_ID_MAX, t + 6), 1);
    return report("card");
}
