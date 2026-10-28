/* The OZKIE v2 lock engine. Section numbers are ozkey-48's unless named. */
#include <stdio.h>
#include <string.h>
#include "oz_lock.h"
#include "oz_cbor.h"

#define PAIR_WINDOW_MS     60000u
#define PAIR_LOCKOUT_MS    300000u
#define PAIR_MAX_FAILURES  5
#define ECHO_WINDOW_MS     10000u
#define RELAY_SSN_BLOCK    32
#define ISSUER_START_SLACK 3600     /* invite may start ≤ 1 h before now (§6) */
#define LOCK_KID           0x00

static const uint8_t CCS_PREFIX[11] = { 0xa1, 0x08, 0xa1, 0x01, 0xa3, 0x01, 0x01, 0x20, 0x04, 0x21, 0x58 };

void oz_ccs_for(const uint8_t pub[32], uint8_t ccs[OZ_CCS_LEN])
{
    memcpy(ccs, CCS_PREFIX, sizeof CCS_PREFIX);
    ccs[11] = 0x20;
    memcpy(ccs + 12, pub, 32);
}

/* ── clock ──────────────────────────────────────────────────────────────── */

int64_t oz_lock_utc(const oz_lock *L)
{
    if (!L->clock_set)
        return 0;
    return L->utc_at_set + (int64_t)((oz_port_monotonic_ms() - L->mono_at_set) / 1000u);
}

/* ── persistence (CBOR records, versioned by a leading 1) ───────────────── */

static void kv_name(char *buf, char c, int i) { snprintf(buf, 4, "%c%d", c, i); }

static int save_bond(const oz_lock *L, int i)
{
    char k[4];
    kv_name(k, 'b', i);
    const oz_bond *b = &L->bonds[i];
    if (!b->used) {
        (void)oz_port_kv_erase(k);   /* absent already is fine */
        return OZ_OK;
    }
    uint8_t buf[96];
    oz_cbor_w w;
    oz_cbor_w_init(&w, buf, sizeof buf);
    oz_cbor_put_array(&w, 6);
    oz_cbor_put_uint(&w, 1);
    oz_cbor_put_uint(&w, b->role);
    oz_cbor_put_bstr(&w, b->pub, 32);
    oz_cbor_put_tstr(&w, b->label, strlen(b->label));
    oz_cbor_put_int(&w, b->expires);
    oz_cbor_put_uint(&w, b->max_days);
    return w.err ? w.err : oz_port_kv_set(k, buf, w.len);
}

static void load_bond(oz_lock *L, int i)
{
    char k[4];
    kv_name(k, 'b', i);
    uint8_t buf[96];
    size_t n;
    memset(&L->bonds[i], 0, sizeof L->bonds[i]);
    if (oz_port_kv_get(k, buf, sizeof buf, &n))
        return;
    oz_cbor_r r;
    oz_cbor_r_init(&r, buf, n);
    oz_bond b = { 0 };
    size_t pl, ll;
    if (oz_cbor_get_array(&r) != 6 || oz_cbor_get_uint(&r) != 1)
        return;
    b.role = (uint8_t)oz_cbor_get_uint(&r);
    const uint8_t *p = oz_cbor_get_bstr(&r, &pl);
    const uint8_t *lab = oz_cbor_get_tstr(&r, &ll);
    b.expires = oz_cbor_get_int(&r);
    b.max_days = (uint16_t)oz_cbor_get_uint(&r);
    if (r.err || !p || pl != 32 || ll > OZ_LOCK_LABEL)
        return;
    memcpy(b.pub, p, 32);
    memcpy(b.label, lab, ll);
    b.used = 1;
    L->bonds[i] = b;
}

static int save_u32(const char *key, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return oz_port_kv_set(key, b, 4);
}

static uint32_t load_u32(const char *key)
{
    uint8_t b[4];
    size_t n;
    if (oz_port_kv_get(key, b, 4, &n) || n != 4)
        return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static int save_nonces(const oz_lock *L)
{
    uint8_t buf[1 + OZ_LOCK_NONCES * 24];
    buf[0] = L->nonce_next;
    memcpy(buf + 1, L->nonces, sizeof L->nonces);
    return oz_port_kv_set("nc", buf, sizeof buf);
}

static int save_creds(const oz_lock *L)
{
    uint8_t buf[1024];
    oz_cbor_w w;
    oz_cbor_w_init(&w, buf, sizeof buf);
    size_t n = 0;
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++)
        n += L->creds[i].used;
    oz_cbor_put_array(&w, 2 + n);
    oz_cbor_put_uint(&w, 1);
    oz_cbor_put_uint(&w, L->next_cred_id);
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++) {
        const oz_cred *c = &L->creds[i];
        if (!c->used)
            continue;
        oz_cbor_put_array(&w, 6);
        oz_cbor_put_uint(&w, c->id);
        oz_cbor_put_uint(&w, c->type);
        oz_cbor_put_bstr(&w, c->secret, c->secret_len);
        oz_cbor_put_int(&w, c->from);
        oz_cbor_put_int(&w, c->to);
        oz_cbor_put_int(&w, c->uses);
    }
    return w.err ? w.err : oz_port_kv_set("cr", buf, w.len);
}

static void load_creds(oz_lock *L)
{
    uint8_t buf[1024];
    size_t n;
    memset(L->creds, 0, sizeof L->creds);
    L->next_cred_id = 1;
    if (oz_port_kv_get("cr", buf, sizeof buf, &n))
        return;
    oz_cbor_r r;
    oz_cbor_r_init(&r, buf, n);
    size_t cnt = oz_cbor_get_array(&r);
    if (cnt < 2 || oz_cbor_get_uint(&r) != 1)
        return;
    L->next_cred_id = (uint16_t)oz_cbor_get_uint(&r);
    for (size_t i = 0; i + 2 < cnt && i < OZ_LOCK_MAX_CREDS && !r.err; i++) {
        oz_cred *c = &L->creds[i];
        size_t sl;
        if (oz_cbor_get_array(&r) != 6)
            break;
        c->id = (uint16_t)oz_cbor_get_uint(&r);
        c->type = (uint8_t)oz_cbor_get_uint(&r);
        const uint8_t *s = oz_cbor_get_bstr(&r, &sl);
        c->from = oz_cbor_get_int(&r);
        c->to = oz_cbor_get_int(&r);
        c->uses = (int32_t)oz_cbor_get_int(&r);
        if (r.err || !s || sl > OZ_LOCK_CRED_SECRET)
            break;
        memcpy(c->secret, s, sl);
        c->secret_len = (uint8_t)sl;
        c->used = 1;
    }
}

/* Relay context record: the keys, IDs and the next sequence number to use
 * after a reboot (B.1.1). The replay window is never stored: it restarts
 * UNKNOWN, so the first request is answered with Echo (§4.1). */
static int relay_persist(void *app, uint64_t ssn_to_store);

