/* EDHOC tests.
 *
 * 1. RFC 9529 §3, the static-DH trace (method 3), byte for byte. That trace
 *    uses cipher suite 2, which is suite 0 with P-256 instead of X25519: same
 *    AEAD, hash, MAC length and key schedule. So the trace's ECDH outputs are
 *    injected through the dh hook and everything else runs as in production.
 * 2. A real suite-0 X25519 handshake: kid for the lock, kccs by value for an
 *    unbonded app, EAD_3 delivered, both sides agree on OSCORE keys and the
 *    pairing code.
 * 3. Failure paths: tampering, unknown credential, wrong suite, low-order key.
 */
#include "oz_edhoc.h"
#include "oz_cbor.h"
#include "oz_port_host.h"
#include "oz_test.h"

/* ── 1. RFC 9529 §3 ─────────────────────────────────────────────────────── */

static struct { uint8_t x[32], g_x[32], y[32], g_y[32], sk_r[32], g_r[32], sk_i[32], g_i[32],
                        g_xy[32], g_rx[32], g_iy[32]; } T;
static uint8_t cred_r[95], cred_i[107], idc_r[4], idc_i[4];

static int trace_dh(void *app, uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32])
{
    (void)app;
    const struct { const uint8_t *priv, *pub, *shared; } tab[] = {
        { T.x, T.g_y, T.g_xy }, { T.y, T.g_x, T.g_xy },
        { T.sk_r, T.g_x, T.g_rx }, { T.x, T.g_r, T.g_rx },
        { T.sk_i, T.g_y, T.g_iy }, { T.y, T.g_i, T.g_iy },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!memcmp(priv, tab[i].priv, 32) && !memcmp(pub, tab[i].pub, 32)) {
            memcpy(out, tab[i].shared, 32);
            return 0;
        }
    printf("trace_dh: unexpected key pair\n");
    return OZ_E_ARG;
}

static int trace_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    (void)app;
    if (len == 4 && !memcmp(id, idc_r, 4)) {
        out->cred = cred_r; out->cred_len = sizeof cred_r;
        memcpy(out->pub, T.g_r, 32);
        return 0;
    }
    if (len == 4 && !memcmp(id, idc_i, 4)) {
        out->cred = cred_i; out->cred_len = sizeof cred_i;
        memcpy(out->pub, T.g_i, 32);
        return 0;
    }
    return OZ_E_NOT_FOUND;
}

static void load_trace(void)
{
    unhex("368ec1f69aeb659ba37d5a8d45b21bdc0299dceaa8ef235f3ca42ce3530f9525", T.x, 32);
    unhex("8af6f430ebe18d34184017a9a11bf511c8dff8f834730b96c1b7c8dbca2fc3b6", T.g_x, 32);
    unhex("e2f4126777205e853b437d6eaca1e1f753cdcc3e2c69fa884b0a1a640977e418", T.y, 32);
    unhex("419701d7f00a26c2dc587a36dd752549f33763c893422c8ea0f955a13a4ff5d5", T.g_y, 32);
    unhex("72cc4761dbd4c78f758931aa589d348d1ef874a7e303ede2f140dcf3e6aa4aac", T.sk_r, 32);
    unhex("bbc34960526ea4d32e940cad2a234148ddc21791a12afbcbac93622046dd44f0", T.g_r, 32);
    unhex("fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b", T.sk_i, 32);
    unhex("ac75e9ece3e50bfc8ed60399889522405c47bf16df96660a41298cb4307f7eb6", T.g_i, 32);
    unhex("2f0cb7e860ba538fbf5c8bded009f6259b4b628fe1eb7dbe9378e5ecf7a824ba", T.g_xy, 32);
    unhex("f2b6eea02220b95eee5a0bc701f074e00a843ea02422f60825fb269b3e161423", T.g_rx, 32);
    unhex("080f425085bc6249089eac8f108ea62326857e12ab07d72028ca1b5f36e004b3", T.g_iy, 32);
    unhex("a2026b6578616d706c652e65647508a101a501020241322001215820bbc34960526ea4d32e940cad"
          "2a234148ddc21791a12afbcbac93622046dd44f02258204519e257236b2a0ce2023f0931f1f386ca"
          "7afda64fcde0108c224c51eabf6072", cred_r, sizeof cred_r);
    unhex("a2027734322d35302d33312d46462d45462d33372d33322d333908a101a5010202412b2001215820"
          "ac75e9ece3e50bfc8ed60399889522405c47bf16df96660a41298cb4307f7eb62258206e5de61138"
          "8a4b8a8211334ac7d37ecb52a387d257e6db3c2a93df21ff3affc8", cred_i, sizeof cred_i);
    unhex("a1044132", idc_r, 4);
    unhex("a104412b", idc_i, 4);
}

