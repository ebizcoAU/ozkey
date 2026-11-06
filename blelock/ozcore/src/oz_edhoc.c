/* EDHOC method 3 (RFC 9528), initiator and responder. Section numbers below
 * are RFC 9528's. */
#include <string.h>
#include "oz_edhoc.h"
#include "oz_cbor.h"

enum {
    ST_INIT = 0,
    ST_I_SENT1, ST_I_GOT2, ST_R_SENT2, ST_DONE,
    ST_FAILED = 0xff,
};

#define MAC_LEN   8
#define KEY_LEN  16
#define IV_LEN   13

/* ── small helpers ──────────────────────────────────────────────────────── */

static int fail(oz_edhoc *s, int rc)
{
    s->state = ST_FAILED;
    return rc;
}

/* An EDHOC error the caller should send; the session is dead either way. */
static int send_err(oz_edhoc *s, int code, const char *text,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    s->state = ST_FAILED;
    *out_len = oz_edhoc_error_msg(code, text, out, cap);
    return *out_len ? OZ_EDHOC_SEND_ERROR : OZ_E_NO_SPACE;
}

static int dh(oz_edhoc *s, uint8_t shared[32], const uint8_t priv[32], const uint8_t pub[32])
{
    if (s->dh)
        return s->dh(s->app, shared, priv, pub);
    int rc = oz_port_x25519(shared, priv, pub);
    if (rc)
        return rc;
    /* A low-order peer point gives all zeros (RFC 7748 §6.1): refuse it. */
    uint8_t z = 0;
    for (int i = 0; i < 32; i++)
        z |= shared[i];
    return z ? OZ_OK : OZ_E_ARG;
}

/* Byte-string identifiers that equal a one-byte CBOR int travel as that int
 * (§3.3.2). */
static int ident_is_int(const uint8_t *b, size_t n)
{
    return n == 1 && (b[0] <= 0x17 || (b[0] >= 0x20 && b[0] <= 0x37));
}

static void put_ident(oz_cbor_w *w, const uint8_t *b, size_t n)
{
    if (ident_is_int(b, n))
        oz_cbor_put_raw(w, b, 1);
    else
        oz_cbor_put_bstr(w, b, n);
}

static int get_ident(oz_cbor_r *r, uint8_t *out, size_t cap, size_t *n)
{
    int m = oz_cbor_peek(r);
    if (m == OZ_CBOR_UINT || m == OZ_CBOR_NINT) {
        uint8_t ib = r->p[r->pos];
        if ((ib & 0x1f) >= 24)
            return OZ_E_ARG;          /* only one-byte ints are identifiers */
        r->pos++;
        out[0] = ib;
        *n = 1;
        return OZ_OK;
    }
    size_t len;
    const uint8_t *p = oz_cbor_get_bstr(r, &len);
    if (!p || len > cap || ident_is_int(p, len))
        return OZ_E_ARG;              /* §3.3.2: such a bstr MUST be an int */
    memcpy(out, p, len);
    *n = len;
    return OZ_OK;
}

/* ID_CRED in PLAINTEXT_2/3: compact 'kid' (§3.5.3.2) or a map. */
static int own_kid(const oz_edhoc_cred *c, const uint8_t **kid, size_t *kid_len)
{
    oz_cbor_r r;
    oz_cbor_r_init(&r, c->id_cred, c->id_cred_len);
    if (oz_cbor_get_map(&r) != 1 || r.err)
        return 0;
    if (oz_cbor_peek(&r) != OZ_CBOR_UINT || oz_cbor_get_uint(&r) != 4)
        return 0;
    if (oz_cbor_peek(&r) != OZ_CBOR_BSTR)
        return 0;
    *kid = oz_cbor_get_bstr(&r, kid_len);
    return *kid && oz_cbor_at_end(&r);
}

static void put_id_cred_compact(oz_cbor_w *w, const oz_edhoc_cred *c)
{
    const uint8_t *kid;
    size_t kl;
    if (own_kid(c, &kid, &kl))
        put_ident(w, kid, kl);
    else
        oz_cbor_put_raw(w, c->id_cred, c->id_cred_len);
}