static int save_relay(oz_lock *L, int i, uint64_t ssn_to_store)
{
    char k[4];
    kv_name(k, 'r', i);
    if (!L->relay_used[i]) {
        (void)oz_port_kv_erase(k);
        return OZ_OK;
    }
    const oz_oscore_ctx *c = &L->relay[i];
    uint8_t buf[96];
    oz_cbor_w w;
    oz_cbor_w_init(&w, buf, sizeof buf);
    oz_cbor_put_array(&w, 7);
    oz_cbor_put_uint(&w, 1);
    oz_cbor_put_bstr(&w, c->sender_key, 16);
    oz_cbor_put_bstr(&w, c->recipient_key, 16);
    oz_cbor_put_bstr(&w, c->common_iv, 13);
    oz_cbor_put_bstr(&w, c->sender_id, c->sender_id_len);
    oz_cbor_put_bstr(&w, c->recipient_id, c->recipient_id_len);
    oz_cbor_put_uint(&w, ssn_to_store);
    return w.err ? w.err : oz_port_kv_set(k, buf, w.len);
}

typedef struct { oz_lock *L; int slot; } relay_ref;
static relay_ref relay_refs[OZ_LOCK_MAX_BONDS];

static int relay_persist(void *app, uint64_t ssn_to_store)
{
    relay_ref *ref = app;
    return save_relay(ref->L, ref->slot, ssn_to_store);
}

static void relay_hook(oz_lock *L, int i)
{
    relay_refs[i].L = L;
    relay_refs[i].slot = i;
    L->relay[i].persist = relay_persist;
    L->relay[i].app = &relay_refs[i];
    L->relay[i].block = RELAY_SSN_BLOCK;
}

static void load_relay(oz_lock *L, int i)
{
    char k[4];
    kv_name(k, 'r', i);
    uint8_t buf[96];
    size_t n;
    L->relay_used[i] = 0;
    if (oz_port_kv_get(k, buf, sizeof buf, &n))
        return;
    oz_cbor_r r;
    oz_cbor_r_init(&r, buf, n);
    oz_oscore_ctx *c = &L->relay[i];
    memset(c, 0, sizeof *c);
    size_t l1, l2, l3, l4, l5;
    if (oz_cbor_get_array(&r) != 7 || oz_cbor_get_uint(&r) != 1)
        return;
    const uint8_t *sk = oz_cbor_get_bstr(&r, &l1), *rk = oz_cbor_get_bstr(&r, &l2),
                  *iv = oz_cbor_get_bstr(&r, &l3), *sid = oz_cbor_get_bstr(&r, &l4),
                  *rid = oz_cbor_get_bstr(&r, &l5);
    uint64_t ssn = oz_cbor_get_uint(&r);
    if (r.err || l1 != 16 || l2 != 16 || l3 != 13 || l4 > OZ_OSCORE_MAX_ID || l5 > OZ_OSCORE_MAX_ID)
        return;
    memcpy(c->sender_key, sk, 16);
    memcpy(c->recipient_key, rk, 16);
    memcpy(c->common_iv, iv, 13);
    memcpy(c->sender_id, sid, l4);
    c->sender_id_len = (uint8_t)l4;
    memcpy(c->recipient_id, rid, l5);
    c->recipient_id_len = (uint8_t)l5;
    c->ssn = ssn;
    c->ssn_limit = ssn;
    c->rw_state = OZ_OSCORE_RW_UNKNOWN;
    relay_hook(L, i);
    L->relay_used[i] = 1;
}

static void drop_relay(oz_lock *L, int i)
{
    oz_oscore_wipe(&L->relay[i]);
    L->relay_used[i] = 0;
    save_relay(L, i, 0);
}

/* ── setup ──────────────────────────────────────────────────────────────── */

static int mint_identity(oz_lock *L)
{
    int rc = oz_port_random(L->priv, 32);
    if (!rc) rc = oz_port_x25519_public_key(L->pub, L->priv);
    if (!rc) rc = oz_port_kv_set("lk", L->priv, 32);
    return rc;
}

int oz_lock_init(oz_lock *L, const char *device_id, const char *fw, const oz_lock_hooks *hooks)
{
    memset(L, 0, sizeof *L);
    if (strlen(device_id) > OZ_INVITE_MAX_DEVICE_ID)
        return OZ_E_ARG;
    strcpy(L->device_id, device_id);
    L->fw = fw;
    if (hooks)
        L->hooks = *hooks;
    size_t n;
    int rc = oz_port_kv_get("lk", L->priv, 32, &n);
    if (rc || n != 32)
        rc = mint_identity(L);
    else
        rc = oz_port_x25519_public_key(L->pub, L->priv);
    if (rc)
        return rc;
    oz_ccs_for(L->pub, L->ccs);
    for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++) {
        load_bond(L, i);
        load_relay(L, i);
    }
    L->epoch = load_u32("ep");
    uint8_t nb[1 + OZ_LOCK_NONCES * 24];
    if (!oz_port_kv_get("nc", nb, sizeof nb, &n) && n == sizeof nb) {
        L->nonce_next = nb[0] % OZ_LOCK_NONCES;
        memcpy(L->nonces, nb + 1, sizeof L->nonces);
    }
    load_creds(L);
    L->lock_state = OZ_LOCKSTATE_LOCKED;
    L->battery = 100;
    return OZ_OK;
}

int oz_lock_claimed(const oz_lock *L) { return L->bonds[0].used && L->bonds[0].role == OZ_ROLE_ADMIN; }

int oz_lock_info(const oz_lock *L, uint8_t *out, size_t cap, size_t *out_len)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, out, cap);
    oz_cbor_put_map(&w, 5);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_tstr(&w, L->device_id, strlen(L->device_id));
    oz_cbor_put_uint(&w, 2); oz_cbor_put_tstr(&w, L->fw, strlen(L->fw));
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, L->pub, 32);
    oz_cbor_put_uint(&w, 4); oz_cbor_put_uint(&w, oz_lock_claimed(L) ? 1 : 0);
    oz_cbor_put_uint(&w, 5); oz_cbor_put_tstr(&w, "ak1037-dev", 10);
    if (w.err)
        return w.err;
    *out_len = w.len;
    return OZ_OK;
}

void oz_link_open(oz_link *k, int relay)
{
    memset(k, 0, sizeof *k);
    k->relay = relay;
    k->slot = -1;
    k->relay_ctx_slot = -1;
}

void oz_link_close(oz_link *k)
{
    oz_edhoc_wipe(&k->edhoc);
    oz_oscore_wipe(&k->osc);
    int relay = k->relay;
    oz_link_open(k, relay);
}

/* ── bonds ──────────────────────────────────────────────────────────────── */

static int bond_live(const oz_lock *L, int i)
{
    if (i < 0 || i >= OZ_LOCK_MAX_BONDS || !L->bonds[i].used)
        return 0;
    int64_t now = oz_lock_utc(L);
    return !(L->bonds[i].expires && now && now > L->bonds[i].expires);
}

static int bond_by_pub(const oz_lock *L, const uint8_t pub[32])
{
    for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++)
        if (L->bonds[i].used && !memcmp(L->bonds[i].pub, pub, 32))
            return i;
    return -1;
}

static int free_slot(const oz_lock *L)
{
    for (int i = 1; i < OZ_LOCK_MAX_BONDS; i++)
        if (!L->bonds[i].used)
            return i;
    return -1;
}

