/* oz_invite.h — member invites (OZINV), verified by the lock.
 *
 * The MAC is the one the BANOI app computes (ftpos ozkey_commissioner
 * member_invite.dart, frozen vectors in member_invite_test.dart):
 *   key = HKDF-SHA256(ikm = issuer's pairing secret,
 *                     salt = utf8(device_id ‖ issuer_hex), info = "ozkey/invite-v1")
 *   mac = HMAC-SHA256(key, canonical)
 * canonical:
 *   v1  "1|d|i|r|l|n|e"                       (legacy, sunset 2026-08-24)
 *   v2  "2|d|i|r|l|n|e|me"                    (XF-87: signed membership expiry, 0 = permanent)
 *   v3  "3|d|i|r|l|n|e|me|bind|iss"           (ozkey-48 §3.3 "OZINV2": guest-key binding)
 * where i, n, bind are lowercase hex and the integers are decimal.
 *
 * In OZKIE v2 the invite travels in EDHOC EAD_3 as a CBOR array, not as the
 * QR's base64url JSON: the fields and the MAC are unchanged, and the lock
 * needs no JSON or base64 decoder. The app transcodes the QR it scanned.
 *   [v, d, i(32 B), r, l, n(16 B), e, me, m(32 B)]            v1/v2
 *   [v, d, i(32 B), r, l, n(16 B), e, me, m(32 B), bind(16 B), iss]   v3
 */
#ifndef OZ_INVITE_H
#define OZ_INVITE_H

#include "oz_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provisional EAD label (ozkey-48 §3.3), sent negative = critical. */
#define OZ_EAD_INVITE 20297

#define OZ_INVITE_MAX_DEVICE_ID 40
#define OZ_INVITE_MAX_LABEL     64

enum { OZ_INVITE_ROLE_MEMBER = 0, OZ_INVITE_ROLE_ADMIN = 1 };

typedef struct {
    uint8_t  version;                        /* 1, 2 or 3 */
    char     device_id[OZ_INVITE_MAX_DEVICE_ID + 1];
    uint8_t  issuer[32];                     /* issuer's X25519 public key (app_id) */
    uint8_t  role;
    char     label[OZ_INVITE_MAX_LABEL + 1];
    uint8_t  nonce[16];
    int64_t  expires;                        /* unix seconds: redeem-by */
    int64_t  membership_expires;             /* unix seconds, 0 = permanent (v2+) */
    uint8_t  mac[32];
    uint8_t  bind[16];                       /* v3: SHA-256(redeemer X25519 pub)[0..16) */
    uint32_t iss;                            /* v3: issuing bond slot */
} oz_invite;

int oz_invite_compute_mac(const oz_invite *inv, const uint8_t secret[32], uint8_t mac[32]);
/* OZ_OK, or OZ_E_AUTH when the MAC doesn't match. Policy (expiry, nonce reuse,
 * binding, who the issuer is) is the lock engine's job. */
int oz_invite_verify(const oz_invite *inv, const uint8_t secret[32]);

int oz_invite_to_cbor(const oz_invite *inv, uint8_t *out, size_t cap, size_t *out_len);
int oz_invite_from_cbor(oz_invite *inv, const uint8_t *in, size_t len);

/* bind value for an app key: SHA-256(pub)[0..16). */
int oz_invite_bind_of(const uint8_t pub[32], uint8_t bind[16]);

#ifdef __cplusplus
}
#endif
#endif /* OZ_INVITE_H */
