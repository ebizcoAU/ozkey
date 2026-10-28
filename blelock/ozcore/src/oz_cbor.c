/* Deterministic CBOR writer and a strict, zero-copy reader (RFC 8949). */
#include <string.h>
#include "oz_cbor.h"

/* ── Writer ─────────────────────────────────────────────────────────────── */

void oz_cbor_w_init(oz_cbor_w *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

static void put_bytes(oz_cbor_w *w, const uint8_t *p, size_t n)
{
    if (w->err)
        return;
    if (n > w->cap - w->len) {
        w->err = OZ_E_NO_SPACE;
        return;
    }
    if (n)
        memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put_head(oz_cbor_w *w, int major, uint64_t v)
{
    uint8_t h[9];
    size_t n;
    uint8_t mt = (uint8_t)(major << 5);
    if (v < 24) {
        h[0] = (uint8_t)(mt | v);
        n = 1;
    } else if (v <= 0xff) {
        h[0] = (uint8_t)(mt | 24);
        h[1] = (uint8_t)v;
        n = 2;
    } else if (v <= 0xffff) {
        h[0] = (uint8_t)(mt | 25);
        h[1] = (uint8_t)(v >> 8);
        h[2] = (uint8_t)v;
        n = 3;
    } else if (v <= 0xffffffffu) {
        h[0] = (uint8_t)(mt | 26);
        for (int i = 0; i < 4; i++)
            h[1 + i] = (uint8_t)(v >> (24 - 8 * i));
        n = 5;
    } else {
        h[0] = (uint8_t)(mt | 27);
        for (int i = 0; i < 8; i++)
            h[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        n = 9;
    }
    put_bytes(w, h, n);
}

void oz_cbor_put_uint(oz_cbor_w *w, uint64_t v) { put_head(w, OZ_CBOR_UINT, v); }

void oz_cbor_put_int(oz_cbor_w *w, int64_t v)
{
    if (v >= 0)
        put_head(w, OZ_CBOR_UINT, (uint64_t)v);
    else
        put_head(w, OZ_CBOR_NINT, (uint64_t)(-(v + 1)));
}

void oz_cbor_put_bstr(oz_cbor_w *w, const uint8_t *p, size_t n)
{
    put_head(w, OZ_CBOR_BSTR, n);
    put_bytes(w, p, n);
}

void oz_cbor_put_tstr(oz_cbor_w *w, const char *s, size_t n)
{
    put_head(w, OZ_CBOR_TSTR, n);
    put_bytes(w, (const uint8_t *)s, n);
}

void oz_cbor_put_array(oz_cbor_w *w, size_t count) { put_head(w, OZ_CBOR_ARRAY, count); }
void oz_cbor_put_map(oz_cbor_w *w, size_t pairs) { put_head(w, OZ_CBOR_MAP, pairs); }
void oz_cbor_put_bool(oz_cbor_w *w, int v) { put_head(w, OZ_CBOR_SIMPLE, v ? 21 : 20); }
void oz_cbor_put_null(oz_cbor_w *w) { put_head(w, OZ_CBOR_SIMPLE, 22); }
void oz_cbor_put_raw(oz_cbor_w *w, const uint8_t *p, size_t n) { put_bytes(w, p, n); }
void oz_cbor_put_bstr_head(oz_cbor_w *w, size_t n) { put_head(w, OZ_CBOR_BSTR, n); }

/* ── Reader ─────────────────────────────────────────────────────────────── */

void oz_cbor_r_init(oz_cbor_r *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

int oz_cbor_at_end(const oz_cbor_r *r) { return !r->err && r->pos == r->len; }

int oz_cbor_peek(const oz_cbor_r *r)
{
    if (r->err || r->pos >= r->len)
        return -1;
    return r->p[r->pos] >> 5;
}

/* Read a head. Non-shortest forms are rejected: a peer that re-encodes would
 * change transcript hashes, so it is treated as malformed. */
static int get_head(oz_cbor_r *r, int *major, uint64_t *val)
{
    if (r->err || r->pos >= r->len)
        goto bad;
    uint8_t ib = r->p[r->pos++];
    uint8_t ai = ib & 0x1f;
    *major = ib >> 5;
    if (ai < 24) {
        *val = ai;
        return 0;
    }
    if (ai > 27)
        goto bad;   /* reserved, or indefinite length */
    size_t n = (size_t)1 << (ai - 24);
    if (n > r->len - r->pos)
        goto bad;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v = (v << 8) | r->p[r->pos++];
    if ((n == 1 && v < 24) || (n == 2 && v <= 0xff) ||
        (n == 4 && v <= 0xffff) || (n == 8 && v <= 0xffffffffu))
        goto bad;
    *val = v;
    return 0;
bad:
    r->err = OZ_E_ARG;
    return -1;
}

int64_t oz_cbor_get_int(oz_cbor_r *r)
{
    int m;
    uint64_t v;
    if (get_head(r, &m, &v))
        return 0;
    if ((m != OZ_CBOR_UINT && m != OZ_CBOR_NINT) || v > INT64_MAX) {
        r->err = OZ_E_ARG;
        return 0;
    }
    return m == OZ_CBOR_UINT ? (int64_t)v : -1 - (int64_t)v;
}

uint64_t oz_cbor_get_uint(oz_cbor_r *r)
{
    int m;
    uint64_t v;
    if (get_head(r, &m, &v))
        return 0;
    if (m != OZ_CBOR_UINT) {
        r->err = OZ_E_ARG;
        return 0;
    }
    return v;
}

static const uint8_t *get_string(oz_cbor_r *r, int want, size_t *n)
{
    int m;
    uint64_t v;
    *n = 0;
    if (get_head(r, &m, &v))
        return NULL;
    if (m != want || v > r->len - r->pos) {
        r->err = OZ_E_ARG;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += (size_t)v;
    *n = (size_t)v;
    return p;
}

const uint8_t *oz_cbor_get_bstr(oz_cbor_r *r, size_t *n) { return get_string(r, OZ_CBOR_BSTR, n); }
const uint8_t *oz_cbor_get_tstr(oz_cbor_r *r, size_t *n) { return get_string(r, OZ_CBOR_TSTR, n); }

static size_t get_container(oz_cbor_r *r, int want)
{
    int m;
    uint64_t v;
    if (get_head(r, &m, &v))
        return 0;
    /* Each element needs at least one byte, so a count above the remaining
     * input is malformed (and can't drive a long loop). */
    if (m != want || v > r->len - r->pos) {
        r->err = OZ_E_ARG;
        return 0;
    }
    return (size_t)v;
}

size_t oz_cbor_get_array(oz_cbor_r *r) { return get_container(r, OZ_CBOR_ARRAY); }
size_t oz_cbor_get_map(oz_cbor_r *r) { return get_container(r, OZ_CBOR_MAP); }

int oz_cbor_get_bool(oz_cbor_r *r)
{
    int m;
    uint64_t v;
    if (get_head(r, &m, &v))
        return 0;
    if (m != OZ_CBOR_SIMPLE || (v != 20 && v != 21)) {
        r->err = OZ_E_ARG;
        return 0;
    }
    return v == 21;
}

static void skip_item(oz_cbor_r *r, int depth)
{
    int m;
    uint64_t v;
    if (depth > OZ_CBOR_MAX_DEPTH) {
        r->err = OZ_E_ARG;
        return;
    }
    if (get_head(r, &m, &v))
        return;
    switch (m) {
    case OZ_CBOR_UINT:
    case OZ_CBOR_NINT:
        return;
    case OZ_CBOR_BSTR:
    case OZ_CBOR_TSTR:
        if (v > r->len - r->pos)
            r->err = OZ_E_ARG;
        else
            r->pos += (size_t)v;
        return;
    case OZ_CBOR_ARRAY:
    case OZ_CBOR_MAP:
        if (v > r->len - r->pos) {
            r->err = OZ_E_ARG;
            return;
        }
        for (uint64_t i = 0; i < (m == OZ_CBOR_MAP ? 2 * v : v) && !r->err; i++)
            skip_item(r, depth + 1);
        return;
    case OZ_CBOR_SIMPLE:
        if (v == 20 || v == 21 || v == 22)
            return;
        r->err = OZ_E_ARG;   /* floats, undefined, other simples */
        return;
    default:
        r->err = OZ_E_ARG;   /* tags */
        return;
    }
}

const uint8_t *oz_cbor_skip(oz_cbor_r *r, size_t *n)
{
    size_t start = r->pos;
    skip_item(r, 0);
    if (r->err) {
        *n = 0;
        return NULL;
    }
    *n = r->pos - start;
    return r->p + start;
}