static void bump_epoch(oz_lock *L)
{
    L->epoch++;
    save_u32("ep", L->epoch);
}

static void revoke(oz_lock *L, int i)
{
    memset(&L->bonds[i], 0, sizeof L->bonds[i]);
    save_bond(L, i);
    if (L->relay_used[i])
        drop_relay(L, i);
}

static int nonce_seen(const oz_lock *L, const uint8_t nonce[16], const uint8_t **tag)
{
    for (int i = 0; i < OZ_LOCK_NONCES; i++)
        if (!memcmp(L->nonces[i].nonce, nonce, 16)) {
            static const uint8_t zero16[16];
            if (!memcmp(nonce, zero16, 16) && !memcmp(L->nonces[i].tag, zero16, 8))
                continue;   /* empty slot */
            *tag = L->nonces[i].tag;
            return 1;
        }
    return 0;
}

static void nonce_burn(oz_lock *L, const uint8_t nonce[16], const uint8_t *tag8)
{
    oz_nonce_entry *e = &L->nonces[L->nonce_next];
    memcpy(e->nonce, nonce, 16);
    if (tag8)
        memcpy(e->tag, tag8, 8);
    else
        memset(e->tag, 0, 8);
    L->nonce_next = (uint8_t)((L->nonce_next + 1) % OZ_LOCK_NONCES);
    save_nonces(L);
}

/* ── EDHOC (lock = responder) ───────────────────────────────────────────── */

/* ID_CRED_I → CRED_I: {4: slot} for a bond, {14: canonical CCS} otherwise. */
static int lock_get_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    oz_link *k = app;
    oz_lock *L = k->lock;
    if (len == 4 && id[0] == 0xa1 && id[1] == 0x04 && id[2] == 0x41) {
        int slot = id[3];
        if (!bond_live(L, slot))
            return OZ_E_NOT_FOUND;
        oz_ccs_for(L->bonds[slot].pub, k->peer_ccs);
        out->cred = k->peer_ccs;
        out->cred_len = OZ_CCS_LEN;
        memcpy(out->pub, L->bonds[slot].pub, 32);
        return OZ_OK;
    }
    /* Anything we can't use is "unknown credential", so the app gets an EDHOC
     * error back rather than silence. */
    if (oz_edhoc_cred_from_kccs(id, len, out))
        return OZ_E_NOT_FOUND;
    oz_ccs_for(out->pub, k->peer_ccs);
    if (out->cred_len != OZ_CCS_LEN || memcmp(out->cred, k->peer_ccs, OZ_CCS_LEN))
        return OZ_E_NOT_FOUND;   /* only the canonical form (§3) */
    out->cred = k->peer_ccs;
    return OZ_OK;
}

static uint8_t pick_c_r(const oz_lock *L, const oz_link *k)
{
    if (!k->relay)
        return 0x01;
    for (uint8_t c = 0x02; c <= 0x17; c++) {
        int taken = 0;
        for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++)
            if (L->relay_used[i] && L->relay[i].recipient_id_len == 1 && L->relay[i].recipient_id[0] == c)
                taken = 1;
        if (!taken)
            return c;
    }
    return 0x18;
}

static size_t edhoc_refuse(oz_link *k, const char *why, uint8_t *out, size_t cap)
{
    oz_edhoc_wipe(&k->edhoc);
    k->edhoc_active = 0;
    return oz_edhoc_error_msg(OZ_EDHOC_ERR_UNSPECIFIED, why, out, cap);
}

/* Find the invite EAD item; refuse any other critical item (RFC 9528 §3.8). */
static int find_invite(const uint8_t *ead, size_t len, const uint8_t **val, size_t *vlen)
{
    oz_cbor_r r;
    oz_cbor_r_init(&r, ead, len);
    *val = NULL;
    while (!r.err && r.pos < r.len) {
        int64_t label = oz_cbor_get_int(&r);
        const uint8_t *v = NULL;
        size_t n = 0;
        if (oz_cbor_peek(&r) == OZ_CBOR_BSTR)
            v = oz_cbor_get_bstr(&r, &n);
        if (label == -OZ_EAD_INVITE || label == OZ_EAD_INVITE) {
            *val = v;
            *vlen = n;
        } else if (label < 0) {
            return OZ_E_ARG;
        }
    }
    return r.err ? OZ_E_ARG : OZ_OK;
}

/* §3.3: redeem an invite for the key that just authenticated. */
static int redeem_invite(oz_lock *L, const uint8_t *val, size_t vlen, const uint8_t pub[32], int *slot)
{
    oz_invite inv;
    if (oz_invite_from_cbor(&inv, val, vlen) || inv.role != OZ_INVITE_ROLE_MEMBER ||
        strcmp(inv.device_id, L->device_id))
        return OZ_R_BOND_DENIED;
    int is = bond_by_pub(L, inv.issuer);
    if (!bond_live(L, is) || (L->bonds[is].role != OZ_ROLE_ADMIN && L->bonds[is].role != OZ_ROLE_ISSUER))
        return OZ_R_BOND_DENIED;
    int issuer_role = L->bonds[is].role;
    if (issuer_role == OZ_ROLE_ISSUER && (inv.version < 3 || inv.iss != (uint32_t)is))
        return OZ_R_BOND_DENIED;
    if (inv.version >= 3 && inv.iss != (uint32_t)is)
        return OZ_R_BOND_DENIED;
    uint8_t secret[32];
    int rc = oz_port_x25519(secret, L->priv, inv.issuer);
    if (!rc) rc = oz_invite_verify(&inv, secret);
    oz_wipe(secret, sizeof secret);
    if (rc)
        return OZ_R_BOND_DENIED;
    if (inv.version >= 3) {
        uint8_t bind[16];
        oz_invite_bind_of(pub, bind);
        if (oz_ct_equal(bind, inv.bind, 16))
            return OZ_R_INVITE_BOUND_ELSEWHERE;   /* not consumed */
    }
    int64_t now = oz_lock_utc(L);
    if (now && inv.expires && now > inv.expires)
        return OZ_R_MEMBER_EXPIRED;
    if (issuer_role == OZ_ROLE_ISSUER) {
        /* Bounded guest windows (§6): a membership end is required, no further
         * out than max_days. Without a set clock the bound can't be checked. */
        if (!inv.membership_expires || !now ||
            inv.membership_expires > now + (int64_t)L->bonds[is].max_days * 86400 ||
            inv.expires < now - ISSUER_START_SLACK)
            return OZ_R_BOND_DENIED;
    }
    const uint8_t *tag;
    if (nonce_seen(L, inv.nonce, &tag)) {
        /* Idempotent retry by the same key: hand back its existing bond. */
        int existing = bond_by_pub(L, pub);
        if (existing > 0 && !memcmp(tag, pub, 8)) {
            *slot = existing;
            return OZ_R_MEMBER_OK;
        }
        return OZ_R_MEMBER_REPLAY;
    }
    int s = free_slot(L);
    if (s < 0)
        return OZ_R_MEMBER_FULL;
    oz_bond *b = &L->bonds[s];
    memset(b, 0, sizeof *b);
    b->used = 1;
    b->role = OZ_ROLE_MEMBER;
    memcpy(b->pub, pub, 32);
    strncpy(b->label, inv.label, OZ_LOCK_LABEL);
    b->expires = inv.membership_expires;
    save_bond(L, s);
    nonce_burn(L, inv.nonce, pub);   /* only successful enrolments are recorded */
    bump_epoch(L);
    *slot = s;
    return OZ_R_MEMBER_OK;
}

