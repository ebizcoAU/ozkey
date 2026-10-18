/* Known-answer tests for the ozcore crypto layer.
 *
 * Vectors are from the RFCs / FIPS documents named on each case, and every one
 * was cross-checked against Python `cryptography` 45 (OpenSSL) on 2026-10-18.
 * The "suite 0" CCM cases use COSE AES-CCM-16-64-128 parameters with a long
 * AAD so the multi-block AAD path is exercised.
 */
#include <stdio.h>
#include <string.h>
#include "oz_crypto.h"
#include "oz_port_host.h"

static int failures, checks;

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void expect(const char *name, const uint8_t *got, size_t len, const char *want_hex)
{
    uint8_t want[512];
    size_t wl = unhex(want_hex, want, sizeof want);
    checks++;
    if (wl != len || memcmp(got, want, len)) {
        failures++;
        printf("FAIL %s\n  got  ", name);
        for (size_t i = 0; i < len; i++) printf("%02x", got[i]);
        printf("\n  want %s\n", want_hex);
    }
}

static void expect_rc(const char *name, int got, int want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: rc %d, want %d\n", name, got, want);
    }
}

static void test_sha256(void)
{
    uint8_t d[32];
    oz_sha256((const uint8_t *)"abc", 3, d);
    expect("sha256 FIPS180 abc", d, 32,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    oz_sha256((const uint8_t *)m, strlen(m), d);
    expect("sha256 FIPS180 448-bit", d, 32,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Streaming in odd-sized pieces must equal one-shot. */
    oz_sha256_ctx c;
    oz_port_sha256_init(&c);
    for (size_t i = 0; i < strlen(m); i += 7)
        oz_port_sha256_update(&c, (const uint8_t *)m + i, strlen(m) - i < 7 ? strlen(m) - i : 7);
    oz_port_sha256_final(&c, d);
    expect("sha256 streaming", d, 32,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_hmac_hkdf(void)
{
    uint8_t k[131], out[64], prk[32], salt[13], info[10], ikm[22];

    memset(k, 0x0b, 20);
    oz_hmac_sha256(k, 20, (const uint8_t *)"Hi There", 8, out);
    expect("hmac RFC4231 TC1", out, 32,
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const char *m2 = "what do ya want for nothing?";
    oz_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)m2, strlen(m2), out);
    expect("hmac RFC4231 TC2", out, 32,
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    memset(k, 0xaa, 131);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    oz_hmac_sha256(k, 131, (const uint8_t *)m6, strlen(m6), out);
    expect("hmac RFC4231 TC6 long key", out, 32,
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    memset(ikm, 0x0b, 22);
    unhex("000102030405060708090a0b0c", salt, sizeof salt);
    unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof info);
    oz_hkdf_extract(salt, 13, ikm, 22, prk);
    expect("hkdf RFC5869 TC1 prk", prk, 32,
           "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    oz_hkdf_expand(prk, info, 10, out, 42);
    expect("hkdf RFC5869 TC1 okm", out, 42,
           "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    oz_hkdf_extract(NULL, 0, ikm, 22, prk);
    oz_hkdf_expand(prk, NULL, 0, out, 42);
    expect("hkdf RFC5869 TC3 no salt/info", out, 42,
           "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
}

static void test_aes_ccm(void)
{
    uint8_t key[16], in[16], out[512], nonce[13], aad[300], pt[64], back[64];

    unhex("000102030405060708090a0b0c0d0e0f", key, 16);
    unhex("00112233445566778899aabbccddeeff", in, 16);
    oz_port_aes128_encrypt_block(key, in, out);
    expect("aes128 FIPS197 C.1", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");

    /* RFC 3610 packet vector #1: 13-byte nonce, 8-byte tag, 8-byte aad. */
    unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key, 16);
    unhex("00000003020100a0a1a2a3a4a5", nonce, 13);
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)i;
    for (int i = 0; i < 23; i++) pt[i] = (uint8_t)(8 + i);
    oz_aes128_ccm_seal(key, nonce, 13, aad, 8, pt, 23, 8, out, sizeof out);
    expect("ccm RFC3610 #1", out, 31,
           "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0");
    expect_rc("ccm RFC3610 #1 open",
              oz_aes128_ccm_open(key, nonce, 13, aad, 8, out, 31, 8, back, sizeof back), OZ_OK);
    expect("ccm RFC3610 #1 roundtrip", back, 23, "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e");

    /* Suite 0 parameters, 300-byte aad, 50-byte pt. */
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    unhex("101112131415161718191a1b1c", nonce, 13);
    for (int i = 0; i < 300; i++) aad[i] = (uint8_t)(i * 7);
    for (int i = 0; i < 50; i++) pt[i] = (uint8_t)(i * 13 + 5);
    oz_aes128_ccm_seal(key, nonce, 13, aad, 300, pt, 50, 8, out, sizeof out);
    expect("ccm suite0 long aad", out, 58,
           "a1e7f73e061a6ff6792af664a961335f2103759a52db2c8f73d3f772c0dcf82c8999a675"
           "de107c0ec3317f97e3cc2865e2b2bab0ce30e3753375");
    oz_aes128_ccm_seal(key, nonce, 13, NULL, 0, pt, 50, 8, out, sizeof out);
    expect("ccm suite0 no aad", out, 58,
           "a1e7f73e061a6ff6792af664a961335f2103759a52db2c8f73d3f772c0dcf82c8999a675"
           "de107c0ec3317f97e3cc2865e2b2252f033daa6757b8");
    oz_aes128_ccm_seal(key, nonce, 13, aad, 20, NULL, 0, 8, out, sizeof out);
    expect("ccm suite0 empty pt", out, 8, "6ffe949888464222");

    /* In-place seal/open, then tamper. */
    memcpy(out, pt, 50);
    oz_aes128_ccm_seal(key, nonce, 13, aad, 300, out, 50, 8, out, sizeof out);
    expect_rc("ccm in-place open",
              oz_aes128_ccm_open(key, nonce, 13, aad, 300, out, 58, 8, out, sizeof out), OZ_OK);
    expect("ccm in-place roundtrip", out, 50,
           "05121f2c394653606d7a8794a1aebbc8d5e2effc091623303d4a5764717e8b98a5b2bfccd9e6f3000d1a2734414e5b687582");
    oz_aes128_ccm_seal(key, nonce, 13, aad, 300, pt, 50, 8, out, sizeof out);
    out[3] ^= 1;
    expect_rc("ccm tampered ct",
              oz_aes128_ccm_open(key, nonce, 13, aad, 300, out, 58, 8, back, sizeof back), OZ_E_AUTH);
    expect("ccm tampered → out zeroed", back, 50,
           "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
    out[3] ^= 1;
    aad[299] ^= 1;
    expect_rc("ccm tampered aad",
              oz_aes128_ccm_open(key, nonce, 13, aad, 300, out, 58, 8, back, sizeof back), OZ_E_AUTH);

    expect_rc("ccm bad nonce len",
              oz_aes128_ccm_seal(key, nonce, 6, NULL, 0, pt, 1, 8, out, sizeof out), OZ_E_ARG);
    expect_rc("ccm odd tag len",
              oz_aes128_ccm_seal(key, nonce, 13, NULL, 0, pt, 1, 7, out, sizeof out), OZ_E_ARG);
    expect_rc("ccm out too small",
              oz_aes128_ccm_seal(key, nonce, 13, NULL, 0, pt, 50, 8, out, 57), OZ_E_NO_SPACE);
}

static void test_x25519(void)
{
    uint8_t a[32], b[32], pub[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
    oz_port_x25519_public_key(pub, a);
    expect("x25519 RFC7748 alice pub", pub, 32,
           "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    oz_port_x25519_public_key(pub, b);
    expect("x25519 RFC7748 bob pub", pub, 32,
           "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    oz_port_x25519(s1, a, pub);
    expect("x25519 RFC7748 shared", s1, 32,
           "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    oz_port_x25519_public_key(pub, a);
    oz_port_x25519(s2, b, pub);
    expect("x25519 shared symmetric", s2, 32,
           "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    uint8_t low_order[32] = { 0 };                     /* the identity point */
    expect_rc("x25519 rejects low-order peer", oz_port_x25519(s1, a, low_order), OZ_E_ARG);
}

static void test_kv(void)
{
    uint8_t buf[8];
    size_t len = 0;
    oz_host_kv_reset();
    expect_rc("kv get absent", oz_port_kv_get("bond0", buf, sizeof buf, &len), OZ_E_NOT_FOUND);
    expect_rc("kv set", oz_port_kv_set("bond0", (const uint8_t *)"abc", 3), OZ_OK);
    expect_rc("kv get", oz_port_kv_get("bond0", buf, sizeof buf, &len), OZ_OK);
    expect("kv value", buf, len, "616263");
    expect_rc("kv erase", oz_port_kv_erase("bond0"), OZ_OK);
    expect_rc("kv get erased", oz_port_kv_get("bond0", buf, sizeof buf, &len), OZ_E_NOT_FOUND);
}

int main(void)
{
    test_sha256();
    test_hmac_hkdf();
    test_aes_ccm();
    test_x25519();
    test_kv();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