/* Read the peer's ID_CRED and rebuild its full map form into s->idc. */
static int get_id_cred(oz_edhoc *s, oz_cbor_r *r, size_t *idc_len)
{
    int m = oz_cbor_peek(r);
    if (m == OZ_CBOR_MAP) {
        size_t n;
        const uint8_t *p = oz_cbor_skip(r, &n);
        if (!p || n > sizeof s->idc)
            return OZ_E_ARG;
        memcpy(s->idc, p, n);
        *idc_len = n;
        return OZ_OK;
    }
    uint8_t kid[OZ_EDHOC_MAX_IDCRED - 8];
    size_t kl;
    if (get_ident(r, kid, sizeof kid, &kl))
        return OZ_E_ARG;
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->idc, sizeof s->idc);
    oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 4);
    oz_cbor_put_bstr(&w, kid, kl);
    *idc_len = w.len;
    return w.err;
}

/* EAD = 1* (label int, ? value bstr): check the shape, return the raw run. */
static int get_ead(oz_cbor_r *r, const uint8_t **ead, size_t *len)
{
    size_t start = r->pos;
    while (!r->err && r->pos < r->len) {
        int m = oz_cbor_peek(r);
        if (m != OZ_CBOR_UINT && m != OZ_CBOR_NINT)
            return OZ_E_ARG;
        (void)oz_cbor_get_int(r);
        if (oz_cbor_peek(r) == OZ_CBOR_BSTR) {
            size_t n;
            (void)oz_cbor_get_bstr(r, &n);
        }
    }
    if (r->err)
        return OZ_E_ARG;
    *ead = r->pos > start ? r->p + start : NULL;
    *len = r->pos - start;
    return OZ_OK;
}

/* EDHOC_KDF (§4.1.2): info = (label, context bstr, length) built in s->b, so
 * ctx must not live in s->b. */
static int kdf(oz_edhoc *s, const uint8_t prk[32], uint32_t label,
               const uint8_t *ctx, size_t ctx_len, uint8_t *out, size_t len)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->b, sizeof s->b);
    oz_cbor_put_uint(&w, label);
    oz_cbor_put_bstr(&w, ctx, ctx_len);
    oz_cbor_put_uint(&w, len);
    if (w.err)
        return w.err;
    return oz_hkdf_expand(prk, s->b, w.len, out, len);
}

/* TH = H( bstr th_prev, raw, cred ) — the TH_3 / TH_4 shape. */
static int transcript(oz_edhoc *s, const uint8_t *raw, size_t raw_len,
                      const uint8_t *cred, size_t cred_len)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->b, sizeof s->b);
    oz_cbor_put_bstr(&w, s->th, 32);
    oz_cbor_put_raw(&w, raw, raw_len);
    oz_cbor_put_raw(&w, cred, cred_len);
    if (w.err)
        return w.err;
    return oz_sha256(s->b, w.len, s->th);
}

/* TH_2 = H( bstr G_Y, bstr H(message_1) ); s->th holds H(message_1). */
static int th2(oz_edhoc *s, const uint8_t g_y[32])
{
    uint8_t in[68];
    in[0] = 0x58; in[1] = 32;
    memcpy(in + 2, g_y, 32);
    in[34] = 0x58; in[35] = 32;
    memcpy(in + 36, s->th, 32);
    return oz_sha256(in, sizeof in, s->th);
}

/* PRK_2e and PRK_3e2m (§4.1.1.1-2) once TH_2 is in s->th. */
static int prk_2e_3e2m(oz_edhoc *s, const uint8_t g_xy[32], const uint8_t g_rx[32])
{
    uint8_t salt[32];
    int rc = oz_hkdf_extract(s->th, 32, g_xy, 32, s->prk_2e);
    if (!rc) rc = kdf(s, s->prk_2e, 1, s->th, 32, salt, 32);
    if (!rc) rc = oz_hkdf_extract(salt, 32, g_rx, 32, s->prk_3e2m);
    oz_wipe(salt, sizeof salt);
    return rc;
}

/* PRK_4e3m (§4.1.1.3) once TH_3 is in s->th. */
static int prk_4e3m(oz_edhoc *s, const uint8_t g_iy[32])
{
    uint8_t salt[32];
    int rc = kdf(s, s->prk_3e2m, 5, s->th, 32, salt, 32);
    if (!rc) rc = oz_hkdf_extract(salt, 32, g_iy, 32, s->prk_4e3m);
    oz_wipe(salt, sizeof salt);
    return rc;
}