static const char *rname(int r)
{
    switch (r) {
    case OZ_R_BOND_DENIED: return "BOND_DENIED";
    case OZ_R_MEMBER_REPLAY: return "MEMBER_REPLAY";
    case OZ_R_MEMBER_FULL: return "MEMBER_FULL";
    case OZ_R_MEMBER_EXPIRED: return "MEMBER_EXPIRED";
    case OZ_R_INVITE_BOUND_ELSEWHERE: return "INVITE_BOUND_ELSEWHERE";
    case OZ_R_PAIR_LOCKED_OUT: return "PAIR_LOCKED_OUT";
    default: return "DENIED";
    }
}

/* message_3 verified: decide what this key may do (§3.1). */
static int after_msg3(oz_lock *L, oz_link *k, uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    const uint8_t *pub = k->edhoc.peer.pub;
    int slot = bond_by_pub(L, pub);
    int verdict = OZ_R_OK;
    uint64_t now_ms = oz_port_monotonic_ms();

    if (!oz_lock_claimed(L)) {
        if (k->relay)
            verdict = OZ_R_BOND_DENIED;          /* custodial claim: ozkey-51, later */
        else if (now_ms < L->pair_lockout_until_ms)
            verdict = OZ_R_PAIR_LOCKED_OUT;
        else {
            k->pairing = 1;
            memcpy(k->pair_pub, pub, 32);
            oz_edhoc_pair_code(&k->edhoc, &k->pair_code);
            k->pair_deadline_ms = now_ms + PAIR_WINDOW_MS;
            if (L->hooks.pair_prompt)
                L->hooks.pair_prompt(L->hooks.app, 1);
        }
    } else if (slot >= 0) {
        if (!bond_live(L, slot))
            verdict = OZ_R_EXPIRED;
        else if (k->relay && L->bonds[slot].role != OZ_ROLE_ADMIN && L->bonds[slot].role != OZ_ROLE_SERVICE)
            verdict = OZ_R_BOND_DENIED;          /* members on premises only (§4.1) */
    } else {
        const uint8_t *val;
        size_t vlen;
        if (k->relay || find_invite(k->edhoc.ead, k->edhoc.ead_len, &val, &vlen) || !val)
            verdict = OZ_R_BOND_DENIED;
        else {
            int r = redeem_invite(L, val, vlen, pub, &slot);
            if (r != OZ_R_MEMBER_OK)
                verdict = r;
        }
    }
    if (verdict != OZ_R_OK) {
        *out_type = OZ_FRAME_EDHOC;
        *out_len = edhoc_refuse(k, rname(verdict), out, cap);
        return OZ_OK;
    }

    uint8_t secret[16], salt[8];
    oz_edhoc_cid sid, rid;
    oz_oscore_ctx *c = k->relay ? &L->relay[slot] : &k->osc;
    int rc = oz_edhoc_oscore(&k->edhoc, secret, salt, &sid, &rid);
    if (!rc) rc = oz_oscore_derive(c, secret, 16, salt, 8, sid.b, sid.len, rid.b, rid.len, NULL, 0);
    oz_wipe(secret, sizeof secret);
    oz_edhoc_wipe(&k->edhoc);
    k->edhoc_active = 0;
    if (rc)
        return rc;
    if (k->relay) {
        relay_hook(L, slot);
        L->relay_used[slot] = 1;
        save_relay(L, slot, c->ssn + RELAY_SSN_BLOCK);
        c->ssn_limit = c->ssn + RELAY_SSN_BLOCK;
    } else {
        k->ctx_ready = 1;
        k->slot = k->pairing ? -1 : slot;
    }
    *out_len = 0;
    return OZ_OK;
}

static int rx_edhoc(oz_lock *L, oz_link *k, const uint8_t *in, size_t len,
                    uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_type = OZ_FRAME_EDHOC;
    if (!k->edhoc_active) {
        /* message_1: a new handshake replaces whatever this link had. */
        oz_edhoc_init(&k->edhoc, 0);
        k->edhoc.c_own.len = 1;
        k->edhoc.c_own.b[0] = pick_c_r(L, k);
        k->edhoc.own.cred = L->ccs;
        k->edhoc.own.cred_len = OZ_CCS_LEN;
        static const uint8_t lock_idc[4] = { 0xa1, 0x04, 0x41, LOCK_KID };
        k->edhoc.own.id_cred = lock_idc;
        k->edhoc.own.id_cred_len = 4;
        memcpy(k->edhoc.own.pub, L->pub, 32);
        memcpy(k->edhoc.own_priv, L->priv, 32);
        k->edhoc.get_cred = lock_get_cred;
        k->edhoc.app = k;
        k->ctx_ready = 0;
        k->pairing = 0;
        int rc = oz_edhoc_r_msg1(&k->edhoc, in, len, NULL, 0, out, cap, out_len);
        if (rc == OZ_OK)
            k->edhoc_active = 1;
        return rc == OZ_EDHOC_SEND_ERROR ? OZ_OK : rc;
    }
    int rc = oz_edhoc_r_msg3(&k->edhoc, in, len, out, cap, out_len);
    if (rc != OZ_OK) {
        k->edhoc_active = 0;
        return rc == OZ_EDHOC_SEND_ERROR ? OZ_OK : rc;
    }
    return after_msg3(L, k, out_type, out, cap, out_len);
}

/* ── OSCORE requests ────────────────────────────────────────────────────── */

typedef struct {
    oz_lock *L;
    oz_link *k;
    oz_oscore_ctx *ctx;
    int slot;
    int role;
    const oz_coap_msg *req;
    oz_oscore_xchg *x;
    char path[48];
    oz_cbor_r body;          /* request payload */
    uint8_t res[256];        /* response payload */
    oz_cbor_w w;
    uint8_t code;            /* response CoAP code */
} req_ctx;

static int respond(req_ctx *q, int with_piv, uint8_t *out, size_t cap, size_t *out_len)
{
    oz_coap_msg m;
    oz_coap_init(&m, q->req->type == OZ_COAP_CON ? OZ_COAP_ACK : OZ_COAP_NON, q->code, q->req->mid);
    m.tkl = q->req->tkl;
    memcpy(m.token, q->req->token, q->req->tkl);
    if (q->w.len) {
        m.payload = q->res;
        m.payload_len = q->w.len;
    }
    return oz_oscore_protect_resp(q->ctx, q->x, &m, with_piv, out, cap, out_len);
}

static void result(req_ctx *q, int r, size_t extra_pairs)
{
    oz_cbor_put_map(&q->w, 1 + extra_pairs);
    oz_cbor_put_uint(&q->w, 0);
    oz_cbor_put_uint(&q->w, (uint64_t)r);
}

