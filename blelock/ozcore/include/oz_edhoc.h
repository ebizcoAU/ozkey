/* oz_edhoc.h — EDHOC (RFC 9528), the OZKIE v2 handshake (ozkey-48 §3).
 *
 * Supported: method 3 (static DH both sides), cipher suite 0
 * (AES-CCM-16-64-128, SHA-256, MAC 8, X25519). Suite 2 differs only in the
 * curve, so tests can run the RFC 9529 static-DH trace by injecting its DH
 * outputs through `dh`. message_4 is not used (ozkey-48 §3).
 *
 * No malloc: a session is one struct the caller owns (~2 KB). Wipe it with
 * oz_edhoc_wipe() when done.
 *
 * Return convention: OZ_OK; OZ_EDHOC_SEND_ERROR when `out` now holds an
 * EDHOC error message that should be sent before aborting; or a negative
 * OZ_E_* code (abort without sending anything).
 */
#ifndef OZ_EDHOC_H
#define OZ_EDHOC_H

#include "oz_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZ_EDHOC_SEND_ERROR   1

#define OZ_EDHOC_METHOD_STAT_STAT 3
#define OZ_EDHOC_SUITE_0          0

#define OZ_EDHOC_MAX_CID      7     /* connection identifier bytes */
#define OZ_EDHOC_MAX_IDCRED   200   /* full ID_CRED map (kccs by value fits) */
#define OZ_EDHOC_MAX_CRED     192   /* CRED_x (a CCS) */
#define OZ_EDHOC_MAX_EAD      256
#define OZ_EDHOC_MAX_SUITES   4
#define OZ_EDHOC_BUF          800

/* EDHOC error codes (RFC 9528 §6). */
#define OZ_EDHOC_ERR_UNSPECIFIED  1
#define OZ_EDHOC_ERR_WRONG_SUITE  2
#define OZ_EDHOC_ERR_UNKNOWN_CRED 3

/* Exporter label for the keypad pairing code (ozkey-48 §3.4), private-use range. */
#define OZ_EDHOC_EXPORTER_PAIR_CODE 0xCF50u

typedef struct {
    uint8_t len;
    uint8_t b[OZ_EDHOC_MAX_CID];
} oz_edhoc_cid;

/* An authentication credential: CRED_x bytes, the full ID_CRED_x map that
 * refers to it, and the X25519 public key inside it. */
typedef struct {
    const uint8_t *cred;
    size_t         cred_len;
    const uint8_t *id_cred;     /* e.g. {4: h'32'} = a1 04 41 32 */
    size_t         id_cred_len;
    uint8_t        pub[32];
} oz_edhoc_cred;

/* Resolve the peer's ID_CRED (full map form, after undoing the compact 'kid'
 * encoding) to its credential. Return OZ_OK, or OZ_E_NOT_FOUND for an unknown
 * credential. A 'kccs' by-value credential (map key 14) can be accepted with
 * oz_edhoc_cred_from_kccs(). Pointers must stay valid for the session. */
typedef int (*oz_edhoc_cred_fn)(void *app, const uint8_t *id_cred, size_t id_cred_len,
                                oz_edhoc_cred *out);

/* ECDH. Default (NULL) = oz_port_x25519 with an all-zero-output check. */
typedef int (*oz_edhoc_dh_fn)(void *app, uint8_t shared[32],
                              const uint8_t priv[32], const uint8_t pub[32]);

