/* oz_oscore.h — OSCORE (RFC 8613), the OZKIE v2 message layer (ozkey-48 §4).
 *
 * AES-CCM-16-64-128 + HKDF-SHA256 only. One oz_oscore_ctx per security
 * context (a BLE session, or a persisted relay context per bond).
 *
 * Replay window: 32 entries (RFC 8613 §3.2.2). A context restored after a
 * reboot starts with the window UNKNOWN (B.1.2): requests still decrypt, but
 * the exchange is flagged `replay_unknown` and the application must answer
 * 4.01 + Echo without acting, then call oz_oscore_window_start() once a
 * request carrying a valid Echo arrives.
 *
 * Sender sequence numbers can be reserved in blocks (B.1.1): set `persist`
 * and `block`, and the callback is invoked before the first number of each
 * block is used, with the value to store. After a reboot, restore `ssn` to
 * the stored value.
 */
#ifndef OZ_OSCORE_H
#define OZ_OSCORE_H

#include "oz_crypto.h"
#include "oz_coap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZ_OSCORE_MAX_ID   7      /* nonce length 13 − 6 */
#define OZ_OSCORE_MAX_IDCTX 16
#define OZ_OSCORE_SSN_MAX  ((1ull << 40) - 1)

enum {
    OZ_OSCORE_E_DECODE    = -20,  /* not an OSCORE message, or malformed option */
    OZ_OSCORE_E_KID       = -21,  /* kid / kid context don't match this context */
    OZ_OSCORE_E_REPLAY    = -22,  /* Partial IV already seen, or older than the window */
    OZ_OSCORE_E_EXHAUSTED = -23,  /* sender sequence numbers used up: re-key */
    OZ_OSCORE_E_UNEXPECTED_OBSERVE = -24,
};

enum { OZ_OSCORE_RW_EMPTY = 0, OZ_OSCORE_RW_ACTIVE = 1, OZ_OSCORE_RW_UNKNOWN = 2 };

typedef int (*oz_oscore_persist_fn)(void *app, uint64_t ssn_to_store);

typedef struct {
    uint8_t  sender_key[16];
    uint8_t  recipient_key[16];
    uint8_t  common_iv[13];
    uint8_t  sender_id[OZ_OSCORE_MAX_ID];
    uint8_t  sender_id_len;
    uint8_t  recipient_id[OZ_OSCORE_MAX_ID];
    uint8_t  recipient_id_len;
    uint8_t  id_ctx[OZ_OSCORE_MAX_IDCTX];
    uint8_t  id_ctx_len;
    uint8_t  send_id_ctx;        /* client: put 'kid context' in requests */

    uint64_t ssn;                /* next Sender Sequence Number */
    uint64_t ssn_limit;          /* first number not yet persisted */
    uint32_t block;              /* B.1.1 K; 0 = no persistence */
    oz_oscore_persist_fn persist;
    void    *app;

    uint8_t  rw_state;
    uint64_t rw_high;            /* highest Partial IV accepted */
    uint32_t rw_mask;            /* bit i: rw_high − i seen */
} oz_oscore_ctx;

/* Per request/response exchange: what a response is bound to. */
typedef struct {
    uint8_t  kid[OZ_OSCORE_MAX_ID];   /* request_kid */
    uint8_t  kid_len;
    uint8_t  piv[5];                  /* request_piv */
    uint8_t  piv_len;
    uint8_t  nonce[13];               /* request nonce, reused by a response without PIV */
    uint8_t  observe;                 /* the request carried Observe */
    uint8_t  replay_unknown;          /* server: answer 4.01 + Echo, don't act (B.1.2) */
    uint64_t seq;                     /* the request's Partial IV as a number */
} oz_oscore_xchg;

/* Derive Sender/Recipient Key and Common IV (§3.2). salt/id_ctx may be NULL. */
int oz_oscore_derive(oz_oscore_ctx *c, const uint8_t *secret, size_t secret_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *sid, size_t sid_len,
                     const uint8_t *rid, size_t rid_len,
                     const uint8_t *id_ctx, size_t id_ctx_len);
void oz_oscore_wipe(oz_oscore_ctx *c);

/* Client */
int oz_oscore_protect_req(oz_oscore_ctx *c, const oz_coap_msg *req,
                          uint8_t *out, size_t cap, size_t *out_len, oz_oscore_xchg *x);
/* piv_out: the response's Partial IV, or −1 when it had none (notifications
 * must carry one; the caller keeps the highest per observation). */
int oz_oscore_verify_resp(oz_oscore_ctx *c, const oz_oscore_xchg *x,
                          const uint8_t *in, size_t in_len,
                          uint8_t *pt, size_t pt_cap, oz_coap_msg *resp, int64_t *piv_out);

/* Server */
int oz_oscore_verify_req(oz_oscore_ctx *c, const uint8_t *in, size_t in_len,
                         uint8_t *pt, size_t pt_cap, oz_coap_msg *req, oz_oscore_xchg *x);
/* with_piv: use our own sequence number (required for notifications and for
 * the B.1.2 Echo response); otherwise reuse the request nonce. */
int oz_oscore_protect_resp(oz_oscore_ctx *c, const oz_oscore_xchg *x, const oz_coap_msg *resp,
                           int with_piv, uint8_t *out, size_t cap, size_t *out_len);

/* B.1.2: a request verified fresh by Echo becomes the window's lower limit. */
void oz_oscore_window_start(oz_oscore_ctx *c, uint64_t seq);

/* Read the kid (and kid context) of an OSCORE request without decrypting,
 * to pick the right context. Views point into `in`. */
int oz_oscore_peek_kid(const uint8_t *in, size_t in_len,
                       const uint8_t **kid, size_t *kid_len,
                       const uint8_t **id_ctx, size_t *id_ctx_len);

#ifdef __cplusplus
}
#endif
#endif /* OZ_OSCORE_H */