/* Body map helpers: find integer key in the request's CBOR map. */
static int body_find(const req_ctx *q, int64_t key, oz_cbor_r *at)
{
    oz_cbor_r r = q->body;
    if (oz_cbor_peek(&r) != OZ_CBOR_MAP)
        return 0;
    size_t pairs = oz_cbor_get_map(&r);
    for (size_t i = 0; i < pairs && !r.err; i++) {
        int m = oz_cbor_peek(&r);
        if (m != OZ_CBOR_UINT && m != OZ_CBOR_NINT)
            return 0;
        if (oz_cbor_get_int(&r) == key) {
            *at = r;
            return 1;
        }
        size_t n;
        (void)oz_cbor_skip(&r, &n);
    }
    return 0;
}

static int body_int(const req_ctx *q, int64_t key, int64_t *v)
{
    oz_cbor_r r;
    if (!body_find(q, key, &r))
        return 0;
    int m = oz_cbor_peek(&r);
    if (m != OZ_CBOR_UINT && m != OZ_CBOR_NINT)
        return 0;
    *v = oz_cbor_get_int(&r);
    return !r.err;
}

static const uint8_t *body_bstr(const req_ctx *q, int64_t key, size_t *n)
{
    oz_cbor_r r;
    if (!body_find(q, key, &r) || oz_cbor_peek(&r) != OZ_CBOR_BSTR)
        return NULL;
    return oz_cbor_get_bstr(&r, n);
}

/* §4.3: state-changing cred/bond requests carry the epoch they were built on. */
static int epoch_ok(req_ctx *q)
{
    int64_t e;
    if (body_int(q, 0, &e) && e == (int64_t)q->L->epoch)
        return 1;
    result(q, OZ_R_STALE_EPOCH, 1);
    oz_cbor_put_uint(&q->w, 1);
    oz_cbor_put_uint(&q->w, q->L->epoch);
    return 0;
}

static void ok_with_epoch(req_ctx *q, int r, int64_t key1_val, int has_key1)
{
    result(q, r, has_key1 ? 2 : 1);
    if (has_key1) {
        oz_cbor_put_uint(&q->w, 1);
        oz_cbor_put_int(&q->w, key1_val);
    }
    oz_cbor_put_uint(&q->w, 2);
    oz_cbor_put_uint(&q->w, q->L->epoch);
}

static void v_unlock(req_ctx *q, int unlock)
{
    if (q->L->hooks.actuate)
        q->L->hooks.actuate(q->L->hooks.app, unlock);
    q->L->lock_state = unlock ? OZ_LOCKSTATE_UNLOCKED : OZ_LOCKSTATE_LOCKED;
    result(q, unlock ? OZ_R_UNLOCK_OK : OZ_R_LOCK_OK, 0);
}

static void v_state(req_ctx *q)
{
    result(q, OZ_R_OK, 3);
    oz_cbor_put_uint(&q->w, 1); oz_cbor_put_uint(&q->w, q->L->lock_state);
    oz_cbor_put_uint(&q->w, 2); oz_cbor_put_uint(&q->w, 4);     /* door state: unknown, no sensor */
    oz_cbor_put_uint(&q->w, 3); oz_cbor_put_uint(&q->w, q->L->battery);
}

static void v_info(req_ctx *q)
{
    result(q, OZ_R_OK, 3);
    oz_cbor_put_uint(&q->w, 1); oz_cbor_put_tstr(&q->w, q->L->fw, strlen(q->L->fw));
    oz_cbor_put_uint(&q->w, 2); oz_cbor_put_tstr(&q->w, "ak1037-dev", 10);
    oz_cbor_put_uint(&q->w, 3); oz_cbor_put_array(&q->w, 2);
    oz_cbor_put_uint(&q->w, OZ_CRED_PIN); oz_cbor_put_uint(&q->w, OZ_CRED_RFID);
}

static void v_time_put(req_ctx *q)
{
    int64_t t;
    if (!body_int(q, 1, &t) || t <= 0) {
        result(q, OZ_R_BAD_REQUEST, 0);
        return;
    }
    /* CONTRACT.md: the clock only moves forward. */
    if (q->L->clock_set && t < oz_lock_utc(q->L)) {
        result(q, OZ_R_TIME_REJECTED, 0);
        return;
    }
    q->L->utc_at_set = t;
    q->L->mono_at_set = oz_port_monotonic_ms();
    q->L->clock_set = 1;
    result(q, OZ_R_TIME_OK, 0);
}

static void v_time_get(req_ctx *q)
{
    result(q, OZ_R_OK, 1);
    oz_cbor_put_uint(&q->w, 1);
    oz_cbor_put_int(&q->w, oz_lock_utc(q->L));
}

static void v_bond_list(req_ctx *q)
{
    size_t n = 0;
    for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++)
        n += q->L->bonds[i].used;
    result(q, OZ_R_OK, 2);
    oz_cbor_put_uint(&q->w, 1);
    oz_cbor_put_uint(&q->w, q->L->epoch);
    oz_cbor_put_uint(&q->w, 2);
    oz_cbor_put_array(&q->w, n);
    for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++) {
        const oz_bond *b = &q->L->bonds[i];
        if (!b->used)
            continue;
        oz_cbor_put_array(&q->w, 5);
        oz_cbor_put_uint(&q->w, (uint64_t)i);
        oz_cbor_put_uint(&q->w, b->role);
        oz_cbor_put_tstr(&q->w, b->label, strlen(b->label));
        oz_cbor_put_int(&q->w, b->expires);
        oz_cbor_put_bstr(&q->w, b->pub, 8);   /* key tag: enough to match, not to read */
    }
}

/* Admin enrols a service, bridge or issuer key (§6). */
static void v_bond_add(req_ctx *q)
{
    if (!epoch_ok(q))
        return;
    int64_t role = 0, max_days = 30;
    size_t pl = 0, ll = 0;
    const uint8_t *pub = body_bstr(q, 2, &pl);
    oz_cbor_r lr;
    const uint8_t *label = NULL;
    if (body_find(q, 3, &lr) && oz_cbor_peek(&lr) == OZ_CBOR_TSTR)
        label = oz_cbor_get_tstr(&lr, &ll);
    body_int(q, 4, &max_days);
    if (!body_int(q, 1, &role) || !pub || pl != 32 || ll > OZ_LOCK_LABEL ||
        (role != OZ_ROLE_SERVICE && role != OZ_ROLE_BRIDGE && role != OZ_ROLE_ISSUER) ||
        max_days < 1 || max_days > 90) {
        result(q, OZ_R_BAD_REQUEST, 0);
        return;
    }
    if (bond_by_pub(q->L, pub) >= 0) {
        result(q, OZ_R_DENIED, 0);
        return;
    }
    int s = free_slot(q->L);
    if (s < 0) {
        result(q, OZ_R_NO_SLOT, 0);
        return;
    }
    oz_bond *b = &q->L->bonds[s];
    memset(b, 0, sizeof *b);
    b->used = 1;
    b->role = (uint8_t)role;
    memcpy(b->pub, pub, 32);
    if (label)
        memcpy(b->label, label, ll);
    b->max_days = (uint16_t)max_days;
    save_bond(q->L, s);
    bump_epoch(q->L);
    ok_with_epoch(q, OZ_R_BOND_OK, s, 1);
}

