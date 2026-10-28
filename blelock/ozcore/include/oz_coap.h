/* oz_coap.h — CoAP message codec (RFC 7252 §3), used inside OZKIE frames.
 *
 * OZKIE carries CoAP in the RFC 7252 message format (4-byte header, token,
 * options, payload) over its BLE / MQTT frames instead of UDP (ozkey-48 §4).
 * Zero-copy: option values and the payload point into caller buffers.
 */
#ifndef OZ_COAP_H
#define OZ_COAP_H

#include <stddef.h>
#include <stdint.h>
#include "oz_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZ_COAP_MAX_OPTS  12
#define OZ_COAP_MAX_TOKEN  8

/* Types */
enum { OZ_COAP_CON = 0, OZ_COAP_NON = 1, OZ_COAP_ACK = 2, OZ_COAP_RST = 3 };

/* Codes: class << 5 | detail */
#define OZ_COAP_CODE(c, d)  ((uint8_t)(((c) << 5) | (d)))
enum {
    OZ_COAP_EMPTY   = 0x00,
    OZ_COAP_GET     = 0x01, OZ_COAP_POST = 0x02, OZ_COAP_PUT = 0x03, OZ_COAP_DELETE = 0x04,
    OZ_COAP_FETCH   = 0x05,
    OZ_COAP_CREATED = 0x41, OZ_COAP_DELETED = 0x42, OZ_COAP_VALID = 0x43,
    OZ_COAP_CHANGED = 0x44, OZ_COAP_CONTENT = 0x45,
    OZ_COAP_BAD_REQUEST = 0x80, OZ_COAP_UNAUTHORIZED = 0x81, OZ_COAP_BAD_OPTION = 0x82,
    OZ_COAP_FORBIDDEN = 0x83, OZ_COAP_NOT_FOUND = 0x84, OZ_COAP_METHOD_NOT_ALLOWED = 0x85,
    OZ_COAP_PRECONDITION_FAILED = 0x8c,
    OZ_COAP_INTERNAL_ERROR = 0xa0, OZ_COAP_NOT_IMPLEMENTED = 0xa1,
    OZ_COAP_SERVICE_UNAVAILABLE = 0xa3,
};

/* Option numbers */
enum {
    OZ_COAP_OPT_IF_MATCH = 1, OZ_COAP_OPT_URI_HOST = 3, OZ_COAP_OPT_ETAG = 4,
    OZ_COAP_OPT_OBSERVE = 6, OZ_COAP_OPT_URI_PORT = 7, OZ_COAP_OPT_OSCORE = 9,
    OZ_COAP_OPT_URI_PATH = 11, OZ_COAP_OPT_CONTENT_FORMAT = 12, OZ_COAP_OPT_MAX_AGE = 14,
    OZ_COAP_OPT_URI_QUERY = 15, OZ_COAP_OPT_ACCEPT = 17, OZ_COAP_OPT_BLOCK2 = 23,
    OZ_COAP_OPT_BLOCK1 = 27, OZ_COAP_OPT_PROXY_URI = 35, OZ_COAP_OPT_PROXY_SCHEME = 39,
    OZ_COAP_OPT_ECHO = 252, OZ_COAP_OPT_NO_RESPONSE = 258,
};

#define OZ_COAP_CF_CBOR 60

typedef struct {
    uint16_t       num;
    uint16_t       len;
    const uint8_t *val;
} oz_coap_opt;

typedef struct {
    uint8_t        type;
    uint8_t        code;
    uint16_t       mid;
    uint8_t        tkl;
    uint8_t        token[OZ_COAP_MAX_TOKEN];
    uint8_t        n_opts;
    oz_coap_opt    opts[OZ_COAP_MAX_OPTS];   /* kept sorted by number */
    const uint8_t *payload;
    size_t         payload_len;
} oz_coap_msg;

void oz_coap_init(oz_coap_msg *m, uint8_t type, uint8_t code, uint16_t mid);
/* Insert keeping ascending order; repeats stay in insertion order. */
int  oz_coap_add_opt(oz_coap_msg *m, uint16_t num, const uint8_t *val, size_t len);
/* Uri-Path segments from "a/b/c". The string must outlive the message. */
int  oz_coap_set_path(oz_coap_msg *m, const char *path);
/* Uint option value in network order, shortest form (0 → empty). buf ≥ 4. */
int  oz_coap_add_uint_opt(oz_coap_msg *m, uint16_t num, uint32_t v, uint8_t buf[4]);
const oz_coap_opt *oz_coap_find(const oz_coap_msg *m, uint16_t num);
uint32_t oz_coap_opt_uint(const oz_coap_opt *o);
/* Uri-Path joined with '/' into out (NUL-terminated). */
int  oz_coap_path(const oz_coap_msg *m, char *out, size_t cap);

/* Whole message (header + token + options + payload). */
int  oz_coap_encode(const oz_coap_msg *m, uint8_t *out, size_t cap, size_t *out_len);
int  oz_coap_decode(oz_coap_msg *m, const uint8_t *in, size_t len);

/* The option run alone (delta-encoded from 0), optionally followed by
 * 0xff + payload: OSCORE's plaintext body after the code byte. */
int  oz_coap_encode_opts(const oz_coap_opt *opts, size_t n, const uint8_t *payload,
                         size_t payload_len, uint8_t *out, size_t cap, size_t *out_len);
int  oz_coap_decode_opts(oz_coap_msg *m, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* OZ_COAP_H */