static oz_edhoc I, R;   /* ~3 KB each: keep them off the stack */

static void test_rfc9529_trace(void)
{
    load_trace();
    uint8_t m[256], e[64];
    size_t ml, el;

    /* First attempt: I offers only suite 6, R supports only 2 → error (02, 02). */
    oz_edhoc_init(&I, 1);
    I.n_suites = 1; I.suites[0] = 6;
    I.c_own.len = 1; I.c_own.b[0] = 0x37;
    I.eph_set = 1; memcpy(I.eph_priv, T.x, 32); memcpy(I.eph_pub, T.g_x, 32);
    oz_edhoc_init(&R, 0);
    R.n_suites = 1; R.suites[0] = 2;
    oz_edhoc_i_msg1(&I, NULL, 0, m, sizeof m, &ml);
    int rc = oz_edhoc_r_msg1(&R, m, ml, NULL, 0, e, sizeof e, &el);
    expect_int("trace: suite 6 refused", rc, OZ_EDHOC_SEND_ERROR);
    expect("trace: error SUITES_R", e, el, "0202");

    /* Second attempt with the trace's keys. */
    oz_edhoc_init(&I, 1);
    I.n_suites = 2; I.suites[0] = 6; I.suites[1] = 2;
    I.c_own.len = 1; I.c_own.b[0] = 0x37;
    I.own.cred = cred_i; I.own.cred_len = sizeof cred_i;
    I.own.id_cred = idc_i; I.own.id_cred_len = 4;
    memcpy(I.own_priv, T.sk_i, 32);
    I.eph_set = 1; memcpy(I.eph_priv, T.x, 32); memcpy(I.eph_pub, T.g_x, 32);
    I.get_cred = trace_cred; I.dh = trace_dh;

    oz_edhoc_init(&R, 0);
    R.n_suites = 1; R.suites[0] = 2;
    R.c_own.len = 1; R.c_own.b[0] = 0x27;
    R.own.cred = cred_r; R.own.cred_len = sizeof cred_r;
    R.own.id_cred = idc_r; R.own.id_cred_len = 4;
    memcpy(R.own_priv, T.sk_r, 32);
    R.eph_set = 1; memcpy(R.eph_priv, T.y, 32); memcpy(R.eph_pub, T.g_y, 32);
    R.get_cred = trace_cred; R.dh = trace_dh;

    rc = oz_edhoc_i_msg1(&I, NULL, 0, m, sizeof m, &ml);
    expect_int("trace: msg1 rc", rc, 0);
    expect("trace: message_1", m, ml,
           "0382060258208af6f430ebe18d34184017a9a11bf511c8dff8f834730b96c1b7c8dbca2fc3b637");

    uint8_t m2[256];
    size_t m2l;
    rc = oz_edhoc_r_msg1(&R, m, ml, NULL, 0, m2, sizeof m2, &m2l);
    expect_int("trace: msg2 rc", rc, 0);
    expect("trace: message_2", m2, m2l,
           "582b419701d7f00a26c2dc587a36dd752549f33763c893422c8ea0f955a13a4ff5d5"
           "9862a1eef9e0e7e1886fcd");

    rc = oz_edhoc_i_msg2(&I, m2, m2l, e, sizeof e, &el);
    expect_int("trace: I accepts message_2", rc, 0);
    expect("trace: C_R seen by I", I.c_peer.b, I.c_peer.len, "27");

    uint8_t m3[256];
    size_t m3l;
    rc = oz_edhoc_i_msg3(&I, NULL, 0, m3, sizeof m3, &m3l);
    expect_int("trace: msg3 rc", rc, 0);
    expect("trace: message_3", m3, m3l, "52e562097bc417dd5919485ac7891ffd90a9fc");

    rc = oz_edhoc_r_msg3(&R, m3, m3l, e, sizeof e, &el);
    expect_int("trace: R accepts message_3", rc, 0);
    expect("trace: I's credential seen by R", R.peer.cred, R.peer.cred_len,
           "a2027734322d35302d33312d46462d45462d33372d33322d333908a101a5010202412b2001215820"
           "ac75e9ece3e50bfc8ed60399889522405c47bf16df96660a41298cb4307f7eb62258206e5de61138"
           "8a4b8a8211334ac7d37ecb52a387d257e6db3c2a93df21ff3affc8");

    expect("trace: PRK_exporter (I)", I.prk_exporter, 32,
           "e14d06699cee248c5a04bf9227bbcd4ce394de7dcb56db43555474171e6446db");
    expect("trace: PRK_exporter (R)", R.prk_exporter, 32,
           "e14d06699cee248c5a04bf9227bbcd4ce394de7dcb56db43555474171e6446db");

    uint8_t sec[16], salt[8];
    oz_edhoc_cid sid, rid;
    oz_edhoc_oscore(&I, sec, salt, &sid, &rid);
    expect("trace: OSCORE Master Secret", sec, 16, "f9868f6a3aca78a05d1485b35030b162");
    expect("trace: OSCORE Master Salt", salt, 8, "ada24c7dbfc85eeb");
    expect("trace: client Sender ID", sid.b, sid.len, "27");
    expect("trace: client Recipient ID", rid.b, rid.len, "37");
    oz_edhoc_oscore(&R, sec, salt, &sid, &rid);
    expect("trace: server Master Secret", sec, 16, "f9868f6a3aca78a05d1485b35030b162");
    expect("trace: server Sender ID", sid.b, sid.len, "37");
}