static int parse_tail_int(const char *path, const char *prefix, long *v)
{
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl) || !path[pl])
        return 0;
    long n = 0;
    for (const char *p = path + pl; *p; p++) {
        if (*p < '0' || *p > '9' || n > 100000)
            return 0;
        n = n * 10 + (*p - '0');
    }
    *v = n;
    return 1;
}

static void v_bond_revoke(req_ctx *q, long target)
{
    /* CONTRACT.md M4 note 6: check order leaks nothing about other bonds. */
    if (target == 0) {
        result(q, OZ_R_REVOKE_DENIED, 0);
        return;
    }
    if (q->role != OZ_ROLE_ADMIN && target != q->slot) {
        result(q, OZ_R_REVOKE_DENIED, 0);
        return;
    }
    if (!epoch_ok(q))
        return;
    if (target >= OZ_LOCK_MAX_BONDS || !q->L->bonds[target].used) {
        result(q, OZ_R_NOT_FOUND, 0);
        return;
    }
    revoke(q->L, (int)target);
    bump_epoch(q->L);
    ok_with_epoch(q, OZ_R_REVOKE_OK, 0, 0);
}

static int unhex16(const char *s, uint8_t out[16])
{
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char c = s[2 * i + j];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    return s[32] ? -1 : 0;
}

static void v_invite_cancel(req_ctx *q, const char *hex)
{
    uint8_t nonce[16];
    if (strlen(hex) != 32 || unhex16(hex, nonce)) {
        result(q, OZ_R_BAD_REQUEST, 0);
        return;
    }
    const uint8_t *tag;
    static const uint8_t zero8[8];
    if (nonce_seen(q->L, nonce, &tag)) {
        /* Already redeemed: revoke the bond instead. Already cancelled: fine. */
        result(q, memcmp(tag, zero8, 8) ? OZ_R_REVOKE_DENIED : OZ_R_REVOKE_OK, 0);
        return;
    }
    nonce_burn(q->L, nonce, NULL);
    result(q, OZ_R_REVOKE_OK, 0);
}

static int cred_from_body(req_ctx *q, oz_cbor_r *r, oz_cred *c)
{
    /* [type, secret, from, to, uses] for cred/sync, or the POST body map. */
    size_t sl;
    if (r) {
        if (oz_cbor_get_array(r) != 5)
            return -1;
        c->type = (uint8_t)oz_cbor_get_uint(r);
        const uint8_t *s = oz_cbor_get_bstr(r, &sl);
        c->from = oz_cbor_get_int(r);
        c->to = oz_cbor_get_int(r);
        c->uses = (int32_t)oz_cbor_get_int(r);
        if (r->err || !s || !sl || sl > OZ_LOCK_CRED_SECRET)
            return -1;
        memcpy(c->secret, s, sl);
        c->secret_len = (uint8_t)sl;
    } else {
        int64_t type = 0, from = 0, to = 0, uses = -1;
        const uint8_t *s = body_bstr(q, 3, &sl);
        if (!body_int(q, 1, &type) || !s || !sl || sl > OZ_LOCK_CRED_SECRET)
            return -1;
        body_int(q, 4, &from);
        body_int(q, 5, &to);
        body_int(q, 6, &uses);
        c->type = (uint8_t)type;
        c->from = from;
        c->to = to;
        c->uses = (int32_t)uses;
        memcpy(c->secret, s, sl);
        c->secret_len = (uint8_t)sl;
    }
    if (c->type != OZ_CRED_PIN && c->type != OZ_CRED_RFID && c->type != OZ_CRED_FINGERPRINT)
        return -1;
    if (c->type == OZ_CRED_PIN)
        for (size_t i = 0; i < c->secret_len; i++)
            if (c->secret[i] < '0' || c->secret[i] > '9')
                return -1;
    return 0;
}

static void v_cred_add(req_ctx *q)
{
    if (!epoch_ok(q))
        return;
    oz_cred c = { 0 };
    if (cred_from_body(q, NULL, &c)) {
        result(q, OZ_R_BAD_REQUEST, 0);
        return;
    }
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++) {
        if (q->L->creds[i].used)
            continue;
        c.used = 1;
        c.id = q->L->next_cred_id++;
        q->L->creds[i] = c;
        save_creds(q->L);
        bump_epoch(q->L);
        ok_with_epoch(q, OZ_R_CRED_OK, c.id, 1);
        return;
    }
    result(q, OZ_R_NO_SLOT, 0);
}

static void v_cred_delete(req_ctx *q, long id)
{
    if (!epoch_ok(q))
        return;
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++) {
        if (q->L->creds[i].used && q->L->creds[i].id == id) {
            oz_wipe(&q->L->creds[i], sizeof q->L->creds[i]);
            save_creds(q->L);
            bump_epoch(q->L);
            ok_with_epoch(q, OZ_R_CRED_OK, 0, 0);
            return;
        }
    }
    result(q, OZ_R_CRED_NOT_FOUND, 0);
}

static void v_cred_list(req_ctx *q)
{
    size_t n = 0;
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++)
        n += q->L->creds[i].used;
    result(q, OZ_R_OK, 2);
    oz_cbor_put_uint(&q->w, 1);
    oz_cbor_put_uint(&q->w, q->L->epoch);
    oz_cbor_put_uint(&q->w, 2);
    oz_cbor_put_array(&q->w, n);
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++) {
        const oz_cred *c = &q->L->creds[i];
        if (!c->used)
            continue;
        oz_cbor_put_array(&q->w, 5);   /* no secrets in listings */
        oz_cbor_put_uint(&q->w, c->id);
        oz_cbor_put_uint(&q->w, c->type);
        oz_cbor_put_int(&q->w, c->from);
        oz_cbor_put_int(&q->w, c->to);
        oz_cbor_put_int(&q->w, c->uses);
    }
}

static void v_cred_sync(req_ctx *q)
{
    if (!epoch_ok(q))
        return;
    oz_cbor_r r;
    if (!body_find(q, 1, &r) || oz_cbor_peek(&r) != OZ_CBOR_ARRAY) {
        result(q, OZ_R_BAD_REQUEST, 0);
        return;
    }
    size_t n = oz_cbor_get_array(&r);
    if (n > OZ_LOCK_MAX_CREDS) {
        result(q, OZ_R_NO_SLOT, 0);
        return;
    }
    oz_cred fresh[OZ_LOCK_MAX_CREDS];
    memset(fresh, 0, sizeof fresh);
    for (size_t i = 0; i < n; i++) {
        if (cred_from_body(q, &r, &fresh[i])) {
            result(q, OZ_R_BAD_REQUEST, 0);
            return;
        }
        fresh[i].used = 1;
        fresh[i].id = (uint16_t)(q->L->next_cred_id + i);
    }
    q->L->next_cred_id = (uint16_t)(q->L->next_cred_id + n);
    oz_wipe(q->L->creds, sizeof q->L->creds);
    memcpy(q->L->creds, fresh, sizeof fresh);
    oz_wipe(fresh, sizeof fresh);
    save_creds(q->L);
    bump_epoch(q->L);
    ok_with_epoch(q, OZ_R_CRED_OK, (int64_t)n, 1);
}