/* MAC_2: context_2 = << C_R, ID_CRED_R, TH_2, CRED_R, ? EAD_2 >> (§5.3.2). */
static int mac2(oz_edhoc *s, const uint8_t *c_r, size_t c_r_len, const oz_edhoc_cred *cred_r,
                const uint8_t *ead, size_t ead_len, uint8_t mac[MAC_LEN])
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->a, sizeof s->a);
    put_ident(&w, c_r, c_r_len);
    oz_cbor_put_raw(&w, cred_r->id_cred, cred_r->id_cred_len);
    oz_cbor_put_bstr(&w, s->th, 32);
    oz_cbor_put_raw(&w, cred_r->cred, cred_r->cred_len);
    oz_cbor_put_raw(&w, ead, ead_len);
    if (w.err)
        return w.err;
    return kdf(s, s->prk_3e2m, 2, s->a, w.len, mac, MAC_LEN);
}

/* MAC_3: context_3 = << ID_CRED_I, TH_3, CRED_I, ? EAD_3 >> (§5.4.2). */
static int mac3(oz_edhoc *s, const oz_edhoc_cred *cred_i,
                const uint8_t *ead, size_t ead_len, uint8_t mac[MAC_LEN])
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->a, sizeof s->a);
    oz_cbor_put_raw(&w, cred_i->id_cred, cred_i->id_cred_len);
    oz_cbor_put_bstr(&w, s->th, 32);
    oz_cbor_put_raw(&w, cred_i->cred, cred_i->cred_len);
    oz_cbor_put_raw(&w, ead, ead_len);
    if (w.err)
        return w.err;
    return kdf(s, s->prk_4e3m, 6, s->a, w.len, mac, MAC_LEN);
}

/* K_3, IV_3 and A_3 = ["Encrypt0", h'', TH_3] (§5.4.2). */
static int k3_iv3(oz_edhoc *s, uint8_t k[KEY_LEN], uint8_t iv[IV_LEN], uint8_t aad[45])
{
    int rc = kdf(s, s->prk_3e2m, 3, s->th, 32, k, KEY_LEN);
    if (!rc) rc = kdf(s, s->prk_3e2m, 4, s->th, 32, iv, IV_LEN);
    static const uint8_t hdr[13] = { 0x83, 0x68, 'E', 'n', 'c', 'r', 'y', 'p', 't', '0',
                                     0x40, 0x58, 0x20 };
    memcpy(aad, hdr, sizeof hdr);
    memcpy(aad + 13, s->th, 32);
    return rc;
}

/* TH_4 is in s->th: derive PRK_exporter (§4.1.3) and drop the handshake keys. */
static int finish(oz_edhoc *s)
{
    uint8_t prk_out[32];
    int rc = kdf(s, s->prk_4e3m, 7, s->th, 32, prk_out, 32);
    if (!rc) rc = kdf(s, prk_out, 10, NULL, 0, s->prk_exporter, 32);
    oz_wipe(prk_out, sizeof prk_out);
    oz_wipe(s->prk_2e, sizeof s->prk_2e);
    oz_wipe(s->prk_3e2m, sizeof s->prk_3e2m);
    oz_wipe(s->prk_4e3m, sizeof s->prk_4e3m);
    oz_wipe(s->eph_priv, sizeof s->eph_priv);
    if (rc)
        return fail(s, rc);
    s->state = ST_DONE;
    return OZ_OK;
}

static int make_eph(oz_edhoc *s)
{
    if (s->eph_set)
        return OZ_OK;
    int rc = oz_port_random(s->eph_priv, 32);
    if (!rc) rc = oz_port_x25519_public_key(s->eph_pub, s->eph_priv);
    return rc;
}

