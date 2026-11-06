/* CBOR writer/reader tests. Encodings from RFC 8949 Appendix A. */
#include "oz_cbor.h"
#include "oz_test.h"

static void enc_int(int64_t v, const char *want)
{
    uint8_t b[16];
    oz_cbor_w w;
    oz_cbor_w_init(&w, b, sizeof b);
    oz_cbor_put_int(&w, v);
    char name[48];
    snprintf(name, sizeof name, "int %lld", (long long)v);
    expect(name, b, w.len, want);

    oz_cbor_r r;
    oz_cbor_r_init(&r, b, w.len);
    expect_int(name, oz_cbor_get_int(&r), v);
    expect_int("int at end", oz_cbor_at_end(&r), 1);
}

static void rejects(const char *name, const char *hex)
{
    uint8_t b[16];
    size_t n = unhex(hex, b, sizeof b);
    oz_cbor_r r;
    oz_cbor_r_init(&r, b, n);
    size_t sl;
    (void)oz_cbor_skip(&r, &sl);
    expect_int(name, r.err != 0, 1);
}

int main(void)
{
    enc_int(0, "00");
    enc_int(23, "17");
    enc_int(24, "1818");
    enc_int(100, "1864");
    enc_int(1000, "1903e8");
    enc_int(1000000, "1a000f4240");
    enc_int(1000000000000LL, "1b000000e8d4a51000");
    enc_int(-1, "20");
    enc_int(-10, "29");
    enc_int(-100, "3863");
    enc_int(-1000, "3903e7");

    uint8_t b[64];
    oz_cbor_w w;
    oz_cbor_w_init(&w, b, sizeof b);
    oz_cbor_put_bstr(&w, (const uint8_t *)"\x01\x02\x03\x04", 4);
    oz_cbor_put_tstr(&w, "IETF", 4);
    oz_cbor_put_array(&w, 3);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, 2); oz_cbor_put_uint(&w, 3);
    oz_cbor_put_map(&w, 2);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, 2);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_uint(&w, 4);
    oz_cbor_put_bool(&w, 1);
    oz_cbor_put_null(&w);
    expect("sequence", b, w.len, "4401020304" "6449455446" "83010203" "a201020304" "f5" "f6");

    oz_cbor_r r;
    oz_cbor_r_init(&r, b, w.len);
    size_t n;
    const uint8_t *p = oz_cbor_get_bstr(&r, &n);
    expect("read bstr", p, n, "01020304");
    p = oz_cbor_get_tstr(&r, &n);
    expect("read tstr", p, n, "49455446");
    const uint8_t *arr = oz_cbor_skip(&r, &n);
    expect("skip array", arr, n, "83010203");
    expect_int("map pairs", (long long)oz_cbor_get_map(&r), 2);
    for (int i = 0; i < 4; i++)
        (void)oz_cbor_get_uint(&r);
    expect_int("bool", oz_cbor_get_bool(&r), 1);
    (void)oz_cbor_skip(&r, &n);
    expect_int("sequence read to end", oz_cbor_at_end(&r), 1);

    /* Overflow is sticky and reported. */
    oz_cbor_w_init(&w, b, 3);
    oz_cbor_put_bstr(&w, (const uint8_t *)"abcd", 4);
    expect_int("writer overflow", w.err, OZ_E_NO_SPACE);

    rejects("non-shortest uint", "1817");
    rejects("non-shortest len", "580101");
    rejects("indefinite bstr", "5f4101ff");
    rejects("tag", "c0");
    rejects("half float", "f93c00");
    rejects("truncated bstr", "4401");
    rejects("huge array count", "9bffffffffffffffff");
    rejects("nesting too deep", "818181818181818181818100");

    return report("cbor");
}