typedef struct {
    /* configuration, set before the first message */
    int            initiator;
    uint8_t        method;
    uint8_t        n_suites;
    int8_t         suites[OZ_EDHOC_MAX_SUITES]; /* I: preference order, last = selected.
                                                   R: the supported set */
    oz_edhoc_cid   c_own;          /* C_I for an initiator, C_R for a responder */
    oz_edhoc_cred  own;            /* our credential */
    uint8_t        own_priv[32];   /* our static X25519 private key */
    oz_edhoc_cred_fn get_cred;
    oz_edhoc_dh_fn dh;
    void          *app;
    int            eph_set;        /* tests: eph_priv/eph_pub given, don't generate */
    uint8_t        eph_priv[32];
    uint8_t        eph_pub[32];

    /* results */
    uint8_t        state;
    int8_t         suite;          /* the selected suite */
    oz_edhoc_cid   c_peer;
    oz_edhoc_cred  peer;           /* valid after message_2 (I) / message_3 (R) */
    const uint8_t *ead;            /* EAD_1 (R after msg 1), EAD_2 (I after msg 2), */
    size_t         ead_len;        /* EAD_3 (R after msg 3): points into the session */

    /* internal */
    uint8_t        peer_eph[32];
    uint8_t        th[32];
    uint8_t        prk_2e[32];
    uint8_t        prk_3e2m[32];
    uint8_t        prk_4e3m[32];
    uint8_t        prk_exporter[32];
    uint8_t        a[OZ_EDHOC_BUF];   /* scratch: contexts, plaintexts */
    uint8_t        b[OZ_EDHOC_BUF];   /* scratch: KDF info, transcript inputs */
    uint8_t        pt[OZ_EDHOC_BUF];  /* decrypted plaintext kept for peer/ead views */
    uint8_t        idc[OZ_EDHOC_MAX_IDCRED]; /* peer ID_CRED, rebuilt in map form */
} oz_edhoc;

/* Prepare a session. own/own_priv/c_own/get_cred are copied by the caller
 * into the struct afterwards (or through the fields directly). */
void oz_edhoc_init(oz_edhoc *s, int initiator);
void oz_edhoc_wipe(oz_edhoc *s);

/* Initiator */
int oz_edhoc_i_msg1(oz_edhoc *s, const uint8_t *ead1, size_t ead1_len,
                    uint8_t *out, size_t cap, size_t *out_len);
/* Decrypts message_2, resolves CRED_R through get_cred and verifies MAC_2. */
int oz_edhoc_i_msg2(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t cap, size_t *out_len);
/* Builds message_3 carrying EAD_3 (an already-encoded EAD sequence, or NULL). */
int oz_edhoc_i_msg3(oz_edhoc *s, const uint8_t *ead3, size_t ead3_len,
                    uint8_t *out, size_t cap, size_t *out_len);

/* Responder */
int oz_edhoc_r_msg1(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    const uint8_t *ead2, size_t ead2_len,
                    uint8_t *out, size_t cap, size_t *out_len);
/* Decrypts message_3, resolves CRED_I, verifies MAC_3. On OZ_OK the session
 * is complete: s->peer is the authenticated initiator, s->ead its EAD_3. */
int oz_edhoc_r_msg3(oz_edhoc *s, const uint8_t *in, size_t in_len,
                    uint8_t *out, size_t cap, size_t *out_len);

/* After completion (I: after msg3 is built; R: after msg3 verified). */
int oz_edhoc_exporter(oz_edhoc *s, uint32_t label, const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t len);
/* OSCORE context inputs (RFC 9528 A.1): 16-byte secret, 8-byte salt and the
 * sender / recipient IDs for this side. */
int oz_edhoc_oscore(oz_edhoc *s, uint8_t secret[16], uint8_t salt[8],
                    oz_edhoc_cid *sender_id, oz_edhoc_cid *recipient_id);
/* The 6-digit first-pairing code (ozkey-48 §3.4), 0..999999. */
int oz_edhoc_pair_code(oz_edhoc *s, uint32_t *code);

/* Helpers */
/* X25519 key out of a CCS {8: {1: {1: 1, -1: 4, -2: x}}} (OKP, X25519). */
int oz_edhoc_ccs_x25519(const uint8_t *ccs, size_t len, uint8_t pub[32]);
/* For ID_CRED = {14: CCS} (kccs): fill a credential pointing into id_cred. */
int oz_edhoc_cred_from_kccs(const uint8_t *id_cred, size_t len, oz_edhoc_cred *out);
/* Write an EDHOC error message (code 1 with text, or 3 with true). */
size_t oz_edhoc_error_msg(int code, const char *text, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* OZ_EDHOC_H */