static int resolve_peer(oz_edhoc *s, size_t idc_len)
{
    if (!s->get_cred)
        return OZ_E_NOT_FOUND;
    memset(&s->peer, 0, sizeof s->peer);
    int rc = s->get_cred(s->app, s->idc, idc_len, &s->peer);
    if (rc)
        return rc;
    if (!s->peer.cred || s->peer.cred_len > OZ_EDHOC_MAX_CRED)
        return OZ_E_ARG;
    /* MACs cover ID_CRED as received, whatever the callback returned. */
    s->peer.id_cred = s->idc;
    s->peer.id_cred_len = idc_len;
    return OZ_OK;
}

/* ── public: setup ──────────────────────────────────────────────────────── */

void oz_edhoc_init(oz_edhoc *s, int initiator)
{
    memset(s, 0, sizeof *s);
    s->initiator = initiator;
    s->method = OZ_EDHOC_METHOD_STAT_STAT;
    s->n_suites = 1;
    s->suites[0] = OZ_EDHOC_SUITE_0;
    s->suite = -1;
}

void oz_edhoc_wipe(oz_edhoc *s) { oz_wipe(s, sizeof *s); }

size_t oz_edhoc_error_msg(int code, const char *text, uint8_t *out, size_t cap)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, out, cap);
    oz_cbor_put_int(&w, code);
    if (code == OZ_EDHOC_ERR_UNKNOWN_CRED)
        oz_cbor_put_bool(&w, 1);
    else
        oz_cbor_put_tstr(&w, text ? text : "", text ? strlen(text) : 0);
    return w.err ? 0 : w.len;
}

/* ── public: initiator ──────────────────────────────────────────────────── */

int oz_edhoc_i_msg1(oz_edhoc *s, const uint8_t *ead1, size_t ead1_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    if (!s->initiator || s->state != ST_INIT || !s->n_suites || s->n_suites > OZ_EDHOC_MAX_SUITES)
        return fail(s, OZ_E_ARG);
    int rc = make_eph(s);
    if (rc)
        return fail(s, rc);
    oz_cbor_w w;
    oz_cbor_w_init(&w, out, cap);
    oz_cbor_put_int(&w, s->method);
    if (s->n_suites == 1) {
        oz_cbor_put_int(&w, s->suites[0]);
    } else {
        oz_cbor_put_array(&w, s->n_suites);
        for (int i = 0; i < s->n_suites; i++)
            oz_cbor_put_int(&w, s->suites[i]);
    }
    oz_cbor_put_bstr(&w, s->eph_pub, 32);
    put_ident(&w, s->c_own.b, s->c_own.len);
    oz_cbor_put_raw(&w, ead1, ead1_len);
    if (w.err)
        return fail(s, w.err);
    s->suite = s->suites[s->n_suites - 1];
    rc = oz_sha256(out, w.len, s->th);       /* H(message_1), kept for TH_2 */
    if (rc)
        return fail(s, rc);
    *out_len = w.len;
    s->state = ST_I_SENT1;
    return OZ_OK;
}

