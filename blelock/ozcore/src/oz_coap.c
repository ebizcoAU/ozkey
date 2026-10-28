/* CoAP message codec (RFC 7252 §3). */
#include <string.h>
#include "oz_coap.h"

void oz_coap_init(oz_coap_msg *m, uint8_t type, uint8_t code, uint16_t mid)
{
    memset(m, 0, sizeof *m);
    m->type = type;
    m->code = code;
    m->mid = mid;
}

int oz_coap_add_opt(oz_coap_msg *m, uint16_t num, const uint8_t *val, size_t len)
{
    if (m->n_opts >= OZ_COAP_MAX_OPTS || len > 0xffff)
        return OZ_E_NO_SPACE;
    size_t i = m->n_opts;
    while (i > 0 && m->opts[i - 1].num > num) {
        m->opts[i] = m->opts[i - 1];
        i--;
    }
    m->opts[i].num = num;
    m->opts[i].len = (uint16_t)len;
    m->opts[i].val = val;
    m->n_opts++;
    return OZ_OK;
}

int oz_coap_set_path(oz_coap_msg *m, const char *path)
{
    const char *p = path;
    while (*p == '/')
        p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/')
            e++;
        int rc = oz_coap_add_opt(m, OZ_COAP_OPT_URI_PATH, (const uint8_t *)p, (size_t)(e - p));
        if (rc)
            return rc;
        p = *e ? e + 1 : e;
    }
    return OZ_OK;
}

int oz_coap_add_uint_opt(oz_coap_msg *m, uint16_t num, uint32_t v, uint8_t buf[4])
{
    size_t n = 0;
    for (int shift = 24; shift >= 0; shift -= 8)
        if (n || (v >> shift) & 0xff)
            buf[n++] = (uint8_t)(v >> shift);
    return oz_coap_add_opt(m, num, buf, n);
}

const oz_coap_opt *oz_coap_find(const oz_coap_msg *m, uint16_t num)
{
    for (size_t i = 0; i < m->n_opts; i++)
        if (m->opts[i].num == num)
            return &m->opts[i];
    return NULL;
}

uint32_t oz_coap_opt_uint(const oz_coap_opt *o)
{
    uint32_t v = 0;
    for (size_t i = 0; o && i < o->len && i < 4; i++)
        v = (v << 8) | o->val[i];
    return v;
}

int oz_coap_path(const oz_coap_msg *m, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < m->n_opts; i++) {
        if (m->opts[i].num != OZ_COAP_OPT_URI_PATH)
            continue;
        size_t need = (n ? 1u : 0u) + m->opts[i].len;
        if (need + 1 > cap - n)
            return OZ_E_NO_SPACE;
        if (n)
            out[n++] = '/';
        memcpy(out + n, m->opts[i].val, m->opts[i].len);
        n += m->opts[i].len;
    }
    if (!cap)
        return OZ_E_NO_SPACE;
    out[n] = 0;
    return OZ_OK;
}

/* ── encoding ───────────────────────────────────────────────────────────── */

static uint8_t nibble(uint32_t v, uint8_t *ext, size_t *ext_len)
{
    if (v < 13) {
        *ext_len = 0;
        return (uint8_t)v;
    }
    if (v < 269) {
        ext[0] = (uint8_t)(v - 13);
        *ext_len = 1;
        return 13;
    }
    ext[0] = (uint8_t)((v - 269) >> 8);
    ext[1] = (uint8_t)(v - 269);
    *ext_len = 2;
    return 14;
}

int oz_coap_encode_opts(const oz_coap_opt *opts, size_t n, const uint8_t *payload,
                        size_t payload_len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t pos = 0;
    uint16_t prev = 0;
    for (size_t i = 0; i < n; i++) {
        if (opts[i].num < prev)
            return OZ_E_ARG;
        uint8_t ed[2], el[2];
        size_t edn, eln;
        uint8_t d = nibble((uint32_t)(opts[i].num - prev), ed, &edn);
        uint8_t l = nibble(opts[i].len, el, &eln);
        size_t need = 1 + edn + eln + opts[i].len;
        if (need > cap - pos)
            return OZ_E_NO_SPACE;
        out[pos++] = (uint8_t)((d << 4) | l);
        memcpy(out + pos, ed, edn); pos += edn;
        memcpy(out + pos, el, eln); pos += eln;
        if (opts[i].len)
            memcpy(out + pos, opts[i].val, opts[i].len);
        pos += opts[i].len;
        prev = opts[i].num;
    }
    if (payload_len) {
        if (payload_len + 1 > cap - pos)
            return OZ_E_NO_SPACE;
        out[pos++] = 0xff;
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }
    *out_len = pos;
    return OZ_OK;
}