/* ── 2. Real suite-0 handshake ──────────────────────────────────────────── */

typedef struct { uint8_t priv[32], pub[32], ccs[96], idc[8]; size_t ccs_len, idc_len; } party;
static party lock_p, app_p, stranger_p;
static uint8_t app_kccs[128];
static size_t app_kccs_len;

static void make_party(party *p, uint8_t kid, const char *sub)
{
    oz_port_random(p->priv, 32);
    oz_port_x25519_public_key(p->pub, p->priv);
    oz_cbor_w w;
    oz_cbor_w_init(&w, p->ccs, sizeof p->ccs);
    oz_cbor_put_map(&w, 2);
    oz_cbor_put_uint(&w, 2); oz_cbor_put_tstr(&w, sub, strlen(sub));
    oz_cbor_put_uint(&w, 8); oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_map(&w, 4);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, 1);          /* kty OKP */
    oz_cbor_put_uint(&w, 2); oz_cbor_put_bstr(&w, &kid, 1);    /* kid */
    oz_cbor_put_int(&w, -1); oz_cbor_put_uint(&w, 4);          /* crv X25519 */
    oz_cbor_put_int(&w, -2); oz_cbor_put_bstr(&w, p->pub, 32); /* x */
    p->ccs_len = w.len;
    oz_cbor_w_init(&w, p->idc, sizeof p->idc);
    oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 4); oz_cbor_put_bstr(&w, &kid, 1);
    p->idc_len = w.len;
}