int oz_edhoc_i_msg2(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (s->state != ST_I_SENT1)
        return fail(s, OZ_E_ARG);
    oz_cbor_r r;
    oz_cbor_r_init(&r, in, in_len);
    size_t n;
    const uint8_t *gyc = oz_cbor_get_bstr(&r, &n);
    if (!gyc || !oz_cbor_at_end(&r) || n <= 32 || n - 32 > sizeof s->pt)
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "message_2 malformed", out, cap, out_len);
    size_t ct_len = n - 32;
    memcpy(s->peer_eph, gyc, 32);

    uint8_t g_xy[32], g_rx[32], mac[MAC_LEN];
    int rc = th2(s, s->peer_eph);
    if (!rc) rc = dh(s, g_xy, s->eph_priv, s->peer_eph);
    if (!rc) rc = oz_hkdf_extract(s->th, 32, g_xy, 32, s->prk_2e);
    if (!rc) rc = kdf(s, s->prk_2e, 0, s->th, 32, s->a, ct_len);    /* KEYSTREAM_2 */
    oz_wipe(g_xy, sizeof g_xy);
    if (rc)
        return fail(s, rc);
    for (size_t i = 0; i < ct_len; i++)
        s->pt[i] = (uint8_t)(gyc[32 + i] ^ s->a[i]);

    /* PLAINTEXT_2 = ( C_R, ID_CRED_R, Signature_or_MAC_2, ? EAD_2 ) */
    oz_cbor_r_init(&r, s->pt, ct_len);
    size_t cl, idc_len, ml;
    if (get_ident(&r, s->c_peer.b, sizeof s->c_peer.b, &cl) || get_id_cred(s, &r, &idc_len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "plaintext_2 malformed", out, cap, out_len);
    s->c_peer.len = (uint8_t)cl;
    const uint8_t *mac_rx = oz_cbor_get_bstr(&r, &ml);
    if (!mac_rx || ml != MAC_LEN || get_ead(&r, &s->ead, &s->ead_len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "plaintext_2 malformed", out, cap, out_len);
    if (s->c_peer.len == s->c_own.len && !memcmp(s->c_peer.b, s->c_own.b, s->c_own.len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "C_R equals C_I", out, cap, out_len);

    rc = resolve_peer(s, idc_len);
    if (rc == OZ_E_NOT_FOUND)
        return send_err(s, OZ_EDHOC_ERR_UNKNOWN_CRED, NULL, out, cap, out_len);
    if (rc)
        return fail(s, rc);

    uint8_t salt[32];
    rc = kdf(s, s->prk_2e, 1, s->th, 32, salt, 32);
    if (!rc) rc = dh(s, g_rx, s->eph_priv, s->peer.pub);
    if (!rc) rc = oz_hkdf_extract(salt, 32, g_rx, 32, s->prk_3e2m);
    oz_wipe(salt, sizeof salt);
    oz_wipe(g_rx, sizeof g_rx);
    if (!rc) rc = mac2(s, s->c_peer.b, s->c_peer.len, &s->peer, s->ead, s->ead_len, mac);
    if (rc)
        return fail(s, rc);
    if (oz_ct_equal(mac, mac_rx, MAC_LEN))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "MAC_2 mismatch", out, cap, out_len);

    rc = transcript(s, s->pt, ct_len, s->peer.cred, s->peer.cred_len);   /* TH_3 */
    if (rc)
        return fail(s, rc);
    s->state = ST_I_GOT2;
    return OZ_OK;
}

int oz_edhoc_i_msg3(oz_edhoc *s, const uint8_t *ead3, size_t ead3_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    if (s->state != ST_I_GOT2 || ead3_len > OZ_EDHOC_MAX_EAD)
        return fail(s, OZ_E_ARG);
    uint8_t g_iy[32], mac[MAC_LEN], k[KEY_LEN], iv[IV_LEN], aad[45];
    int rc = dh(s, g_iy, s->own_priv, s->peer_eph);
    if (!rc) rc = prk_4e3m(s, g_iy);
    oz_wipe(g_iy, sizeof g_iy);
    if (!rc) rc = mac3(s, &s->own, ead3, ead3_len, mac);
    if (rc)
        return fail(s, rc);

    /* PLAINTEXT_3 = ( ID_CRED_I, Signature_or_MAC_3, ? EAD_3 ) */
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->a, sizeof s->a);
    put_id_cred_compact(&w, &s->own);
    oz_cbor_put_bstr(&w, mac, MAC_LEN);
    oz_cbor_put_raw(&w, ead3, ead3_len);
    if (w.err)
        return fail(s, w.err);
    size_t pt_len = w.len;

    rc = k3_iv3(s, k, iv, aad);
    if (rc)
        return fail(s, rc);
    oz_cbor_w o;
    oz_cbor_w_init(&o, out, cap);
    oz_cbor_put_bstr_head(&o, pt_len + MAC_LEN);
    if (o.err || cap - o.len < pt_len + MAC_LEN)
        return fail(s, OZ_E_NO_SPACE);
    rc = oz_aes128_ccm_seal(k, iv, IV_LEN, aad, sizeof aad, s->a, pt_len,
                            MAC_LEN, out + o.len, cap - o.len);
    oz_wipe(k, sizeof k);
    if (rc)
        return fail(s, rc);
    *out_len = o.len + pt_len + MAC_LEN;

    rc = transcript(s, s->a, pt_len, s->own.cred, s->own.cred_len);       /* TH_4 */
    if (rc)
        return fail(s, rc);
    return finish(s);
}

/* ── public: responder ──────────────────────────────────────────────────── */