static void factory_reset(void)
{
    char k[4];
    for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++) {
        kv_name(k, 'b', i);
        oz_port_kv_erase(k);
        kv_name(k, 'r', i);
        oz_port_kv_erase(k);
    }
    oz_port_kv_erase("ep");
    oz_port_kv_erase("nc");
    oz_port_kv_erase("cr");
    oz_port_kv_erase("lk");
}

/* Roles that may use each verb (§5, §6). Members get relay refused at EDHOC. */
#define R_ADMIN   (1u << OZ_ROLE_ADMIN)
#define R_MEMBER  (1u << OZ_ROLE_MEMBER)
#define R_SERVICE (1u << OZ_ROLE_SERVICE)
#define R_BRIDGE  (1u << OZ_ROLE_BRIDGE)

static int allowed(unsigned mask, int role) { return (mask >> role) & 1u; }

/* Is a valid, recent Echo attached (RFC 9175)? Consumes it. */
static int echo_ok(oz_link *k, const oz_coap_msg *req)
{
    const oz_coap_opt *o = oz_coap_find(req, OZ_COAP_OPT_ECHO);
    int ok = o && k->echo_valid && o->len == 8 && !oz_ct_equal(o->val, k->echo, 8) &&
             oz_port_monotonic_ms() - k->echo_ms <= ECHO_WINDOW_MS;
    if (ok)
        k->echo_valid = 0;
    return ok;
}

static int send_echo(req_ctx *q, uint8_t *out, size_t cap, size_t *out_len, int with_piv)
{
    oz_port_random(q->k->echo, 8);
    q->k->echo_ms = oz_port_monotonic_ms();
    q->k->echo_valid = 1;
    oz_coap_msg m;
    oz_coap_init(&m, q->req->type == OZ_COAP_CON ? OZ_COAP_ACK : OZ_COAP_NON,
                 OZ_COAP_UNAUTHORIZED, q->req->mid);
    m.tkl = q->req->tkl;
    memcpy(m.token, q->req->token, q->req->tkl);
    oz_coap_add_opt(&m, OZ_COAP_OPT_ECHO, q->k->echo, 8);
    return oz_oscore_protect_resp(q->ctx, q->x, &m, with_piv, out, cap, out_len);
}

