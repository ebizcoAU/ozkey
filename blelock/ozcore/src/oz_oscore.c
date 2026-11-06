/* OSCORE (RFC 8613). Section numbers below are RFC 8613's. */
#include <string.h>
#include "oz_oscore.h"
#include "oz_cbor.h"

#define ALG_AES_CCM_16_64_128 10
#define TAG_LEN 8
#define NONCE_LEN 13

/* Class U (outer only) options; Observe is both inner and outer (§4.1). */
static int outer_only(uint16_t num)
{
    return num == OZ_COAP_OPT_URI_HOST || num == OZ_COAP_OPT_URI_PORT ||
           num == OZ_COAP_OPT_PROXY_URI || num == OZ_COAP_OPT_PROXY_SCHEME;
}

/* ── context ────────────────────────────────────────────────────────────── */

static int derive_one(const uint8_t prk[32], const uint8_t *id, size_t id_len,
                      const oz_oscore_ctx *c, const char *type, uint8_t *out, size_t len)
{
    uint8_t info[64];
    oz_cbor_w w;
    oz_cbor_w_init(&w, info, sizeof info);
    oz_cbor_put_array(&w, 5);
    oz_cbor_put_bstr(&w, id, id_len);
    if (c->id_ctx_len)
        oz_cbor_put_bstr(&w, c->id_ctx, c->id_ctx_len);
    else
        oz_cbor_put_null(&w);
    oz_cbor_put_int(&w, ALG_AES_CCM_16_64_128);
    oz_cbor_put_tstr(&w, type, strlen(type));
    oz_cbor_put_uint(&w, len);
    if (w.err)
        return w.err;
    return oz_hkdf_expand(prk, info, w.len, out, len);
}

int oz_oscore_derive(oz_oscore_ctx *c, const uint8_t *secret, size_t secret_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *sid, size_t sid_len,
                     const uint8_t *rid, size_t rid_len,
                     const uint8_t *id_ctx, size_t id_ctx_len)
{
    if (sid_len > OZ_OSCORE_MAX_ID || rid_len > OZ_OSCORE_MAX_ID ||
        id_ctx_len > OZ_OSCORE_MAX_IDCTX ||
        (sid_len == rid_len && !memcmp(sid, rid, sid_len)))   /* §3.3 */
        return OZ_E_ARG;
    memset(c, 0, sizeof *c);
    memcpy(c->sender_id, sid, sid_len);
    c->sender_id_len = (uint8_t)sid_len;
    memcpy(c->recipient_id, rid, rid_len);
    c->recipient_id_len = (uint8_t)rid_len;
    if (id_ctx_len)
        memcpy(c->id_ctx, id_ctx, id_ctx_len);
    c->id_ctx_len = (uint8_t)id_ctx_len;

    uint8_t prk[32];
    int rc = oz_hkdf_extract(salt, salt_len, secret, secret_len, prk);
    if (!rc) rc = derive_one(prk, sid, sid_len, c, "Key", c->sender_key, 16);
    if (!rc) rc = derive_one(prk, rid, rid_len, c, "Key", c->recipient_key, 16);
    if (!rc) rc = derive_one(prk, NULL, 0, c, "IV", c->common_iv, NONCE_LEN);
    oz_wipe(prk, sizeof prk);
    c->rw_state = OZ_OSCORE_RW_EMPTY;
    return rc;
}

void oz_oscore_wipe(oz_oscore_ctx *c) { oz_wipe(c, sizeof *c); }

/* ── nonce, AAD, Partial IV ─────────────────────────────────────────────── */

/* §5.2: S ‖ pad(ID_PIV, 7) ‖ pad(PIV, 5), XOR Common IV. */
static void make_nonce(const oz_oscore_ctx *c, const uint8_t *id, size_t id_len,
                       const uint8_t *piv, size_t piv_len, uint8_t nonce[NONCE_LEN])
{
    memset(nonce, 0, NONCE_LEN);
    nonce[0] = (uint8_t)id_len;
    memcpy(nonce + 1 + (OZ_OSCORE_MAX_ID - id_len), id, id_len);
    memcpy(nonce + NONCE_LEN - piv_len, piv, piv_len);
    for (int i = 0; i < NONCE_LEN; i++)
        nonce[i] ^= c->common_iv[i];
}