static int suite_supported(const oz_edhoc *s, int64_t suite)
{
    for (int i = 0; i < s->n_suites; i++)
        if (s->suites[i] == suite)
            return 1;
    return 0;
}

static int wrong_suite(oz_edhoc *s, uint8_t *out, size_t cap, size_t *out_len)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, out, cap);
    oz_cbor_put_int(&w, OZ_EDHOC_ERR_WRONG_SUITE);
    if (s->n_suites == 1) {
        oz_cbor_put_int(&w, s->suites[0]);
    } else {
        oz_cbor_put_array(&w, s->n_suites);
        for (int i = 0; i < s->n_suites; i++)
            oz_cbor_put_int(&w, s->suites[i]);
    }
    s->state = ST_FAILED;
    if (w.err)
        return OZ_E_NO_SPACE;
    *out_len = w.len;
    return OZ_EDHOC_SEND_ERROR;
}

int oz_edhoc_r_msg1(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    const uint8_t *ead2, size_t ead2_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (s->initiator || s->state != ST_INIT || ead2_len > OZ_EDHOC_MAX_EAD)
        return fail(s, OZ_E_ARG);

    /* message_1 = ( METHOD, SUITES_I, G_X, C_I, ? EAD_1 ) */
    oz_cbor_r r;
    oz_cbor_r_init(&r, in, in_len);
    int64_t method = oz_cbor_get_int(&r);
    int64_t sel = -1, earlier[OZ_EDHOC_MAX_SUITES];
    size_t n_earlier = 0;
    if (oz_cbor_peek(&r) == OZ_CBOR_ARRAY) {
        size_t n = oz_cbor_get_array(&r);
        if (n < 2 || n > OZ_EDHOC_MAX_SUITES + 1)
            return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "SUITES_I malformed", out, cap, out_len);
        for (size_t i = 0; i < n - 1; i++) {
            int64_t v = oz_cbor_get_int(&r);
            if (i < OZ_EDHOC_MAX_SUITES)
                earlier[n_earlier++] = v;
        }
        sel = oz_cbor_get_int(&r);
    } else {
        sel = oz_cbor_get_int(&r);
    }
    size_t gl, cl;
    const uint8_t *g_x = oz_cbor_get_bstr(&r, &gl);
    if (r.err || !g_x || gl != 32 ||
        get_ident(&r, s->c_peer.b, sizeof s->c_peer.b, &cl) ||
        get_ead(&r, &s->ead, &s->ead_len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "message_1 malformed", out, cap, out_len);
    if (method != s->method)
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "method not supported", out, cap, out_len);
    if (!suite_supported(s, sel))
        return wrong_suite(s, out, cap, out_len);
    for (size_t i = 0; i < n_earlier; i++)
        if (suite_supported(s, earlier[i]))
            return wrong_suite(s, out, cap, out_len);
    s->c_peer.len = (uint8_t)cl;
    s->suite = (int8_t)sel;
    memcpy(s->peer_eph, g_x, 32);
    if (s->c_peer.len == s->c_own.len && !memcmp(s->c_peer.b, s->c_own.b, s->c_own.len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "C_I equals C_R", out, cap, out_len);
    /* EAD_1 points into the caller's buffer; it is only valid during this call. */

    uint8_t g_xy[32], g_rx[32], mac[MAC_LEN];
    int rc = oz_sha256(in, in_len, s->th);    /* H(message_1) */
    if (!rc) rc = make_eph(s);
    if (!rc) rc = th2(s, s->eph_pub);
    if (!rc) rc = dh(s, g_xy, s->eph_priv, s->peer_eph);
    if (!rc) rc = dh(s, g_rx, s->own_priv, s->peer_eph);
    if (!rc) rc = prk_2e_3e2m(s, g_xy, g_rx);
    oz_wipe(g_xy, sizeof g_xy);
    oz_wipe(g_rx, sizeof g_rx);
    if (!rc) rc = mac2(s, s->c_own.b, s->c_own.len, &s->own, ead2, ead2_len, mac);
    if (rc)
        return fail(s, rc);

    /* PLAINTEXT_2 = ( C_R, ID_CRED_R, Signature_or_MAC_2, ? EAD_2 ) */
    oz_cbor_w w;
    oz_cbor_w_init(&w, s->pt, sizeof s->pt);
    put_ident(&w, s->c_own.b, s->c_own.len);
    put_id_cred_compact(&w, &s->own);
    oz_cbor_put_bstr(&w, mac, MAC_LEN);
    oz_cbor_put_raw(&w, ead2, ead2_len);
    if (w.err)
        return fail(s, w.err);
    size_t pt_len = w.len;

    rc = kdf(s, s->prk_2e, 0, s->th, 32, s->a, pt_len);   /* KEYSTREAM_2 */
    if (rc)
        return fail(s, rc);
    oz_cbor_w o;
    oz_cbor_w_init(&o, out, cap);
    oz_cbor_put_bstr_head(&o, 32 + pt_len);
    oz_cbor_put_raw(&o, s->eph_pub, 32);
    if (o.err || cap - o.len < pt_len)
        return fail(s, OZ_E_NO_SPACE);
    for (size_t i = 0; i < pt_len; i++)
        out[o.len + i] = (uint8_t)(s->pt[i] ^ s->a[i]);
    *out_len = o.len + pt_len;

    rc = transcript(s, s->pt, pt_len, s->own.cred, s->own.cred_len);     /* TH_3 */
    if (rc)
        return fail(s, rc);
    s->ead = NULL;
    s->ead_len = 0;
    s->state = ST_R_SENT2;
    return OZ_OK;
}

