/* test_frame — OZKIE GATT framing (ozkey-48 §2.1). */
#include <stdlib.h>

#include "oz_test.h"
#include "oz_port.h"
#include "oz_frame.h"
#include "oz_ble.h"

static oz_reasm R;

static int feed(const uint8_t *f, size_t n, int *done) { return oz_reasm_feed(&R, f, n, done); }

/* Fragment msg at this MTU, feed every frame back; 1 if it comes out intact. */
static int roundtrip(uint8_t type, const uint8_t *msg, size_t len, size_t mtu)
{
    unsigned count;
    if (oz_frag_count(len, mtu, &count))
        return 0;
    oz_reasm_reset(&R);
    for (unsigned i = 0; i < count; i++) {
        uint8_t f[600];
        size_t n;
        int done;
        if (oz_frag(type, msg, len, mtu, i, f, sizeof f, &n) || n > mtu - 3)
            return 0;
        if (feed(f, n, &done) || done != (i + 1 == count))
            return 0;
    }
    return R.type == type && R.len == len && !memcmp(R.buf, msg, len);
}

static void round_trips(void)
{
    static uint8_t msg[OZ_FRAME_MAX_MSG];
    for (size_t i = 0; i < sizeof msg; i++)
        msg[i] = (uint8_t)(i * 31 + 7);
    const size_t mtus[] = {23, 64, 185, 247, 517};
    int ok = 1;
    long cases = 0;
    for (size_t m = 0; m < sizeof mtus / sizeof mtus[0]; m++) {
        size_t chunk = mtus[m] - 4;
        for (size_t len = 0; len <= OZ_FRAME_MAX_MSG; len++) {
            unsigned count;
            if (oz_frag_count(len, mtus[m], &count))
                continue;               /* too many frames at this MTU */
            cases++;
            ok &= roundtrip((uint8_t)(len % 3), msg, len, mtus[m]);
            ok &= count == (len ? (len + chunk - 1) / chunk : 1);
        }
    }
    expect_int("every length at every MTU", ok, 1);
    printf("frame: %ld round trips\n", cases);

    unsigned c = 0;
    expect_int("1024 B at MTU 247 = 5 frames", oz_frag_count(1024, 247, &c), OZ_OK);
    expect_int("  count", c, 5);
    expect_int("608 B at MTU 23 = 32 frames", oz_frag_count(608, 23, &c), OZ_OK);
    expect_int("  count", c, 32);
    expect_int("609 B at MTU 23 needs 33", oz_frag_count(609, 23, &c), OZ_E_NO_SPACE);
    expect_int("1025 B too long", oz_frag_count(1025, 247, &c), OZ_E_NO_SPACE);
    expect_int("MTU 22 refused", oz_frag_count(10, 22, &c), OZ_E_ARG);
    uint8_t f[300];
    size_t n;
    expect_int("frag type 3", oz_frag(3, msg, 10, 247, 0, f, sizeof f, &n), OZ_E_ARG);
    expect_int("frag idx past end", oz_frag(0, msg, 10, 247, 1, f, sizeof f, &n), OZ_E_ARG);
    expect_int("frag small out", oz_frag(0, msg, 100, 247, 0, f, 50, &n), OZ_E_NO_SPACE);
}

