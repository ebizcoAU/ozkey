/* Member invite MAC and its CBOR rendering (see oz_invite.h). */
#include <stdio.h>
#include <string.h>
#include "oz_invite.h"
#include "oz_cbor.h"

static const char *role_wire(uint8_t role) { return role == OZ_INVITE_ROLE_ADMIN ? "admin" : "member"; }

/* Append without overflowing; returns the new length, or cap+1 on overflow. */
static size_t cat(char *buf, size_t pos, size_t cap, const char *s, size_t n)
{
    if (pos > cap || n > cap - pos)
        return cap + 1;
    memcpy(buf + pos, s, n);
    return pos + n;
}

static size_t cat_hex(char *buf, size_t pos, size_t cap, const uint8_t *b, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char two[2] = { hx[b[i] >> 4], hx[b[i] & 15] };
        pos = cat(buf, pos, cap, two, 2);
    }
    return pos;
}

static size_t cat_int(char *buf, size_t pos, size_t cap, int64_t v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", (long long)v);
    return n > 0 ? cat(buf, pos, cap, t, (size_t)n) : cap + 1;
}

int oz_invite_compute_mac(const oz_invite *inv, const uint8_t secret[32], uint8_t mac[32])
{
    if (inv->version < 1 || inv->version > 3)
        return OZ_E_ARG;
    /* salt = utf8(device_id ‖ issuer_hex) */
    char salt[OZ_INVITE_MAX_DEVICE_ID + 64];
    size_t sl = cat(salt, 0, sizeof salt, inv->device_id, strlen(inv->device_id));
    sl = cat_hex(salt, sl, sizeof salt, inv->issuer, 32);
    if (sl > sizeof salt)
        return OZ_E_ARG;

    char c[320];
    const size_t cap = sizeof c;
    size_t n = cat_int(c, 0, cap, inv->version);
    n = cat(c, n, cap, "|", 1);
    n = cat(c, n, cap, inv->device_id, strlen(inv->device_id));
    n = cat(c, n, cap, "|", 1);
    n = cat_hex(c, n, cap, inv->issuer, 32);
    n = cat(c, n, cap, "|", 1);
    n = cat(c, n, cap, role_wire(inv->role), strlen(role_wire(inv->role)));
    n = cat(c, n, cap, "|", 1);
    n = cat(c, n, cap, inv->label, strlen(inv->label));
    n = cat(c, n, cap, "|", 1);
    n = cat_hex(c, n, cap, inv->nonce, 16);
    n = cat(c, n, cap, "|", 1);
    n = cat_int(c, n, cap, inv->expires);
    if (inv->version >= 2) {
        n = cat(c, n, cap, "|", 1);
        n = cat_int(c, n, cap, inv->membership_expires);
    }
    if (inv->version >= 3) {
        n = cat(c, n, cap, "|", 1);
        n = cat_hex(c, n, cap, inv->bind, 16);
        n = cat(c, n, cap, "|", 1);
        n = cat_int(c, n, cap, inv->iss);
    }
    if (n > cap)
        return OZ_E_ARG;

    uint8_t prk[32], key[32];
    int rc = oz_hkdf_extract((const uint8_t *)salt, sl, secret, 32, prk);
    if (!rc) rc = oz_hkdf_expand(prk, (const uint8_t *)"ozkey/invite-v1", 15, key, 32);
    if (!rc) rc = oz_hmac_sha256(key, 32, (const uint8_t *)c, n, mac);
    oz_wipe(prk, sizeof prk);
    oz_wipe(key, sizeof key);
    return rc;
}

int oz_invite_verify(const oz_invite *inv, const uint8_t secret[32])
{
    uint8_t mac[32];
    int rc = oz_invite_compute_mac(inv, secret, mac);
    if (rc)
        return rc;
    return oz_ct_equal(mac, inv->mac, 32) ? OZ_E_AUTH : OZ_OK;
}

