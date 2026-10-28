/* CoAP + OSCORE tests: RFC 8613 Appendix C vectors 1–8, whole messages, both
 * directions; then replay window, B.1.1 sequence blocks, B.1.2 unknown window
 * and tampering. */
#include "oz_oscore.h"
#include "oz_test.h"

static const uint8_t SECRET[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static const uint8_t SALT[8] = { 0x9e, 0x7c, 0xa9, 0x22, 0x23, 0x78, 0x63, 0x40 };
static const uint8_t IDCTX[8] = { 0x37, 0xcb, 0xf3, 0x21, 0x00, 0x17, 0xa2, 0xd3 };
static const uint8_t ID00 = 0x00, ID01 = 0x01;

static void check_ctx(const char *name, const oz_oscore_ctx *c,
                      const char *sk, const char *rk, const char *iv)
{
    char n[96];
    snprintf(n, sizeof n, "%s sender key", name);  expect(n, c->sender_key, 16, sk);
    snprintf(n, sizeof n, "%s recipient key", name); expect(n, c->recipient_key, 16, rk);
    snprintf(n, sizeof n, "%s common IV", name); expect(n, c->common_iv, 13, iv);
}

static void test_derivation(void)
{
    oz_oscore_ctx c;
    oz_oscore_derive(&c, SECRET, 16, SALT, 8, NULL, 0, &ID01, 1, NULL, 0);
    check_ctx("C.1 client", &c, "f0910ed7295e6ad4b54fc793154302ff",
              "ffb14e093c94c9cac9471648b4f98710", "4622d4dd6d944168eefb54987c");
    oz_oscore_derive(&c, SECRET, 16, SALT, 8, &ID01, 1, NULL, 0, NULL, 0);
    check_ctx("C.1 server", &c, "ffb14e093c94c9cac9471648b4f98710",
              "f0910ed7295e6ad4b54fc793154302ff", "4622d4dd6d944168eefb54987c");
    oz_oscore_derive(&c, SECRET, 16, NULL, 0, &ID00, 1, &ID01, 1, NULL, 0);
    check_ctx("C.2 client", &c, "321b26943253c7ffb6003b0b64d74041",
              "e57b5635815177cd679ab4bcec9d7dda", "be35ae297d2dace910c52e99f9");
    oz_oscore_derive(&c, SECRET, 16, SALT, 8, NULL, 0, &ID01, 1, IDCTX, 8);
    check_ctx("C.3 client", &c, "af2a1300a5e95788b356336eeecd2b92",
              "e39a0c7c77b43f03b4b39ab9a268699f", "2ca58fb85ff1b81c0b7181b85e");
    expect_int("same sender and recipient ID refused",
               oz_oscore_derive(&c, SECRET, 16, NULL, 0, &ID01, 1, &ID01, 1, NULL, 0), OZ_E_ARG);
}

/* Protect `plain_hex` with the client context at ssn 20, compare to
 * `prot_hex`, then verify it on the server and compare the result. */
static void request_vector(const char *name, oz_oscore_ctx *cli, oz_oscore_ctx *srv,
                           const char *plain_hex, const char *prot_hex,
                           oz_oscore_xchg *xs)
{
    uint8_t plain[64], out[128], pt[64], re[128];
    size_t pl = unhex(plain_hex, plain, sizeof plain), ol, rl;
    oz_coap_msg m;
    expect_int("decode plain request", oz_coap_decode(&m, plain, pl), 0);
    cli->ssn = 20;
    oz_oscore_xchg xc;
    expect_int("protect request", oz_oscore_protect_req(cli, &m, out, sizeof out, &ol, &xc), 0);
    expect(name, out, ol, prot_hex);

    oz_coap_msg d;
    expect_int("server verifies", oz_oscore_verify_req(srv, out, ol, pt, sizeof pt, &d, xs), 0);
    oz_coap_encode(&d, re, sizeof re, &rl);
    char n[96];
    snprintf(n, sizeof n, "%s decrypted", name);
    expect(n, re, rl, plain_hex);
}

static void test_vectors(void)
{
    oz_oscore_ctx cli, srv;
    oz_oscore_xchg xs;
    uint8_t out[128], pt[64], re[128], plain[64];
    size_t ol, rl, pl;
    oz_coap_msg m, d;
    int64_t piv;

    /* C.4 request, C.7 and C.8 responses. */
    oz_oscore_derive(&cli, SECRET, 16, SALT, 8, NULL, 0, &ID01, 1, NULL, 0);
    oz_oscore_derive(&srv, SECRET, 16, SALT, 8, &ID01, 1, NULL, 0, NULL, 0);
    request_vector("C.4 request", &cli, &srv,
                   "44015d1f00003974396c6f63616c686f737483747631",
                   "44025d1f00003974396c6f63616c686f7374620914ff612f1092f1776f1c1668b3825e", &xs);
    char path[16];

    pl = unhex("64455d1f00003974ff48656c6c6f20576f726c6421", plain, sizeof plain);
    oz_coap_decode(&m, plain, pl);
    srv.ssn = 0;
    oz_oscore_protect_resp(&srv, &xs, &m, 0, out, sizeof out, &ol);
    expect("C.7 response", out, ol,
           "64445d1f0000397490ffdbaad1e9a7e7b2a813d3c31524378303cdafae119106");
    oz_oscore_xchg xc;
    /* The client's exchange for that request: re-protect at ssn 20. */
    {
        uint8_t q[64], qo[128];
        size_t ql = unhex("44015d1f00003974396c6f63616c686f737483747631", q, sizeof q), qol;
        oz_coap_msg qm;
        oz_coap_decode(&qm, q, ql);
        cli.ssn = 20;
        oz_oscore_protect_req(&cli, &qm, qo, sizeof qo, &qol, &xc);
        expect_int("C.4 request path", oz_coap_decode(&qm, q, ql) || oz_coap_path(&qm, path, sizeof path), 0);
        expect("C.4 path", (const uint8_t *)path, strlen(path), "747631");
    }
    expect_int("client verifies C.7", oz_oscore_verify_resp(&cli, &xc, out, ol, pt, sizeof pt, &d, &piv), 0);
    oz_coap_encode(&d, re, sizeof re, &rl);
    expect("C.7 decrypted", re, rl, "64455d1f00003974ff48656c6c6f20576f726c6421");
    expect_int("C.7 has no PIV", piv, -1);

    srv.ssn = 0;
    oz_oscore_protect_resp(&srv, &xs, &m, 1, out, sizeof out, &ol);
    expect("C.8 response with PIV", out, ol,
           "64445d1f00003974920100ff4d4c13669384b67354b2b6175ff4b8658c666a6cf88e");
    expect_int("client verifies C.8", oz_oscore_verify_resp(&cli, &xc, out, ol, pt, sizeof pt, &d, &piv), 0);
    expect_int("C.8 PIV", piv, 0);

    /* C.5: no Master Salt, Sender ID 0x00. */
    oz_oscore_derive(&cli, SECRET, 16, NULL, 0, &ID00, 1, &ID01, 1, NULL, 0);
    oz_oscore_derive(&srv, SECRET, 16, NULL, 0, &ID01, 1, &ID00, 1, NULL, 0);
    request_vector("C.5 request", &cli, &srv,
                   "440171c30000b932396c6f63616c686f737483747631",
                   "440271c30000b932396c6f63616c686f737463091400ff4ed339a5a379b0b8bc731fffb0", &xs);

    /* C.6: ID Context sent as 'kid context'. */
    oz_oscore_derive(&cli, SECRET, 16, SALT, 8, NULL, 0, &ID01, 1, IDCTX, 8);
    cli.send_id_ctx = 1;
    oz_oscore_derive(&srv, SECRET, 16, SALT, 8, &ID01, 1, NULL, 0, IDCTX, 8);
    request_vector("C.6 request", &cli, &srv,
                   "44012f8eef9bbf7a396c6f63616c686f737483747631",
                   "44022f8eef9bbf7a396c6f63616c686f73746b19140837cbf3210017a2d3ff"
                   "72cd7273fd331ac45cffbe55c3", &xs);
}

/* ── behaviour beyond the vectors ───────────────────────────────────────── */

static uint64_t stored;
static int persist_calls;
static int persist(void *app, uint64_t v)
{
    (void)app;
    stored = v;
    persist_calls++;
    return 0;
}

static size_t make_req(oz_oscore_ctx *cli, const char *path, uint8_t *out, size_t cap,
                       oz_oscore_xchg *x)
{
    oz_coap_msg m;
    oz_coap_init(&m, OZ_COAP_CON, OZ_COAP_POST, 0x1234);
    m.tkl = 2; m.token[0] = 0xaa; m.token[1] = 0xbb;
    oz_coap_set_path(&m, path);
    static const uint8_t body[] = { 0xa1, 0x00, 0x05 };   /* {0: 5} */
    m.payload = body;
    m.payload_len = sizeof body;
    size_t n = 0;
    if (oz_oscore_protect_req(cli, &m, out, cap, &n, x))
        return 0;
    return n;
}

static void test_behaviour(void)
{
    oz_oscore_ctx cli, srv;
    oz_oscore_xchg xc, xs;
    uint8_t r1[128], r2[128], pt[128];
    oz_coap_msg d;
    static const uint8_t lock_id = 0xab, app_id = 0x05;
    oz_oscore_derive(&cli, SECRET, 16, SALT, 8, &lock_id, 1, &app_id, 1, NULL, 0);
    oz_oscore_derive(&srv, SECRET, 16, SALT, 8, &app_id, 1, &lock_id, 1, NULL, 0);

    size_t n1 = make_req(&cli, "lock/unlock", r1, sizeof r1, &xc);
    size_t n2 = make_req(&cli, "lock/state", r2, sizeof r2, &xc);
    expect_int("req 2 verifies first (reordered)", oz_oscore_verify_req(&srv, r2, n2, pt, sizeof pt, &d, &xs), 0);
    expect_int("req 1 verifies late, inside window", oz_oscore_verify_req(&srv, r1, n1, pt, sizeof pt, &d, &xs), 0);
    char path[32];
    oz_coap_path(&d, path, sizeof path);
    expect("decrypted Uri-Path", (const uint8_t *)path, strlen(path), "6c6f636b2f756e6c6f636b");
    expect_int("req 1 replay refused", oz_oscore_verify_req(&srv, r1, n1, pt, sizeof pt, &d, &xs),
               OZ_OSCORE_E_REPLAY);

    /* 40 more requests: req 1 is now older than the 32-entry window. */
    for (int i = 0; i < 40; i++) {
        uint8_t r[128];
        size_t n = make_req(&cli, "lock/state", r, sizeof r, &xc);
        oz_oscore_verify_req(&srv, r, n, pt, sizeof pt, &d, &xs);
    }
    expect_int("old request refused", oz_oscore_verify_req(&srv, r2, n2, pt, sizeof pt, &d, &xs),
               OZ_OSCORE_E_REPLAY);

    /* Tampering: one flipped ciphertext bit. */
    size_t n3 = make_req(&cli, "lock/state", r1, sizeof r1, &xc);
    r1[n3 - 3] ^= 0x10;
    expect_int("tampered request refused", oz_oscore_verify_req(&srv, r1, n3, pt, sizeof pt, &d, &xs),
               OZ_E_AUTH);

    /* Wrong context (kid). */
    oz_oscore_ctx other;
    static const uint8_t other_id = 0x06;
    oz_oscore_derive(&other, SECRET, 16, SALT, 8, &app_id, 1, &other_id, 1, NULL, 0);
    n3 = make_req(&cli, "lock/state", r1, sizeof r1, &xc);
    expect_int("kid mismatch", oz_oscore_verify_req(&other, r1, n3, pt, sizeof pt, &d, &xs), OZ_OSCORE_E_KID);
    const uint8_t *kid, *ctxp;
    size_t kl, cl;
    expect_int("peek kid", oz_oscore_peek_kid(r1, n3, &kid, &kl, &ctxp, &cl), 0);
    expect("peeked kid", kid, kl, "ab");

    /* B.1.2: after a lock reboot the window is unknown. */
    srv.rw_state = OZ_OSCORE_RW_UNKNOWN;
    n3 = make_req(&cli, "lock/unlock", r1, sizeof r1, &xc);
    expect_int("unknown window: decrypts", oz_oscore_verify_req(&srv, r1, n3, pt, sizeof pt, &d, &xs), 0);
    expect_int("unknown window: flagged", xs.replay_unknown, 1);
    oz_coap_msg e;
    oz_coap_init(&e, OZ_COAP_ACK, OZ_COAP_UNAUTHORIZED, d.mid);
    static const uint8_t echo[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    oz_coap_add_opt(&e, OZ_COAP_OPT_ECHO, echo, sizeof echo);
    uint8_t er[128], ep[128];
    size_t erl;
    expect_int("4.01 + Echo with PIV", oz_oscore_protect_resp(&srv, &xs, &e, 1, er, sizeof er, &erl), 0);
    oz_coap_msg ed;
    int64_t piv;
    expect_int("client reads Echo", oz_oscore_verify_resp(&cli, &xc, er, erl, ep, sizeof ep, &ed, &piv), 0);
    expect_int("Echo response code", ed.code, OZ_COAP_UNAUTHORIZED);
    const oz_coap_opt *eo = oz_coap_find(&ed, OZ_COAP_OPT_ECHO);
    expect_int("Echo value back", eo && eo->len == 8 && !memcmp(eo->val, echo, 8), 1);
    expect_int("Echo response has PIV", piv >= 0, 1);
    n3 = make_req(&cli, "lock/unlock", r1, sizeof r1, &xc);
    oz_oscore_verify_req(&srv, r1, n3, pt, sizeof pt, &d, &xs);
    oz_oscore_window_start(&srv, xs.seq);
    expect_int("window active again", srv.rw_state, OZ_OSCORE_RW_ACTIVE);
    expect_int("the Echo'd request can't be replayed",
               oz_oscore_verify_req(&srv, r1, n3, pt, sizeof pt, &d, &xs), OZ_OSCORE_E_REPLAY);

    /* B.1.1: sequence numbers reserved in blocks of 16. */
    oz_oscore_derive(&cli, SECRET, 16, SALT, 8, &lock_id, 1, &app_id, 1, NULL, 0);
    cli.persist = persist;
    cli.block = 16;
    for (int i = 0; i < 40; i++)
        make_req(&cli, "x", r1, sizeof r1, &xc);
    expect_int("persist called per block", persist_calls, 3);
    expect_int("stored = end of the current block", (long long)stored, 48);
    /* "Reboot": restore from the stored value; no number below it is reused. */
    cli.ssn = stored;
    cli.ssn_limit = stored;
    make_req(&cli, "x", r1, sizeof r1, &xc);
    expect_int("after reboot, first PIV = stored", (long long)xc.seq, 48);

    /* Exhaustion. */
    cli.persist = NULL;
    cli.ssn = OZ_OSCORE_SSN_MAX + 1;
    oz_coap_msg m;
    oz_coap_init(&m, OZ_COAP_CON, OZ_COAP_GET, 1);
    size_t ol;
    expect_int("sequence space exhausted", oz_oscore_protect_req(&cli, &m, r1, sizeof r1, &ol, &xc),
               OZ_OSCORE_E_EXHAUSTED);
}

int main(void)
{
    test_derivation();
    test_vectors();
    test_behaviour();
    return report("oscore");
}
