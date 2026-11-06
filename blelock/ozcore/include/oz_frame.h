/* oz_frame — OZKIE GATT framing, ozkey-48 §2.1.
 *
 *   byte 0:  bits 7..6 = type (OZ_FRAME_EDHOC / _OSCORE / _ERROR)
 *            bit  5    = more (1 = more fragments follow)
 *            bits 4..0 = fragment number, 0..31, restarting per message
 *   byte 1…: payload fragment, at most ATT MTU − 3 − 1 bytes
 *
 * A message is at most OZ_FRAME_MAX_MSG bytes. Fragment 0 always starts a new
 * message and drops any half-received one, so a peer that gave up mid-message
 * recovers by just sending again. Transport-only: no crypto, no state beyond
 * the one message being reassembled. Over MQTT the whole message travels as
 * one payload and none of this is used.
 */
#ifndef OZ_FRAME_H
#define OZ_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define OZ_FRAME_MAX_MSG   1024
#define OZ_FRAME_MAX_FRAGS 32
#define OZ_FRAME_MIN_MTU   23           /* the BLE default ATT MTU */

typedef struct {
    uint8_t buf[OZ_FRAME_MAX_MSG];
    size_t  len;
    uint8_t type;
    uint8_t next;                       /* fragment number expected next */
    uint8_t active;                     /* a message is half received */
} oz_reasm;

void oz_reasm_reset(oz_reasm *r);

/* Feed one received frame (one GATT write). Returns OZ_OK and sets *done to 1
 * when a whole message is ready in r->type / r->buf / r->len (valid until the
 * next feed), or to 0 when more fragments are needed. A malformed frame or an
 * out-of-order fragment returns OZ_E_ARG, a message over OZ_FRAME_MAX_MSG
 * returns OZ_E_NO_SPACE; both drop the partial message. */
int oz_reasm_feed(oz_reasm *r, const uint8_t *frame, size_t n, int *done);

/* Number of frames needed to send len bytes at this ATT MTU. OZ_E_NO_SPACE if
 * it needs more than OZ_FRAME_MAX_FRAGS or len > OZ_FRAME_MAX_MSG; OZ_E_ARG if
 * att_mtu < OZ_FRAME_MIN_MTU. */
int oz_frag_count(size_t len, size_t att_mtu, unsigned *count);

/* Build frame idx (0-based) of a message into out (at least att_mtu − 3 bytes). */
int oz_frag(uint8_t type, const uint8_t *msg, size_t len, size_t att_mtu,
            unsigned idx, uint8_t *out, size_t cap, size_t *out_len);

#endif