int oz_edhoc_r_msg3(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (s->state != ST_R_SENT2)
        return fail(s, OZ_E_ARG);
    oz_cbor_r r;
    oz_cbor_r_init(&r, in, in_len);
    size_t n;
    const uint8_t *ct = oz_cbor_get_bstr(&r, &n);
    if (!ct || !oz_cbor_at_end(&r) || n <= MAC_LEN || n - MAC_LEN > sizeof s->pt)
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "message_3 malformed", out, cap, out_len);

    uint8_t k[KEY_LEN], iv[IV_LEN], aad[45], g_iy[32], mac[MAC_LEN];
    int rc = k3_iv3(s, k, iv, aad);
    if (rc)
        return fail(s, rc);
    size_t pt_len = n - MAC_LEN;
    rc = oz_aes128_ccm_open(k, iv, IV_LEN, aad, sizeof aad, ct, n, MAC_LEN, s->pt, sizeof s->pt);
    oz_wipe(k, sizeof k);
    if (rc == OZ_E_AUTH)
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "message_3 decrypt failed", out, cap, out_len);
    if (rc)
        return fail(s, rc);

    /* PLAINTEXT_3 = ( ID_CRED_I, Signature_or_MAC_3, ? EAD_3 ) */
    oz_cbor_r_init(&r, s->pt, pt_len);
    size_t idc_len, ml;
    if (get_id_cred(s, &r, &idc_len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "plaintext_3 malformed", out, cap, out_len);
    const uint8_t *mac_rx = oz_cbor_get_bstr(&r, &ml);
    if (!mac_rx || ml != MAC_LEN || get_ead(&r, &s->ead, &s->ead_len))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "plaintext_3 malformed", out, cap, out_len);

    rc = resolve_peer(s, idc_len);
    if (rc == OZ_E_NOT_FOUND)
        return send_err(s, OZ_EDHOC_ERR_UNKNOWN_CRED, NULL, out, cap, out_len);
    if (rc)
        return fail(s, rc);

    rc = dh(s, g_iy, s->eph_priv, s->peer.pub);
    if (!rc) rc = prk_4e3m(s, g_iy);
    oz_wipe(g_iy, sizeof g_iy);
    if (!rc) rc = mac3(s, &s->peer, s->ead, s->ead_len, mac);
    if (rc)
        return fail(s, rc);
    if (oz_ct_equal(mac, mac_rx, MAC_LEN))
        return send_err(s, OZ_EDHOC_ERR_UNSPECIFIED, "MAC_3 mismatch", out, cap, out_len);

    rc = transcript(s, s->pt, pt_len, s->peer.cred, s->peer.cred_len);   /* TH_4 */
    if (rc)
        return fail(s, rc);
    return finish(s);
}

/* ── public: after completion ───────────────────────────────────────────── */

