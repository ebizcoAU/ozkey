/* oz_frame — see oz_frame.h. */
#include <string.h>

#include "oz_frame.h"
#include "oz_port.h"

#define TYPE_OF(b) ((uint8_t)((b) >> 6))
#define MORE_OF(b) (((b) >> 5) & 1u)
#define FRAG_OF(b) ((uint8_t)((b) & 0x1fu))

void oz_reasm_reset(oz_reasm *r)
{
    r->len = 0;
    r->type = 0;
    r->next = 0;
    r->active = 0;
}

static int drop(oz_reasm *r, int rc)
{
    oz_reasm_reset(r);
    return rc;
}

int oz_reasm_feed(oz_reasm *r, const uint8_t *frame, size_t n, int *done)
{
    *done = 0;
    if (n == 0)
        return drop(r, OZ_E_ARG);
    uint8_t type = TYPE_OF(frame[0]), frag = FRAG_OF(frame[0]);
    unsigned more = MORE_OF(frame[0]);
    size_t plen = n - 1;

    if (type > 2)
        return drop(r, OZ_E_ARG);
    if (frag == 0) {
        oz_reasm_reset(r);              /* a new message, whatever came before */
        r->type = type;
        r->active = 1;
    } else if (!r->active || frag != r->next || type != r->type) {
        return drop(r, OZ_E_ARG);
    }
    /* An empty non-final fragment carries nothing and only burns a number. */
    if (more && (plen == 0 || frag == OZ_FRAME_MAX_FRAGS - 1))
        return drop(r, OZ_E_ARG);
    if (plen > OZ_FRAME_MAX_MSG - r->len)
        return drop(r, OZ_E_NO_SPACE);

    memcpy(r->buf + r->len, frame + 1, plen);
    r->len += plen;
    if (more) {
        r->next = (uint8_t)(frag + 1);
        return OZ_OK;
    }
    r->active = 0;
    r->next = 0;
    *done = 1;
    return OZ_OK;
}

static size_t chunk_of(size_t att_mtu) { return att_mtu - 3 - 1; }

int oz_frag_count(size_t len, size_t att_mtu, unsigned *count)
{
    if (att_mtu < OZ_FRAME_MIN_MTU)
        return OZ_E_ARG;
    if (len > OZ_FRAME_MAX_MSG)
        return OZ_E_NO_SPACE;
    size_t c = chunk_of(att_mtu);
    size_t frames = len == 0 ? 1 : (len + c - 1) / c;
    if (frames > OZ_FRAME_MAX_FRAGS)
        return OZ_E_NO_SPACE;
    *count = (unsigned)frames;
    return OZ_OK;
}

int oz_frag(uint8_t type, const uint8_t *msg, size_t len, size_t att_mtu,
            unsigned idx, uint8_t *out, size_t cap, size_t *out_len)
{
    unsigned count;
    int rc = oz_frag_count(len, att_mtu, &count);
    if (rc)
        return rc;
    if (type > 2 || idx >= count)
        return OZ_E_ARG;
    size_t c = chunk_of(att_mtu);
    size_t off = (size_t)idx * c;
    size_t plen = len - off < c ? len - off : c;
    if (cap < plen + 1)
        return OZ_E_NO_SPACE;
    unsigned more = idx + 1 < count;
    out[0] = (uint8_t)((unsigned)type << 6 | more << 5 | idx);
    if (plen)
        memcpy(out + 1, msg + off, plen);
    *out_len = plen + 1;
    return OZ_OK;
}