/* §5.4: ["Encrypt0", h'', bstr .cbor [1, [10], kid, piv, h'']] */
static size_t make_aad(const oz_oscore_xchg *x, uint8_t *aad, size_t cap)
{
    uint8_t arr[32];
    oz_cbor_w a;
    oz_cbor_w_init(&a, arr, sizeof arr);
    oz_cbor_put_array(&a, 5);
    oz_cbor_put_uint(&a, 1);
    oz_cbor_put_array(&a, 1);
    oz_cbor_put_int(&a, ALG_AES_CCM_16_64_128);
    oz_cbor_put_bstr(&a, x->kid, x->kid_len);
    oz_cbor_put_bstr(&a, x->piv, x->piv_len);
    oz_cbor_put_bstr(&a, NULL, 0);            /* no Class I options */
    oz_cbor_w w;
    oz_cbor_w_init(&w, aad, cap);
    oz_cbor_put_array(&w, 3);
    oz_cbor_put_tstr(&w, "Encrypt0", 8);
    oz_cbor_put_bstr(&w, NULL, 0);
    oz_cbor_put_bstr(&w, arr, a.len);
    return (a.err || w.err) ? 0 : w.len;
}

/* Partial IV: the number without leading zero bytes; 0 is 0x00 (§5). */
static size_t piv_encode(uint64_t v, uint8_t piv[5])
{
    uint8_t b[5];
    for (int i = 4; i >= 0; i--) {
        b[i] = (uint8_t)v;
        v >>= 8;
    }
    size_t skip = 0;
    while (skip < 4 && b[skip] == 0)
        skip++;
    memcpy(piv, b + skip, 5 - skip);
    return 5 - skip;
}

static uint64_t piv_value(const uint8_t *piv, size_t len)
{
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++)
        v = (v << 8) | piv[i];
    return v;
}

static int next_ssn(oz_oscore_ctx *c, uint64_t *seq)
{
    if (c->ssn > OZ_OSCORE_SSN_MAX)
        return OZ_OSCORE_E_EXHAUSTED;
    if (c->persist && c->block && c->ssn >= c->ssn_limit) {
        uint64_t store = c->ssn + c->block;
        int rc = c->persist(c->app, store);
        if (rc)
            return rc;
        c->ssn_limit = store;
    }
    *seq = c->ssn++;
    return OZ_OK;
}

/* ── the OSCORE option (§6.1) ───────────────────────────────────────────── */

typedef struct {
    const uint8_t *piv; size_t piv_len;
    const uint8_t *kid; size_t kid_len; int has_kid;
    const uint8_t *ctx; size_t ctx_len; int has_ctx;
} opt_fields;

static int parse_option(const oz_coap_opt *o, opt_fields *f)
{
    memset(f, 0, sizeof *f);
    if (!o->len)
        return OZ_OK;
    const uint8_t *v = o->val;
    size_t n = v[0] & 7, pos = 1;
    if ((v[0] & 0xe0) || n == 6 || n == 7 || 1 + n > o->len)
        return OZ_OSCORE_E_DECODE;
    f->piv = v + 1;
    f->piv_len = n;
    pos += n;
    if (v[0] & 0x10) {
        if (pos >= o->len || v[pos] > o->len - pos - 1)
            return OZ_OSCORE_E_DECODE;
        f->has_ctx = 1;
        f->ctx_len = v[pos];
        f->ctx = v + pos + 1;
        pos += 1 + f->ctx_len;
    }
    if (v[0] & 0x08) {
        f->has_kid = 1;
        f->kid = v + pos;
        f->kid_len = o->len - pos;
    } else if (pos != o->len) {
        return OZ_OSCORE_E_DECODE;
    }
    return OZ_OK;
}

static size_t build_option(uint8_t *out, const uint8_t *piv, size_t piv_len,
                           const uint8_t *ctx, size_t ctx_len, int with_ctx,
                           const uint8_t *kid, size_t kid_len, int with_kid)
{
    size_t pos = 1;
    out[0] = (uint8_t)(piv_len | (with_ctx ? 0x10 : 0) | (with_kid ? 0x08 : 0));
    memcpy(out + pos, piv, piv_len);
    pos += piv_len;
    if (with_ctx) {
        out[pos++] = (uint8_t)ctx_len;
        memcpy(out + pos, ctx, ctx_len);
        pos += ctx_len;
    }
    if (with_kid) {
        memcpy(out + pos, kid, kid_len);
        pos += kid_len;
    }
    if (pos == 1 && out[0] == 0)
        return 0;                  /* nothing to say: empty option value */
    return pos;
}

