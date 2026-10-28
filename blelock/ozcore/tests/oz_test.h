/* oz_test.h — tiny check helpers shared by the P2 tests. */
#ifndef OZ_TEST_H
#define OZ_TEST_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures, checks;

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        if (s[0] == ' ') { s++; continue; }
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void expect(const char *name, const uint8_t *got, size_t len, const char *want_hex)
{
    static uint8_t want[2048];
    size_t wl = unhex(want_hex, want, sizeof want);
    checks++;
    if (wl != len || memcmp(got, want, len)) {
        failures++;
        printf("FAIL %s\n  got  ", name);
        for (size_t i = 0; i < len; i++) printf("%02x", got[i]);
        printf("\n  want %s\n", want_hex);
    }
}

static void expect_int(const char *name, long long got, long long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: %lld, want %lld\n", name, got, want);
    }
}

static int report(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, checks, failures);
    return failures ? 1 : 0;
}

#endif