/* The app knows the lock by kid; the lock knows no one and accepts kccs. */
static int app_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    (void)app;
    if (len != lock_p.idc_len || memcmp(id, lock_p.idc, len))
        return OZ_E_NOT_FOUND;
    out->cred = lock_p.ccs; out->cred_len = lock_p.ccs_len;
    return oz_edhoc_ccs_x25519(lock_p.ccs, lock_p.ccs_len, out->pub);
}

static int lock_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    (void)app;
    return oz_edhoc_cred_from_kccs(id, len, out);
}

static void setup_pair(void)
{
    oz_edhoc_init(&I, 1);
    I.c_own.len = 1; I.c_own.b[0] = 0x05;
    I.own.cred = app_p.ccs; I.own.cred_len = app_p.ccs_len;
    I.own.id_cred = app_kccs; I.own.id_cred_len = app_kccs_len;
    memcpy(I.own_priv, app_p.priv, 32);
    I.get_cred = app_cred;

    oz_edhoc_init(&R, 0);
    R.c_own.len = 2; R.c_own.b[0] = 0xab; R.c_own.b[1] = 0xcd;
    R.own.cred = lock_p.ccs; R.own.cred_len = lock_p.ccs_len;
    R.own.id_cred = lock_p.idc; R.own.id_cred_len = lock_p.idc_len;
    memcpy(R.own_priv, lock_p.priv, 32);
    R.get_cred = lock_cred;
}

static uint8_t m1[1024], m2[1024], m3[1024], er[128];
static size_t m1l, m2l, m3l, erl;

static void test_x25519_roundtrip(void)
{
    make_party(&lock_p, 0x32, "lock");
    make_party(&app_p, 0x01, "app");
    make_party(&stranger_p, 0x33, "stranger");
    oz_cbor_w w;
    oz_cbor_w_init(&w, app_kccs, sizeof app_kccs);
    oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 14);
    oz_cbor_put_raw(&w, app_p.ccs, app_p.ccs_len);
    app_kccs_len = w.len;

    setup_pair();
    /* EAD_3: one critical item with a placeholder value, as an invite would ride. */
    uint8_t ead3[40];
    oz_cbor_w_init(&w, ead3, sizeof ead3);
    oz_cbor_put_int(&w, -20297);
    oz_cbor_put_bstr(&w, (const uint8_t *)"OZINV-test-payload", 18);
    size_t ead3_len = w.len;

    expect_int("x25519: msg1", oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l), 0);
    expect_int("x25519: msg1 size", (long long)m1l, 37);
    expect_int("x25519: msg2", oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l), 0);
    expect_int("x25519: I accepts", oz_edhoc_i_msg2(&I, m2, m2l, er, sizeof er, &erl), 0);
    expect_int("x25519: msg3", oz_edhoc_i_msg3(&I, ead3, ead3_len, m3, sizeof m3, &m3l), 0);
    expect_int("x25519: R accepts", oz_edhoc_r_msg3(&R, m3, m3l, er, sizeof er, &erl), 0);
    printf("x25519 handshake sizes: msg1 %zu, msg2 %zu, msg3 %zu (kccs + %zu-byte EAD_3)\n",
           m1l, m2l, m3l, ead3_len);

    expect_int("x25519: R learnt the app key", memcmp(R.peer.pub, app_p.pub, 32), 0);
    expect_int("x25519: EAD_3 delivered", (long long)R.ead_len, (long long)ead3_len);
    expect_int("x25519: EAD_3 bytes", R.ead ? memcmp(R.ead, ead3, ead3_len) : -1, 0);

    uint8_t si[16], sr[16], ai[8], ar[8];
    oz_edhoc_cid sid_i, rid_i, sid_r, rid_r;
    oz_edhoc_oscore(&I, si, ai, &sid_i, &rid_i);
    oz_edhoc_oscore(&R, sr, ar, &sid_r, &rid_r);
    expect_int("x25519: same master secret", memcmp(si, sr, 16), 0);
    expect_int("x25519: same master salt", memcmp(ai, ar, 8), 0);
    expect_int("x25519: IDs mirror", sid_i.len == rid_r.len && !memcmp(sid_i.b, rid_r.b, sid_i.len), 1);

    uint32_t ci, cr;
    oz_edhoc_pair_code(&I, &ci);
    oz_edhoc_pair_code(&R, &cr);
    expect_int("x25519: same pairing code", ci, cr);
    expect_int("x25519: pairing code < 10^6", ci < 1000000u, 1);
}