static int dispatch(req_ctx *q, uint8_t *out, size_t cap, size_t *out_len)
{
    const char *p = q->path;
    uint8_t meth = q->req->code;
    int role = q->role;
    long id;
    q->code = OZ_COAP_CHANGED;

    if (!strcmp(p, "lock/unlock") || !strcmp(p, "lock/lock")) {
        int unlock = p[5] == 'u';
        unsigned mask = R_ADMIN | R_MEMBER;   /* service never actuates (§6) */
        if (meth != OZ_COAP_POST) { q->code = OZ_COAP_METHOD_NOT_ALLOWED; result(q, OZ_R_BAD_REQUEST, 0); }
        else if (!allowed(mask, role)) { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
        else if (q->k->relay && !echo_ok(q->k, q->req))
            return send_echo(q, out, cap, out_len, 0);                 /* §4.2 */
        else v_unlock(q, unlock);
    } else if (!strcmp(p, "lock/state")) {
        if (!allowed(R_ADMIN | R_MEMBER, role)) { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
        else { q->code = OZ_COAP_CONTENT; v_state(q); }
    } else if (!strcmp(p, "dev/info")) {
        if (!allowed(R_ADMIN | R_MEMBER, role)) { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
        else { q->code = OZ_COAP_CONTENT; v_info(q); }
    } else if (!strcmp(p, "time")) {
        if (meth == OZ_COAP_PUT && allowed(R_ADMIN | R_BRIDGE, role)) v_time_put(q);
        else if (meth == OZ_COAP_GET && allowed(R_ADMIN | R_BRIDGE | R_MEMBER, role)) { q->code = OZ_COAP_CONTENT; v_time_get(q); }
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (!strcmp(p, "bond")) {
        if (meth == OZ_COAP_GET && role == OZ_ROLE_ADMIN) { q->code = OZ_COAP_CONTENT; v_bond_list(q); }
        else if (meth == OZ_COAP_POST && role == OZ_ROLE_ADMIN) v_bond_add(q);
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (!strncmp(p, "bond/invite/", 12)) {
        if (meth == OZ_COAP_DELETE && role == OZ_ROLE_ADMIN) v_invite_cancel(q, p + 12);
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (parse_tail_int(p, "bond/", &id)) {
        if (meth == OZ_COAP_DELETE && allowed(R_ADMIN | R_MEMBER, role)) v_bond_revoke(q, id);
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (!strcmp(p, "cred")) {
        if (meth == OZ_COAP_POST && allowed(R_ADMIN | R_SERVICE, role)) v_cred_add(q);
        else if (meth == OZ_COAP_GET && role == OZ_ROLE_ADMIN) { q->code = OZ_COAP_CONTENT; v_cred_list(q); }
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (!strcmp(p, "cred/sync")) {
        if (meth == OZ_COAP_PUT && allowed(R_ADMIN | R_SERVICE, role)) v_cred_sync(q);
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (parse_tail_int(p, "cred/", &id)) {
        if (meth == OZ_COAP_DELETE && allowed(R_ADMIN | R_SERVICE, role)) v_cred_delete(q, id);
        else { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
    } else if (!strcmp(p, "dev/reset")) {
        if (meth != OZ_COAP_POST || role != OZ_ROLE_ADMIN) { q->code = OZ_COAP_FORBIDDEN; result(q, OZ_R_FORBIDDEN, 0); }
        else if (q->k->relay && !echo_ok(q->k, q->req))
            return send_echo(q, out, cap, out_len, 0);
        else {
            result(q, OZ_R_OK, 0);
            int rc = respond(q, 0, out, cap, out_len);
            /* oz_lock_init clears L, so keep what it needs first. */
            char dev[OZ_INVITE_MAX_DEVICE_ID + 1];
            const char *fw = q->L->fw;
            oz_lock_hooks hooks = q->L->hooks;
            strcpy(dev, q->L->device_id);
            factory_reset();
            oz_lock_init(q->L, dev, fw, &hooks);
            return rc;
        }
    } else {
        q->code = OZ_COAP_NOT_FOUND;
        result(q, OZ_R_UNSUPPORTED, 0);
    }
    if (q->w.err)
        return q->w.err;
    return respond(q, 0, out, cap, out_len);
}

static int rx_oscore(oz_lock *L, oz_link *k, const uint8_t *in, size_t len,
                     uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    static uint8_t pt[1100];
    oz_coap_msg req;
    oz_oscore_xchg x;
    req_ctx q;
    memset(&q, 0, sizeof q);
    q.L = L;
    q.k = k;
    *out_type = OZ_FRAME_OSCORE;
    *out_len = 0;

    if (k->relay) {
        const uint8_t *kid, *ic;
        size_t kl, il;
        if (oz_oscore_peek_kid(in, len, &kid, &kl, &ic, &il))
            return OZ_OK;
        q.slot = -1;
        for (int i = 0; i < OZ_LOCK_MAX_BONDS; i++)
            if (L->relay_used[i] && L->relay[i].recipient_id_len == kl &&
                !memcmp(L->relay[i].recipient_id, kid, kl))
                q.slot = i;
        if (q.slot < 0)
            return OZ_OK;                          /* unknown context: silent */
        q.ctx = &L->relay[q.slot];
    } else {
        if (!k->ctx_ready) {
            oz_port_log("oscore: no session context on this link");
            return OZ_OK;
        }
        q.ctx = &k->osc;
        q.slot = k->slot;
    }
    int vrc = oz_oscore_verify_req(q.ctx, in, len, pt, sizeof pt, &req, &x);
    if (vrc) {
        oz_port_log("oscore: request dropped (%d)", vrc);   /* replay / forgery: no answer */
        return OZ_OK;
    }
    q.req = &req;
    q.x = &x;
    oz_cbor_w_init(&q.w, q.res, sizeof q.res);
    oz_cbor_r_init(&q.body, req.payload, req.payload_len);
    if (oz_coap_path(&req, q.path, sizeof q.path))
        q.path[0] = 0;

    /* First pairing: one POST "pair" is held until the keypad answers (§3.4). */
    if (k->pairing) {
        if (!strcmp(q.path, "pair") && req.code == OZ_COAP_POST) {
            k->pair_x = x;
            k->pair_req = req;
            k->pair_req.n_opts = 0;
            k->pair_req.payload = NULL;
            k->pair_req.payload_len = 0;
            k->pair_req_pending = 1;
            return OZ_OK;
        }
        q.code = OZ_COAP_FORBIDDEN;
        result(&q, OZ_R_PAIR_PENDING, 0);
        return respond(&q, 0, out, cap, out_len);
    }

    if (!bond_live(L, q.slot)) {
        q.code = OZ_COAP_UNAUTHORIZED;
        result(&q, OZ_R_DENIED, 0);
        return respond(&q, 0, out, cap, out_len);
    }
    q.role = L->bonds[q.slot].role;

    /* A relay context restored after a reboot: Echo first (§4.1, B.1.2). */
    if (x.replay_unknown) {
        if (!echo_ok(k, &req))
            return send_echo(&q, out, cap, out_len, 1);
        oz_oscore_window_start(q.ctx, x.seq);
    }
    return dispatch(&q, out, cap, out_len);
}

int oz_lock_rx(oz_lock *L, oz_link *k, uint8_t type, const uint8_t *in, size_t len,
               uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    k->lock = L;
    switch (type) {
    case OZ_FRAME_EDHOC:
        return rx_edhoc(L, k, in, len, out_type, out, cap, out_len);
    case OZ_FRAME_OSCORE:
        return rx_oscore(L, k, in, len, out_type, out, cap, out_len);
    default:
        /* An EDHOC error from the peer ends any handshake in progress. */
        oz_edhoc_wipe(&k->edhoc);
        k->edhoc_active = 0;
        return OZ_OK;
    }
}

/* ── first pairing: the keypad side ─────────────────────────────────────── */

static int pair_finish(oz_lock *L, oz_link *k, int ok,
                       uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    *out_type = OZ_FRAME_OSCORE;
    if (L->hooks.pair_prompt)
        L->hooks.pair_prompt(L->hooks.app, 0);
    k->pairing = 0;
    if (ok) {
        oz_bond *b = &L->bonds[0];
        memset(b, 0, sizeof *b);
        b->used = 1;
        b->role = OZ_ROLE_ADMIN;
        memcpy(b->pub, k->pair_pub, 32);
        strcpy(b->label, "owner");
        save_bond(L, 0);
        bump_epoch(L);
        L->pair_failures = 0;
        k->slot = 0;
    } else if (++L->pair_failures >= PAIR_MAX_FAILURES) {
        L->pair_lockout_until_ms = oz_port_monotonic_ms() + PAIR_LOCKOUT_MS;
        L->pair_failures = 0;
    }
    int rc = OZ_OK;
    if (k->pair_req_pending) {
        req_ctx q;
        memset(&q, 0, sizeof q);
        q.L = L;
        q.k = k;
        q.ctx = &k->osc;
        q.req = &k->pair_req;
        q.x = &k->pair_x;
        q.code = ok ? OZ_COAP_CHANGED : OZ_COAP_UNAUTHORIZED;
        oz_cbor_w_init(&q.w, q.res, sizeof q.res);
        if (ok) {
            result(&q, OZ_R_BOND_OK, 1);
            oz_cbor_put_uint(&q.w, 1);
            oz_cbor_put_uint(&q.w, 0);
        } else {
            result(&q, OZ_R_PAIR_FAILED, 0);
        }
        rc = respond(&q, 0, out, cap, out_len);
        k->pair_req_pending = 0;
    }
    if (!ok) {
        /* Nothing stored; the app must start a new handshake (new code). */
        oz_oscore_wipe(&k->osc);
        k->ctx_ready = 0;
    }
    return rc;
}

int oz_lock_pair_code(oz_lock *L, oz_link *k, uint32_t code,
                      uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (!k->pairing)
        return OZ_E_ARG;
    if (oz_port_monotonic_ms() > k->pair_deadline_ms)
        return pair_finish(L, k, 0, out_type, out, cap, out_len);
    /* Constant-time compare of the two codes. */
    uint8_t a[4] = { (uint8_t)(code >> 24), (uint8_t)(code >> 16), (uint8_t)(code >> 8), (uint8_t)code };
    uint8_t b[4] = { (uint8_t)(k->pair_code >> 24), (uint8_t)(k->pair_code >> 16),
                     (uint8_t)(k->pair_code >> 8), (uint8_t)k->pair_code };
    return pair_finish(L, k, !oz_ct_equal(a, b, 4), out_type, out, cap, out_len);
}

int oz_lock_tick(oz_lock *L, oz_link *k, uint8_t *out_type, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (k->pairing && oz_port_monotonic_ms() > k->pair_deadline_ms)
        return pair_finish(L, k, 0, out_type, out, cap, out_len);
    return OZ_OK;
}

/* ── keypad PIN at the door ─────────────────────────────────────────────── */

int oz_lock_keypad_pin(oz_lock *L, const char *digits)
{
    size_t n = strlen(digits);
    int64_t now = oz_lock_utc(L);
    for (int i = 0; i < OZ_LOCK_MAX_CREDS; i++) {
        oz_cred *c = &L->creds[i];
        if (!c->used || c->type != OZ_CRED_PIN || c->secret_len != n ||
            oz_ct_equal(c->secret, (const uint8_t *)digits, n))
            continue;
        /* Time windows need a set clock; without one a timed PIN is refused. */
        if ((c->from || c->to) && !now)
            return 0;
        if ((c->from && now < c->from) || (c->to && now > c->to) || c->uses == 0)
            return 0;
        if (c->uses > 0) {
            c->uses--;
            save_creds(L);
        }
        if (L->hooks.actuate)
            L->hooks.actuate(L->hooks.app, 1);
        L->lock_state = OZ_LOCKSTATE_UNLOCKED;
        return 1;
    }
    return 0;
}