/* ── protect / verify core ──────────────────────────────────────────────── */

/* Build the outer message in out: header, token, Class U options + OSCORE
 * option + outer Observe, then 0xff and the encrypted inner message. */
static int protect(const oz_coap_msg *in, uint8_t outer_code, const uint8_t *optval, size_t optlen,
                   const uint8_t key[16], const uint8_t nonce[NONCE_LEN],
                   const oz_oscore_xchg *x, uint8_t *out, size_t cap, size_t *out_len)
{
    oz_coap_msg o;
    oz_coap_init(&o, in->type, outer_code, in->mid);
    o.tkl = in->tkl;
    memcpy(o.token, in->token, in->tkl);
    oz_coap_opt inner[OZ_COAP_MAX_OPTS];
    size_t n_inner = 0;
    int rc = oz_coap_add_opt(&o, OZ_COAP_OPT_OSCORE, optval, optlen);
    for (size_t i = 0; !rc && i < in->n_opts; i++) {
        const oz_coap_opt *op = &in->opts[i];
        if (op->num == OZ_COAP_OPT_OSCORE)
            continue;
        if (outer_only(op->num) || op->num == OZ_COAP_OPT_OBSERVE)
            rc = oz_coap_add_opt(&o, op->num, op->val, op->len);
        if (!outer_only(op->num))
            inner[n_inner++] = *op;
    }
    if (rc)
        return rc;

    /* Outer part with an empty payload, then the marker. */
    size_t pos;
    rc = oz_coap_encode(&o, out, cap, &pos);
    if (rc)
        return rc;
    if (cap - pos < 2 + TAG_LEN)
        return OZ_E_NO_SPACE;
    out[pos++] = 0xff;

    /* Plaintext = Code ‖ inner options ‖ (0xff ‖ payload), written in place. */
    out[pos] = in->code;
    size_t body;
    rc = oz_coap_encode_opts(inner, n_inner, in->payload, in->payload_len,
                             out + pos + 1, cap - pos - 1 - TAG_LEN, &body);
    if (rc)
        return rc;
    size_t pt_len = 1 + body;

    uint8_t aad[48];
    size_t aad_len = make_aad(x, aad, sizeof aad);
    if (!aad_len)
        return OZ_E_ARG;
    rc = oz_aes128_ccm_seal(key, nonce, NONCE_LEN, aad, aad_len, out + pos, pt_len,
                            TAG_LEN, out + pos, cap - pos);
    if (rc)
        return rc;
    *out_len = pos + pt_len + TAG_LEN;
    return OZ_OK;
}

/* Decode the outer message; find and parse the OSCORE option. */
static int unwrap(const uint8_t *in, size_t in_len, oz_coap_msg *outer, opt_fields *f)
{
    if (oz_coap_decode(outer, in, in_len))
        return OZ_OSCORE_E_DECODE;
    const oz_coap_opt *o = oz_coap_find(outer, OZ_COAP_OPT_OSCORE);
    if (!o || outer->payload_len <= TAG_LEN)
        return OZ_OSCORE_E_DECODE;
    return parse_option(o, f);
}

/* Decrypt and rebuild the inner message: outer header, inner code/options/
 * payload, plus the outer Class U options. */
static int open_inner(const oz_coap_msg *outer, const uint8_t key[16],
                      const uint8_t nonce[NONCE_LEN], const oz_oscore_xchg *x,
                      uint8_t *pt, size_t pt_cap, oz_coap_msg *m)
{
    uint8_t aad[48];
    size_t aad_len = make_aad(x, aad, sizeof aad);
    size_t pt_len = outer->payload_len - TAG_LEN;
    if (!aad_len || pt_cap < pt_len)
        return OZ_E_NO_SPACE;
    int rc = oz_aes128_ccm_open(key, nonce, NONCE_LEN, aad, aad_len,
                                outer->payload, outer->payload_len, TAG_LEN, pt, pt_cap);
    if (rc)
        return rc;
    oz_coap_init(m, outer->type, pt[0], outer->mid);
    m->tkl = outer->tkl;
    memcpy(m->token, outer->token, outer->tkl);
    if (oz_coap_decode_opts(m, pt + 1, pt_len - 1))
        return OZ_OSCORE_E_DECODE;
    for (size_t i = 0; i < m->n_opts; i++)
        if (m->opts[i].num == OZ_COAP_OPT_OSCORE || outer_only(m->opts[i].num))
            return OZ_OSCORE_E_DECODE;   /* §8.2/8.4: not allowed inside */
    for (size_t i = 0; i < outer->n_opts; i++)
        if (outer_only(outer->opts[i].num) &&
            oz_coap_add_opt(m, outer->opts[i].num, outer->opts[i].val, outer->opts[i].len))
            return OZ_E_NO_SPACE;
    return OZ_OK;
}

