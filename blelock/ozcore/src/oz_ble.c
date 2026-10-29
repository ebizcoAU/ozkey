/* oz_ble — see oz_ble.h. */
#include "oz_ble.h"
#include "oz_port.h"

#define MTU_MAX 517

void oz_ble_init(oz_ble *s, oz_lock *L, oz_link *link, oz_ble_tx tx, void *tx_ctx)
{
    s->L = L;
    s->link = link;
    s->tx = tx;
    s->tx_ctx = tx_ctx;
    s->mtu = OZ_FRAME_MIN_MTU;
    oz_reasm_reset(&s->rx);
}

void oz_ble_connect(oz_ble *s)
{
    oz_link_open(s->link, 0);
    oz_reasm_reset(&s->rx);
    s->mtu = OZ_FRAME_MIN_MTU;
}

void oz_ble_set_mtu(oz_ble *s, size_t mtu)
{
    s->mtu = mtu < OZ_FRAME_MIN_MTU ? OZ_FRAME_MIN_MTU : mtu > MTU_MAX ? MTU_MAX : mtu;
}

void oz_ble_disconnect(oz_ble *s)
{
    oz_link_close(s->link);
    oz_reasm_reset(&s->rx);
}

int oz_ble_send(oz_ble *s, uint8_t type, const uint8_t *msg, size_t len)
{
    unsigned count;
    int rc = oz_frag_count(len, s->mtu, &count);
    if (rc) {
        oz_port_log("ble: %u-byte reply does not fit MTU %u", (unsigned)len, (unsigned)s->mtu);
        return rc;
    }
    uint8_t frame[MTU_MAX - 3];
    for (unsigned i = 0; i < count; i++) {
        size_t n;
        rc = oz_frag(type, msg, len, s->mtu, i, frame, sizeof frame, &n);
        if (!rc)
            rc = s->tx(s->tx_ctx, frame, n);
        if (rc)
            return rc;
    }
    return OZ_OK;
}

/* The engine's reply, if it made one. */
static int reply(oz_ble *s, int rc, uint8_t type, size_t len)
{
    if (len) {
        int sent = oz_ble_send(s, type, s->out, len);
        if (!rc)
            rc = sent;
    }
    return rc;
}

int oz_ble_rx(oz_ble *s, const uint8_t *frame, size_t len)
{
    int done;
    int rc = oz_reasm_feed(&s->rx, frame, len, &done);
    if (rc || !done)
        return rc;
    uint8_t type;
    size_t n = 0;
    rc = oz_lock_rx(s->L, s->link, s->rx.type, s->rx.buf, s->rx.len, &type, s->out, sizeof s->out, &n);
    return reply(s, rc, type, n);
}

int oz_ble_pair_code(oz_ble *s, uint32_t code)
{
    uint8_t type;
    size_t n = 0;
    int rc = oz_lock_pair_code(s->L, s->link, code, &type, s->out, sizeof s->out, &n);
    return reply(s, rc, type, n);
}

int oz_ble_tick(oz_ble *s)
{
    uint8_t type;
    size_t n = 0;
    int rc = reply(s, oz_lock_tick(s->L, s->link, &type, s->out, sizeof s->out, &n), type, n);
    /* Pending `events` notifications, a few per tick so one burst can't
     * starve the rest of the thread. */
    for (int i = 0; i < 4 && !rc; i++) {
        n = 0;
        rc = reply(s, oz_lock_events_poll(s->L, s->link, &type, s->out, sizeof s->out, &n), type, n);
        if (!n)
            break;
    }
    return rc;
}