int oz_edhoc_exporter(oz_edhoc *s, uint32_t label, const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t len)
{
    if (s->state != ST_DONE || ctx_len > sizeof s->a)
        return OZ_E_ARG;
    /* kdf() builds info in s->b; ctx may be the caller's own buffer. */
    return kdf(s, s->prk_exporter, label, ctx, ctx_len, out, len);
}

int oz_edhoc_oscore(oz_edhoc *s, uint8_t secret[16], uint8_t salt[8],
                    oz_edhoc_cid *sender_id, oz_edhoc_cid *recipient_id)
{
    int rc = oz_edhoc_exporter(s, 0, NULL, 0, secret, 16);
    if (!rc) rc = oz_edhoc_exporter(s, 1, NULL, 0, salt, 8);
    if (rc)
        return rc;
    /* Table 14: each side sends with the ID the other side chose. */
    *sender_id = s->c_peer;
    *recipient_id = s->c_own;
    return OZ_OK;
}

int oz_edhoc_pair_code(oz_edhoc *s, uint32_t *code)
{
    uint8_t b[4];
    int rc = oz_edhoc_exporter(s, OZ_EDHOC_EXPORTER_PAIR_CODE, NULL, 0, b, 4);
    if (rc)
        return rc;
    uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    *code = v % 1000000u;
    return OZ_OK;
}

/* ── public: credential helpers ─────────────────────────────────────────── */

/* Find integer key `key` in the map at r; leaves r at its value. */
static int map_find(oz_cbor_r *r, int64_t key)
{
    size_t pairs = oz_cbor_get_map(r);
    for (size_t i = 0; i < pairs && !r->err; i++) {
        int m = oz_cbor_peek(r);
        if ((m == OZ_CBOR_UINT || m == OZ_CBOR_NINT) && oz_cbor_get_int(r) == key)
            return OZ_OK;
        if (m != OZ_CBOR_UINT && m != OZ_CBOR_NINT) {
            size_t n;
            (void)oz_cbor_skip(r, &n);
        }
        size_t n;
        (void)oz_cbor_skip(r, &n);
    }
    return OZ_E_NOT_FOUND;
}

int oz_edhoc_ccs_x25519(const uint8_t *ccs, size_t len, uint8_t pub[32])
{
    oz_cbor_r r;
    oz_cbor_r_init(&r, ccs, len);
    if (map_find(&r, 8) || map_find(&r, 1))        /* cnf → COSE_Key */
        return OZ_E_ARG;
    size_t pairs = oz_cbor_get_map(&r);
    int64_t kty = 0, crv = 0;
    const uint8_t *x = NULL;
    size_t xl = 0;
    for (size_t i = 0; i < pairs && !r.err; i++) {
        int64_t k = oz_cbor_get_int(&r);
        if (k == 1 && oz_cbor_peek(&r) == OZ_CBOR_UINT) {
            kty = oz_cbor_get_int(&r);
        } else if (k == -1 && oz_cbor_peek(&r) == OZ_CBOR_UINT) {
            crv = oz_cbor_get_int(&r);
        } else if (k == -2 && oz_cbor_peek(&r) == OZ_CBOR_BSTR) {
            x = oz_cbor_get_bstr(&r, &xl);
        } else {
            size_t n;
            (void)oz_cbor_skip(&r, &n);
        }
    }
    if (r.err || kty != 1 || crv != 4 || !x || xl != 32)   /* OKP, X25519 */
        return OZ_E_ARG;
    memcpy(pub, x, 32);
    return OZ_OK;
}

int oz_edhoc_cred_from_kccs(const uint8_t *id_cred, size_t len, oz_edhoc_cred *out)
{
    oz_cbor_r r;
    oz_cbor_r_init(&r, id_cred, len);
    if (map_find(&r, 14))
        return OZ_E_NOT_FOUND;
    size_t n;
    const uint8_t *ccs = oz_cbor_skip(&r, &n);
    if (!ccs || n > OZ_EDHOC_MAX_CRED)
        return OZ_E_ARG;
    if (oz_edhoc_ccs_x25519(ccs, n, out->pub))
        return OZ_E_ARG;
    out->cred = ccs;
    out->cred_len = n;
    out->id_cred = id_cred;
    out->id_cred_len = len;
    return OZ_OK;
}
