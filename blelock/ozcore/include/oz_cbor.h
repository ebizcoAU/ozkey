/* oz_cbor.h — the small CBOR subset OZKIE v2 needs (RFC 8949).
 *
 * Writer: deterministic encoding only (shortest-form heads, definite
 * lengths), so EDHOC transcript hashes are reproducible.
 * Reader: zero-copy views into the input. Indefinite lengths, tags, floats
 * and nesting deeper than OZ_CBOR_MAX_DEPTH are rejected as malformed.
 * Both sides keep the first error sticky: check `err` once at the end.
 */
#ifndef OZ_CBOR_H
#define OZ_CBOR_H

#include <stddef.h>
#include <stdint.h>
#include "oz_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OZ_CBOR_MAX_DEPTH 8

enum {
    OZ_CBOR_UINT  = 0,
    OZ_CBOR_NINT  = 1,
    OZ_CBOR_BSTR  = 2,
    OZ_CBOR_TSTR  = 3,
    OZ_CBOR_ARRAY = 4,
    OZ_CBOR_MAP   = 5,
    OZ_CBOR_TAG   = 6,
    OZ_CBOR_SIMPLE = 7,
};

/* ── Writer ─────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      err;   /* OZ_E_NO_SPACE once anything didn't fit */
} oz_cbor_w;

void oz_cbor_w_init(oz_cbor_w *w, uint8_t *buf, size_t cap);
void oz_cbor_put_uint(oz_cbor_w *w, uint64_t v);
void oz_cbor_put_int(oz_cbor_w *w, int64_t v);
void oz_cbor_put_bstr(oz_cbor_w *w, const uint8_t *p, size_t n);
void oz_cbor_put_tstr(oz_cbor_w *w, const char *s, size_t n);
void oz_cbor_put_array(oz_cbor_w *w, size_t count);
void oz_cbor_put_map(oz_cbor_w *w, size_t pairs);
void oz_cbor_put_bool(oz_cbor_w *w, int v);
void oz_cbor_put_null(oz_cbor_w *w);
/* Append bytes that are already CBOR (a credential, an EAD sequence). */
void oz_cbor_put_raw(oz_cbor_w *w, const uint8_t *p, size_t n);
/* Head of a bstr of length n, for content the caller appends with put_raw. */
void oz_cbor_put_bstr_head(oz_cbor_w *w, size_t n);

/* ── Reader ─────────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
    int            err;   /* OZ_E_ARG once the input is malformed */
} oz_cbor_r;

void oz_cbor_r_init(oz_cbor_r *r, const uint8_t *p, size_t len);
int  oz_cbor_at_end(const oz_cbor_r *r);
/* Major type of the next item, or -1 at end / on error. */
int  oz_cbor_peek(const oz_cbor_r *r);
/* Integer of major type 0 or 1 that fits int64. */
int64_t oz_cbor_get_int(oz_cbor_r *r);
uint64_t oz_cbor_get_uint(oz_cbor_r *r);
/* View of a byte / text string's content. */
const uint8_t *oz_cbor_get_bstr(oz_cbor_r *r, size_t *n);
const uint8_t *oz_cbor_get_tstr(oz_cbor_r *r, size_t *n);
size_t oz_cbor_get_array(oz_cbor_r *r);
size_t oz_cbor_get_map(oz_cbor_r *r);
int  oz_cbor_get_bool(oz_cbor_r *r);
/* Skip one whole item (with its children); returns a view of its encoding. */
const uint8_t *oz_cbor_skip(oz_cbor_r *r, size_t *n);

#ifdef __cplusplus
}
#endif
#endif /* OZ_CBOR_H */
