/* ccm_vec — stdin: "key nonce aad pt taglen" per line (hex, "-" = empty).
 * stdout: hex(ciphertext ‖ tag), then "ok"/"bad" for open(seal(x)) == x.
 * Driven by diff_ccm.py against an independent AES-CCM implementation. */
#include <stdio.h>
#include <string.h>
#include "oz_crypto.h"

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (!strcmp(s, "-")) return 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(void)
{
    static char k[64], n[64], a[4096], p[4096];
    static uint8_t key[16], nonce[16], aad[2048], pt[2048], out[2100], back[2100];
    unsigned tag;
    while (scanf("%63s %63s %4095s %4095s %u", k, n, a, p, &tag) == 5) {
        unhex(k, key, sizeof key);
        size_t nl = unhex(n, nonce, sizeof nonce);
        size_t al = unhex(a, aad, sizeof aad);
        size_t pl = unhex(p, pt, sizeof pt);
        if (oz_aes128_ccm_seal(key, nonce, nl, aad, al, pt, pl, tag, out, sizeof out)) {
            puts("err");
            continue;
        }
        for (size_t i = 0; i < pl + tag; i++) printf("%02x", out[i]);
        int rc = oz_aes128_ccm_open(key, nonce, nl, aad, al, out, pl + tag, tag, back, sizeof back);
        printf(" %s\n", (!rc && !memcmp(back, pt, pl)) ? "ok" : "bad");
    }
    return 0;
}