static void malformed(void)
{
    int done;
    uint8_t a[] = {0x20, 1, 2}, b[] = {0x01, 3}, c[] = {0x02, 4};

    oz_reasm_reset(&R);
    expect_int("empty frame", feed(a, 0, &done), OZ_E_ARG);
    uint8_t t3[] = {0xc0, 1};
    expect_int("type 3", feed(t3, 2, &done), OZ_E_ARG);
    expect_int("starts at frag 1", feed(b, 2, &done), OZ_E_ARG);

    expect_int("frag 0", feed(a, 3, &done), OZ_OK);
    expect_int("  not done", done, 0);
    expect_int("skips frag 1", feed(c, 2, &done), OZ_E_ARG);
    expect_int("dropped after error", feed(b, 2, &done), OZ_E_ARG);

    feed(a, 3, &done);
    uint8_t other[] = {0x41, 3};        /* OSCORE type mid-EDHOC */
    expect_int("type changes mid-message", feed(other, 2, &done), OZ_E_ARG);

    feed(a, 3, &done);
    uint8_t restart[] = {0x40, 9};      /* frag 0 again: a new message wins */
    expect_int("frag 0 restarts", feed(restart, 2, &done), OZ_OK);
    expect_int("  done", done, 1);
    expect_int("  type", R.type, 1);
    expect_int("  len", (long long)R.len, 1);
    expect_int("  byte", R.buf[0], 9);

    uint8_t empty_more[] = {0x20};
    expect_int("empty non-final", feed(empty_more, 1, &done), OZ_E_ARG);
    uint8_t empty_final[] = {0x80};     /* an error frame with no payload */
    expect_int("empty single frame", feed(empty_final, 1, &done), OZ_OK);
    expect_int("  done", done, 1);
    expect_int("  len", (long long)R.len, 0);

    /* 31 fragments with "more", then a 32nd that still says "more". */
    uint8_t f[2] = {0, 0xaa};
    oz_reasm_reset(&R);
    int rc = OZ_OK;
    for (uint8_t i = 0; i < 31 && rc == OZ_OK; i++) {
        f[0] = (uint8_t)(0x20 | i);
        rc = feed(f, 2, &done);
    }
    expect_int("31 fragments ok", rc, OZ_OK);
    f[0] = 0x20 | 31;
    expect_int("frag 31 with more", feed(f, 2, &done), OZ_E_ARG);

    /* Over 1024 bytes. */
    static uint8_t big[300];
    memset(big, 0x5a, sizeof big);
    oz_reasm_reset(&R);
    rc = OZ_OK;
    for (uint8_t i = 0; i < 3 && rc == OZ_OK; i++) {
        big[0] = (uint8_t)(0x20 | i);
        rc = feed(big, sizeof big, &done);   /* 3 × 299 = 897 */
    }
    big[0] = 0x03;
    expect_int("past 1024", feed(big, sizeof big, &done), OZ_E_NO_SPACE);
    big[0] = 0x00;
    expect_int("recovers on next frag 0", feed(big, 128, &done), OZ_OK);
    expect_int("  len", (long long)R.len, 127);
}

/* Random frames from a hostile central: never more than 1024 bytes, never stuck. */
static void fuzz(void)
{
    srand(48);
    uint8_t f[260];
    long msgs = 0, bad = 0;
    oz_reasm_reset(&R);
    for (long i = 0; i < 200000; i++) {
        size_t n = (size_t)(rand() % 258);
        for (size_t j = 0; j < n; j++)
            f[j] = (uint8_t)rand();
        if (n && rand() % 2)
            f[0] = (uint8_t)((f[0] & 0xe0) | (R.active ? R.next : 0));
        int done;
        int rc = feed(f, n, &done);
        if (R.len > OZ_FRAME_MAX_MSG || (rc && R.active)) bad++;
        msgs += done;
    }
    expect_int("fuzz: bounded and clean on error", bad, 0);
    printf("frame: fuzz completed %ld messages\n", msgs);
}

/* The LE byte strings must be the spec's UUID strings, reversed. */
static void uuids(void)
{
    const char *want[3] = {"4f5a4b32-0001-4c4f-434b-000000000001",
                           "4f5a4b32-0002-4c4f-434b-000000000001",
                           "4f5a4b32-0003-4c4f-434b-000000000001"};
    const char *le[3] = {OZ_BLE_UUID_SERVICE, OZ_BLE_UUID_INFO, OZ_BLE_UUID_OZKIE};
    for (int i = 0; i < 3; i++) {
        uint8_t be[16];
        size_t n = 0;
        for (const char *p = want[i]; *p && n < 16; p++) {
            if (*p == '-') continue;
            unsigned v;
            sscanf(p, "%2x", &v);
            be[n++] = (uint8_t)v;
            p++;
        }
        int ok = n == 16;
        for (int j = 0; ok && j < 16; j++)
            ok = (uint8_t)le[i][j] == be[15 - j];
        expect_int(want[i], ok, 1);
    }
}

int main(void)
{
    uuids();
    round_trips();
    malformed();
    fuzz();
    return report("frame");
}