/* ── 3. Failure paths ───────────────────────────────────────────────────── */

static void test_failures(void)
{
    /* Tampered message_2 → I refuses. */
    setup_pair();
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l);
    m2[m2l - 1] ^= 0x01;
    expect_int("tampered msg2 refused", oz_edhoc_i_msg2(&I, m2, m2l, er, sizeof er, &erl),
               OZ_EDHOC_SEND_ERROR);

    /* Tampered message_3 → R refuses, and nothing is exported. */
    setup_pair();
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l);
    oz_edhoc_i_msg2(&I, m2, m2l, er, sizeof er, &erl);
    oz_edhoc_i_msg3(&I, NULL, 0, m3, sizeof m3, &m3l);
    m3[5] ^= 0x80;
    expect_int("tampered msg3 refused", oz_edhoc_r_msg3(&R, m3, m3l, er, sizeof er, &erl),
               OZ_EDHOC_SEND_ERROR);
    uint8_t k[16];
    expect_int("no export after failure", oz_edhoc_exporter(&R, 0, NULL, 0, k, 16), OZ_E_ARG);

    /* A different lock answering under the expected kid: MAC_2 fails. */
    setup_pair();
    R.own.cred = stranger_p.ccs; R.own.cred_len = stranger_p.ccs_len;
    memcpy(R.own_priv, stranger_p.priv, 32);
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l);
    expect_int("impostor lock refused", oz_edhoc_i_msg2(&I, m2, m2l, er, sizeof er, &erl),
               OZ_EDHOC_SEND_ERROR);

    /* Unknown lock kid → error code 3. */
    setup_pair();
    R.own.id_cred = stranger_p.idc; R.own.id_cred_len = stranger_p.idc_len;
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l);
    expect_int("unknown cred", oz_edhoc_i_msg2(&I, m2, m2l, er, sizeof er, &erl),
               OZ_EDHOC_SEND_ERROR);
    expect("unknown cred error", er, erl, "03f5");

    /* Method 0 offered → refused. */
    setup_pair();
    I.method = 0;
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    expect_int("method 0 refused", oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l),
               OZ_EDHOC_SEND_ERROR);

    /* Low-order (all-zero) ephemeral key → abort without an answer. */
    setup_pair();
    oz_edhoc_i_msg1(&I, NULL, 0, m1, sizeof m1, &m1l);
    memset(m1 + 4, 0, 32);   /* 00 00 58 20 | G_X */
    expect_int("low-order G_X", oz_edhoc_r_msg1(&R, m1, m1l, NULL, 0, m2, sizeof m2, &m2l) < 0, 1);

    /* Identifier rule: a bstr that should have been an int is malformed. */
    setup_pair();
    static const uint8_t bad1[] = { 0x03, 0x00, 0x58, 0x20,
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
        0x41, 0x05 };
    expect_int("C_I as bstr h'05' refused",
               oz_edhoc_r_msg1(&R, bad1, sizeof bad1, NULL, 0, m2, sizeof m2, &m2l),
               OZ_EDHOC_SEND_ERROR);

    /* Out-of-order calls. */
    setup_pair();
    expect_int("msg3 before msg2", oz_edhoc_i_msg3(&I, NULL, 0, m3, sizeof m3, &m3l), OZ_E_ARG);
}

int main(void)
{
    test_rfc9529_trace();
    test_x25519_roundtrip();
    test_failures();
    return report("edhoc");
}