/* ── replay window (§7.4, B.1.2) ────────────────────────────────────────── */

static int window_check(const oz_oscore_ctx *c, uint64_t seq)
{
    if (c->rw_state != OZ_OSCORE_RW_ACTIVE || seq > c->rw_high)
        return OZ_OK;
    uint64_t d = c->rw_high - seq;
    if (d >= 32 || (c->rw_mask >> d) & 1)
        return OZ_OSCORE_E_REPLAY;
    return OZ_OK;
}

static void window_update(oz_oscore_ctx *c, uint64_t seq)
{
    if (c->rw_state == OZ_OSCORE_RW_EMPTY) {
        c->rw_state = OZ_OSCORE_RW_ACTIVE;
        c->rw_high = seq;
        c->rw_mask = 1;
    } else if (seq > c->rw_high) {
        uint64_t d = seq - c->rw_high;
        c->rw_mask = d >= 32 ? 1 : (c->rw_mask << d) | 1;
        c->rw_high = seq;
    } else {
        c->rw_mask |= 1u << (c->rw_high - seq);
    }
}

void oz_oscore_window_start(oz_oscore_ctx *c, uint64_t seq)
{
    /* Everything at or below seq counts as seen. */
    c->rw_state = OZ_OSCORE_RW_ACTIVE;
    c->rw_high = seq;
    c->rw_mask = 0xffffffffu;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int oz_oscore_protect_req(oz_oscore_ctx *c, const oz_coap_msg *req,
                          uint8_t *out, size_t cap, size_t *out_len, oz_oscore_xchg *x)
{
    uint64_t seq;
    int rc = next_ssn(c, &seq);
    if (rc)
        return rc;
    memset(x, 0, sizeof *x);
    x->seq = seq;
    x->piv_len = (uint8_t)piv_encode(seq, x->piv);
    memcpy(x->kid, c->sender_id, c->sender_id_len);
    x->kid_len = c->sender_id_len;
    x->observe = oz_coap_find(req, OZ_COAP_OPT_OBSERVE) != NULL;
    make_nonce(c, c->sender_id, c->sender_id_len, x->piv, x->piv_len, x->nonce);

    uint8_t optval[1 + 5 + 1 + OZ_OSCORE_MAX_IDCTX + OZ_OSCORE_MAX_ID];
    size_t optlen = build_option(optval, x->piv, x->piv_len, c->id_ctx, c->id_ctx_len,
                                 c->send_id_ctx && c->id_ctx_len,
                                 c->sender_id, c->sender_id_len, 1);
    return protect(req, x->observe ? OZ_COAP_FETCH : OZ_COAP_POST, optval, optlen,
                   c->sender_key, x->nonce, x, out, cap, out_len);
}

int oz_oscore_verify_req(oz_oscore_ctx *c, const uint8_t *in, size_t in_len,
                         uint8_t *pt, size_t pt_cap, oz_coap_msg *req, oz_oscore_xchg *x)
{
    oz_coap_msg outer;
    opt_fields f;
    int rc = unwrap(in, in_len, &outer, &f);
    if (rc)
        return rc;
    if (!f.piv_len || !f.has_kid)
        return OZ_OSCORE_E_DECODE;            /* requests carry both (§5) */
    if (f.kid_len != c->recipient_id_len || memcmp(f.kid, c->recipient_id, f.kid_len) ||
        (f.has_ctx && (f.ctx_len != c->id_ctx_len || memcmp(f.ctx, c->id_ctx, f.ctx_len))))
        return OZ_OSCORE_E_KID;

    memset(x, 0, sizeof *x);
    memcpy(x->kid, f.kid, f.kid_len);
    x->kid_len = (uint8_t)f.kid_len;
    memcpy(x->piv, f.piv, f.piv_len);
    x->piv_len = (uint8_t)f.piv_len;
    x->seq = piv_value(f.piv, f.piv_len);
    if (c->rw_state == OZ_OSCORE_RW_UNKNOWN)
        x->replay_unknown = 1;
    else if (window_check(c, x->seq))
        return OZ_OSCORE_E_REPLAY;
    make_nonce(c, f.kid, f.kid_len, f.piv, f.piv_len, x->nonce);

    rc = open_inner(&outer, c->recipient_key, x->nonce, x, pt, pt_cap, req);
    if (rc)
        return rc;
    x->observe = oz_coap_find(req, OZ_COAP_OPT_OBSERVE) != NULL;
    if (!x->replay_unknown)
        window_update(c, x->seq);
    return OZ_OK;
}

int oz_oscore_protect_resp(oz_oscore_ctx *c, const oz_oscore_xchg *x, const oz_coap_msg *resp,
                           int with_piv, uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t nonce[NONCE_LEN], piv[5], optval[8];
    size_t piv_len = 0;
    if (with_piv) {
        uint64_t seq;
        int rc = next_ssn(c, &seq);
        if (rc)
            return rc;
        piv_len = piv_encode(seq, piv);
        make_nonce(c, c->sender_id, c->sender_id_len, piv, piv_len, nonce);
    } else {
        memcpy(nonce, x->nonce, NONCE_LEN);
    }
    size_t optlen = build_option(optval, piv, piv_len, NULL, 0, 0, NULL, 0, 0);
    int observe = x->observe && oz_coap_find(resp, OZ_COAP_OPT_OBSERVE);
    return protect(resp, observe ? OZ_COAP_CONTENT : OZ_COAP_CHANGED, optval, optlen,
                   c->sender_key, nonce, x, out, cap, out_len);
}

int oz_oscore_verify_resp(oz_oscore_ctx *c, const oz_oscore_xchg *x,
                          const uint8_t *in, size_t in_len,
                          uint8_t *pt, size_t pt_cap, oz_coap_msg *resp, int64_t *piv_out)
{
    oz_coap_msg outer;
    opt_fields f;
    int rc = unwrap(in, in_len, &outer, &f);
    if (rc)
        return rc;
    /* Matching the token to the request is the caller's job; the binding to
     * the request is cryptographic (request kid + PIV in the AAD). */
    uint8_t nonce[NONCE_LEN];
    if (f.piv_len) {
        const uint8_t *id = f.has_kid ? f.kid : c->recipient_id;
        size_t id_len = f.has_kid ? f.kid_len : c->recipient_id_len;
        if (id_len > OZ_OSCORE_MAX_ID)
            return OZ_OSCORE_E_DECODE;
        make_nonce(c, id, id_len, f.piv, f.piv_len, nonce);
    } else {
        memcpy(nonce, x->nonce, NONCE_LEN);
    }
    rc = open_inner(&outer, c->recipient_key, nonce, x, pt, pt_cap, resp);
    if (rc)
        return rc;
    if (!x->observe && oz_coap_find(resp, OZ_COAP_OPT_OBSERVE))
        return OZ_OSCORE_E_UNEXPECTED_OBSERVE;   /* §4.1.3.5.2 */
    if (piv_out)
        *piv_out = f.piv_len ? (int64_t)piv_value(f.piv, f.piv_len) : -1;
    return OZ_OK;
}

int oz_oscore_peek_kid(const uint8_t *in, size_t in_len,
                       const uint8_t **kid, size_t *kid_len,
                       const uint8_t **id_ctx, size_t *id_ctx_len)
{
    oz_coap_msg outer;
    opt_fields f;
    int rc = unwrap(in, in_len, &outer, &f);
    if (rc)
        return rc;
    if (!f.has_kid)
        return OZ_OSCORE_E_DECODE;
    *kid = f.kid;
    *kid_len = f.kid_len;
    *id_ctx = f.has_ctx ? f.ctx : NULL;
    *id_ctx_len = f.has_ctx ? f.ctx_len : 0;
    return OZ_OK;
}