int oz_coap_encode(const oz_coap_msg *m, uint8_t *out, size_t cap, size_t *out_len)
{
    if (m->tkl > OZ_COAP_MAX_TOKEN || m->type > 3)
        return OZ_E_ARG;
    size_t hdr = 4u + m->tkl;
    if (cap < hdr)
        return OZ_E_NO_SPACE;
    out[0] = (uint8_t)(0x40 | (m->type << 4) | m->tkl);
    out[1] = m->code;
    out[2] = (uint8_t)(m->mid >> 8);
    out[3] = (uint8_t)m->mid;
    memcpy(out + 4, m->token, m->tkl);
    size_t body;
    int rc = oz_coap_encode_opts(m->opts, m->n_opts, m->payload, m->payload_len,
                                 out + hdr, cap - hdr, &body);
    if (rc)
        return rc;
    *out_len = hdr + body;
    return OZ_OK;
}

/* ── decoding ───────────────────────────────────────────────────────────── */

static int ext_val(uint8_t nib, const uint8_t *in, size_t len, size_t *pos, uint32_t *v)
{
    if (nib < 13) {
        *v = nib;
        return 0;
    }
    if (nib == 13) {
        if (*pos + 1 > len)
            return -1;
        *v = 13u + in[(*pos)++];
        return 0;
    }
    if (nib == 14) {
        if (*pos + 2 > len)
            return -1;
        *v = 269u + (((uint32_t)in[*pos] << 8) | in[*pos + 1]);
        *pos += 2;
        return 0;
    }
    return -1;   /* 15 is reserved outside the payload marker */
}

int oz_coap_decode_opts(oz_coap_msg *m, const uint8_t *in, size_t len)
{
    size_t pos = 0;
    uint32_t num = 0;
    m->n_opts = 0;
    m->payload = NULL;
    m->payload_len = 0;
    while (pos < len) {
        uint8_t b = in[pos++];
        if (b == 0xff) {
            if (pos == len)
                return OZ_E_ARG;   /* marker followed by nothing */
            m->payload = in + pos;
            m->payload_len = len - pos;
            return OZ_OK;
        }
        uint32_t d, l;
        if (ext_val(b >> 4, in, len, &pos, &d) || ext_val(b & 0x0f, in, len, &pos, &l))
            return OZ_E_ARG;
        num += d;
        if (num > 0xffff || l > len - pos)
            return OZ_E_ARG;
        if (m->n_opts >= OZ_COAP_MAX_OPTS)
            return OZ_E_NO_SPACE;
        m->opts[m->n_opts].num = (uint16_t)num;
        m->opts[m->n_opts].len = (uint16_t)l;
        m->opts[m->n_opts].val = in + pos;
        m->n_opts++;
        pos += l;
    }
    return OZ_OK;
}

int oz_coap_decode(oz_coap_msg *m, const uint8_t *in, size_t len)
{
    if (len < 4 || (in[0] >> 6) != 1)
        return OZ_E_ARG;
    memset(m, 0, sizeof *m);
    m->type = (in[0] >> 4) & 3;
    m->tkl = in[0] & 0x0f;
    m->code = in[1];
    m->mid = (uint16_t)((in[2] << 8) | in[3]);
    if (m->tkl > OZ_COAP_MAX_TOKEN || len < 4u + m->tkl)
        return OZ_E_ARG;
    memcpy(m->token, in + 4, m->tkl);
    return oz_coap_decode_opts(m, in + 4 + m->tkl, len - 4 - m->tkl);
}