int oz_invite_to_cbor(const oz_invite *inv, uint8_t *out, size_t cap, size_t *out_len)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, out, cap);
    oz_cbor_put_array(&w, inv->version >= 3 ? 11 : 9);
    oz_cbor_put_uint(&w, inv->version);
    oz_cbor_put_tstr(&w, inv->device_id, strlen(inv->device_id));
    oz_cbor_put_bstr(&w, inv->issuer, 32);
    oz_cbor_put_tstr(&w, role_wire(inv->role), strlen(role_wire(inv->role)));
    oz_cbor_put_tstr(&w, inv->label, strlen(inv->label));
    oz_cbor_put_bstr(&w, inv->nonce, 16);
    oz_cbor_put_int(&w, inv->expires);
    oz_cbor_put_int(&w, inv->membership_expires);
    oz_cbor_put_bstr(&w, inv->mac, 32);
    if (inv->version >= 3) {
        oz_cbor_put_bstr(&w, inv->bind, 16);
        oz_cbor_put_uint(&w, inv->iss);
    }
    if (w.err)
        return w.err;
    *out_len = w.len;
    return OZ_OK;
}

static int get_fixed(oz_cbor_r *r, uint8_t *out, size_t n)
{
    size_t l;
    const uint8_t *p = oz_cbor_get_bstr(r, &l);
    if (!p || l != n)
        return OZ_E_ARG;
    memcpy(out, p, n);
    return OZ_OK;
}

static int get_text(oz_cbor_r *r, char *out, size_t max)
{
    size_t l;
    const uint8_t *p = oz_cbor_get_tstr(r, &l);
    if (!p || l > max || memchr(p, 0, l) || memchr(p, '|', l))
        return OZ_E_ARG;   /* '|' would make the canonical string ambiguous */
    memcpy(out, p, l);
    out[l] = 0;
    return OZ_OK;
}

int oz_invite_from_cbor(oz_invite *inv, const uint8_t *in, size_t len)
{
    memset(inv, 0, sizeof *inv);
    oz_cbor_r r;
    oz_cbor_r_init(&r, in, len);
    size_t n = oz_cbor_get_array(&r);
    uint64_t v = oz_cbor_get_uint(&r);
    if (r.err || v < 1 || v > 3 || n != (v >= 3 ? 11u : 9u))
        return OZ_E_ARG;
    inv->version = (uint8_t)v;
    char role[8];
    if (get_text(&r, inv->device_id, OZ_INVITE_MAX_DEVICE_ID) ||
        get_fixed(&r, inv->issuer, 32) ||
        get_text(&r, role, sizeof role - 1))
        return OZ_E_ARG;
    if (!strcmp(role, "member"))
        inv->role = OZ_INVITE_ROLE_MEMBER;
    else if (!strcmp(role, "admin"))
        inv->role = OZ_INVITE_ROLE_ADMIN;
    else
        return OZ_E_ARG;
    if (get_text(&r, inv->label, OZ_INVITE_MAX_LABEL) || get_fixed(&r, inv->nonce, 16))
        return OZ_E_ARG;
    inv->expires = oz_cbor_get_int(&r);
    inv->membership_expires = oz_cbor_get_int(&r);
    if (get_fixed(&r, inv->mac, 32))
        return OZ_E_ARG;
    if (inv->version >= 3) {
        if (get_fixed(&r, inv->bind, 16))
            return OZ_E_ARG;
        uint64_t iss = oz_cbor_get_uint(&r);
        if (iss > 0xffffffffu)
            return OZ_E_ARG;
        inv->iss = (uint32_t)iss;
    }
    if (!oz_cbor_at_end(&r) || (inv->version == 1 && inv->membership_expires != 0))
        return OZ_E_ARG;
    return OZ_OK;
}

int oz_invite_bind_of(const uint8_t pub[32], uint8_t bind[16])
{
    uint8_t h[32];
    int rc = oz_sha256(pub, 32, h);
    if (!rc)
        memcpy(bind, h, 16);
    return rc;
}
