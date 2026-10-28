/* Lock engine end to end: a simulated app (EDHOC initiator + OSCORE client)
 * against oz_lock, in process. Each block names the ozkey-48 rule it checks. */
#include "oz_lock.h"
#include "oz_cbor.h"
#include "oz_port_host.h"
#include "oz_test.h"

/* ── the lock and its hardware hooks ────────────────────────────────────── */

static oz_lock L;
static int unlocks, locks, prompt_on;
static void hw_actuate(void *app, int unlock) { (void)app; if (unlock) unlocks++; else locks++; }
static void hw_prompt(void *app, int on) { (void)app; prompt_on = on; }
static const oz_lock_hooks HOOKS = { hw_actuate, hw_prompt, NULL };

static void boot_lock(void)
{
    oz_lock_init(&L, "ozk-test0001", "ozcore-p2", &HOOKS);
}

/* ── a simulated app ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t priv[32], pub[32], ccs[OZ_CCS_LEN], kccs[64];
    size_t kccs_len;
    uint8_t idc_slot[4];
    int slot;                  /* bond slot once known, −1 before */
    oz_edhoc e;
    oz_oscore_ctx osc;
    uint16_t mid;
    /* last response */
    uint8_t code;
    int result;
    uint8_t pt[512];
    oz_coap_msg resp;
    uint8_t echo[8];
    int has_echo;
} app_t;

static app_t owner, member, stranger, issuer, guest, service;
static oz_link ble, relay;
static uint8_t lock_ccs_view[OZ_CCS_LEN];

static void app_new(app_t *a)
{
    memset(a, 0, sizeof *a);
    oz_port_random(a->priv, 32);
    oz_port_x25519_public_key(a->pub, a->priv);
    oz_ccs_for(a->pub, a->ccs);
    oz_cbor_w w;
    oz_cbor_w_init(&w, a->kccs, sizeof a->kccs);
    oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 14);
    oz_cbor_put_raw(&w, a->ccs, OZ_CCS_LEN);
    a->kccs_len = w.len;
    a->slot = -1;
}

/* The app knows the lock from `info`: its key, hence its canonical CCS. */
static int app_lock_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    (void)app;
    static const uint8_t lock_idc[4] = { 0xa1, 0x04, 0x41, 0x00 };
    if (len != 4 || memcmp(id, lock_idc, 4))
        return OZ_E_NOT_FOUND;
    oz_ccs_for(L.pub, lock_ccs_view);
    out->cred = lock_ccs_view;
    out->cred_len = OZ_CCS_LEN;
    memcpy(out->pub, L.pub, 32);
    return OZ_OK;
}

/* Run EDHOC on link k. Returns 0 when the lock accepted message_3 silently,
 * else 1 (the lock answered with an EDHOC error; its text in `why`). */
static int handshake(app_t *a, oz_link *k, const uint8_t *ead3, size_t ead3_len, char *why)
{
    uint8_t m1[128], m2[256], m3[512], lo[256];
    size_t m1l, m2l, m3l, lol;
    uint8_t lt;
    oz_edhoc_init(&a->e, 1);
    a->e.c_own.len = 1;
    a->e.c_own.b[0] = 0x05;
    a->e.own.cred = a->ccs;
    a->e.own.cred_len = OZ_CCS_LEN;
    if (a->slot >= 0) {
        a->idc_slot[0] = 0xa1; a->idc_slot[1] = 0x04; a->idc_slot[2] = 0x41;
        a->idc_slot[3] = (uint8_t)a->slot;
        a->e.own.id_cred = a->idc_slot;
        a->e.own.id_cred_len = 4;
    } else {
        a->e.own.id_cred = a->kccs;
        a->e.own.id_cred_len = a->kccs_len;
    }
    memcpy(a->e.own_priv, a->priv, 32);
    a->e.get_cred = app_lock_cred;
    if (why) why[0] = 0;

    oz_edhoc_i_msg1(&a->e, NULL, 0, m1, sizeof m1, &m1l);
    oz_lock_rx(&L, k, OZ_FRAME_EDHOC, m1, m1l, &lt, m2, sizeof m2, &m2l);
    if (oz_edhoc_i_msg2(&a->e, m2, m2l, lo, sizeof lo, &lol)) {
        if (why) snprintf(why, 32, "msg2 refused");
        return 1;
    }
    oz_edhoc_i_msg3(&a->e, ead3, ead3_len, m3, sizeof m3, &m3l);
    oz_lock_rx(&L, k, OZ_FRAME_EDHOC, m3, m3l, &lt, lo, sizeof lo, &lol);
    if (lol) {
        /* EDHOC error: (1, "TEXT") */
        oz_cbor_r r;
        oz_cbor_r_init(&r, lo, lol);
        (void)oz_cbor_get_int(&r);
        size_t tl;
        const uint8_t *t = oz_cbor_get_tstr(&r, &tl);
        if (why && t) { memcpy(why, t, tl < 31 ? tl : 31); why[tl < 31 ? tl : 31] = 0; }
        return 1;
    }
    uint8_t sec[16], salt[8];
    oz_edhoc_cid sid, rid;
    oz_edhoc_oscore(&a->e, sec, salt, &sid, &rid);
    oz_oscore_derive(&a->osc, sec, 16, salt, 8, sid.b, sid.len, rid.b, rid.len, NULL, 0);
    return 0;
}

static oz_oscore_xchg held_x;

/* Build an OSCORE request; returns its length in out. */
static size_t make_req(app_t *a, uint8_t method, const char *path, const uint8_t *body, size_t bl,
                       const uint8_t *echo, uint8_t *out, size_t cap, oz_oscore_xchg *x)
{
    oz_coap_msg m;
    oz_coap_init(&m, OZ_COAP_CON, method, ++a->mid);
    m.tkl = 1;
    m.token[0] = (uint8_t)a->mid;
    oz_coap_set_path(&m, path);
    if (echo)
        oz_coap_add_opt(&m, OZ_COAP_OPT_ECHO, echo, 8);
    m.payload = body;
    m.payload_len = bl;
    size_t n = 0;
    oz_oscore_protect_req(&a->osc, &m, out, cap, &n, x);
    return n;
}

/* Deliver a request frame and read the answer. Returns 0 on a response. */
static int deliver(app_t *a, oz_link *k, const uint8_t *req, size_t rl, oz_oscore_xchg *x)
{
    uint8_t out[600];
    size_t ol;
    uint8_t lt;
    a->code = 0;
    a->result = -1;
    a->has_echo = 0;
    oz_lock_rx(&L, k, OZ_FRAME_OSCORE, req, rl, &lt, out, sizeof out, &ol);
    if (!ol)
        return -1;
    int64_t piv;
    if (oz_oscore_verify_resp(&a->osc, x, out, ol, a->pt, sizeof a->pt, &a->resp, &piv))
        return -2;
    a->code = a->resp.code;
    const oz_coap_opt *e = oz_coap_find(&a->resp, OZ_COAP_OPT_ECHO);
    if (e && e->len == 8) {
        memcpy(a->echo, e->val, 8);
        a->has_echo = 1;
    }
    if (a->resp.payload_len) {
        oz_cbor_r r;
        oz_cbor_r_init(&r, a->resp.payload, a->resp.payload_len);
        if (oz_cbor_get_map(&r) && oz_cbor_get_uint(&r) == 0)
            a->result = (int)oz_cbor_get_uint(&r);
    }
    return 0;
}

static int call(app_t *a, oz_link *k, uint8_t method, const char *path, const uint8_t *body, size_t bl)
{
    uint8_t req[600];
    oz_oscore_xchg x;
    size_t n = make_req(a, method, path, body, bl, NULL, req, sizeof req, &x);
    if (deliver(a, k, req, n, &x))
        return -1;
    if (a->has_echo) {   /* 4.01 + Echo: repeat once with it (§4.2) */
        uint8_t echo[8];
        memcpy(echo, a->echo, 8);
        n = make_req(a, method, path, body, bl, echo, req, sizeof req, &x);
        if (deliver(a, k, req, n, &x))
            return -1;
    }
    return a->result;
}

/* Response map value for key (uint), or −1. */
static int64_t resp_int(const app_t *a, uint64_t key)
{
    oz_cbor_r r;
    oz_cbor_r_init(&r, a->resp.payload, a->resp.payload_len);
    size_t pairs = oz_cbor_get_map(&r);
    for (size_t i = 0; i < pairs && !r.err; i++) {
        uint64_t k = oz_cbor_get_uint(&r);
        int m = oz_cbor_peek(&r);
        if (k == key && (m == OZ_CBOR_UINT || m == OZ_CBOR_NINT))
            return oz_cbor_get_int(&r);
        size_t n;
        (void)oz_cbor_skip(&r, &n);
    }
    return -1;
}

static size_t body_epoch(uint8_t *b, size_t cap, size_t extra_pairs)
{
    oz_cbor_w w;
    oz_cbor_w_init(&w, b, cap);
    oz_cbor_put_map(&w, 1 + extra_pairs);
    oz_cbor_put_uint(&w, 0);
    oz_cbor_put_uint(&w, L.epoch);
    return w.len;
}

/* ── scenarios ──────────────────────────────────────────────────────────── */

static void first_pairing(void)
{
    uint8_t out[300];
    size_t ol;
    uint8_t lt;
    expect_int("fresh lock is unclaimed", oz_lock_claimed(&L), 0);

    /* Wrong code: nothing stored, failure counted. */
    oz_link_open(&ble, 0);
    expect_int("pairing handshake", handshake(&owner, &ble, NULL, 0, NULL), 0);
    expect_int("keypad prompt on", prompt_on, 1);
    uint8_t req[200];
    oz_oscore_xchg x;
    size_t n = make_req(&owner, OZ_COAP_POST, "pair", NULL, 0, NULL, req, sizeof req, &x);
    oz_lock_rx(&L, &ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    expect_int("pair request is held", (long long)ol, 0);
    uint32_t code;
    oz_edhoc_pair_code(&owner.e, &code);
    oz_lock_pair_code(&L, &ble, (code + 1) % 1000000u, &lt, out, sizeof out, &ol);
    int64_t piv;
    expect_int("wrong code answered", ol > 0 &&
               !oz_oscore_verify_resp(&owner.osc, &x, out, ol, owner.pt, sizeof owner.pt, &owner.resp, &piv), 1);
    expect_int("wrong code → 4.01", owner.resp.code, OZ_COAP_UNAUTHORIZED);
    expect_int("still unclaimed", oz_lock_claimed(&L), 0);
    expect_int("prompt off", prompt_on, 0);

    /* Right code. */
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    n = make_req(&owner, OZ_COAP_POST, "pair", NULL, 0, NULL, req, sizeof req, &x);
    oz_lock_rx(&L, &ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    oz_edhoc_pair_code(&owner.e, &code);
    oz_lock_pair_code(&L, &ble, code, &lt, out, sizeof out, &ol);
    oz_oscore_verify_resp(&owner.osc, &x, out, ol, owner.pt, sizeof owner.pt, &owner.resp, &piv);
    expect_int("right code → 2.04", owner.resp.code, OZ_COAP_CHANGED);
    expect_int("claimed", oz_lock_claimed(&L), 1);
    owner.slot = 0;
    expect_int("owner unlocks in the same session", call(&owner, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0),
               OZ_R_UNLOCK_OK);
    expect_int("actuator ran", unlocks, 1);

    /* A second key can't claim a claimed lock (ownership-theft rule). */
    oz_link_open(&ble, 0);
    char why[32];
    expect_int("stranger refused", handshake(&stranger, &ble, NULL, 0, why), 1);
    expect("stranger reason", (const uint8_t *)why, strlen(why), "424f4e445f44454e494544");   /* BOND_DENIED */
}

static void pair_lockout(void)
{
    /* Separate fresh lock: five wrong codes close the window. */
    oz_host_kv_reset();
    boot_lock();
    app_t a;
    app_new(&a);
    uint8_t out[300];
    size_t ol;
    uint8_t lt;
    for (int i = 0; i < 5; i++) {
        oz_link_open(&ble, 0);
        handshake(&a, &ble, NULL, 0, NULL);
        oz_lock_pair_code(&L, &ble, 1000000u, &lt, out, sizeof out, &ol);   /* never valid */
    }
    oz_link_open(&ble, 0);
    char why[32];
    expect_int("locked out after 5 failures", handshake(&a, &ble, NULL, 0, why), 1);
    expect("lockout reason", (const uint8_t *)why, strlen(why), "504149525f4c4f434b45445f4f5554");
}

static void claim(app_t *a)
{
    uint8_t out[300], req[200];
    size_t ol;
    uint8_t lt;
    oz_oscore_xchg x;
    oz_link_open(&ble, 0);
    handshake(a, &ble, NULL, 0, NULL);
    size_t n = make_req(a, OZ_COAP_POST, "pair", NULL, 0, NULL, req, sizeof req, &x);
    oz_lock_rx(&L, &ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    uint32_t code;
    oz_edhoc_pair_code(&a->e, &code);
    oz_lock_pair_code(&L, &ble, code, &lt, out, sizeof out, &ol);
    a->slot = 0;
}

static size_t mint_invite(const app_t *iss_app, int iss_slot, uint8_t version, const app_t *for_app,
                          int64_t me, uint8_t nonce_byte, uint8_t *ead, size_t cap)
{
    oz_invite inv;
    memset(&inv, 0, sizeof inv);
    inv.version = version;
    strcpy(inv.device_id, L.device_id);
    memcpy(inv.issuer, iss_app->pub, 32);
    inv.role = OZ_INVITE_ROLE_MEMBER;
    strcpy(inv.label, "Ba Ngoai");
    memset(inv.nonce, nonce_byte, 16);
    inv.expires = oz_lock_utc(&L) ? oz_lock_utc(&L) + 600 : 1789000000;
    inv.membership_expires = me;
    if (version >= 3) {
        oz_invite_bind_of(for_app->pub, inv.bind);
        inv.iss = (uint32_t)iss_slot;
    }
    uint8_t secret[32];
    oz_port_x25519(secret, iss_app->priv, L.pub);   /* the issuer's pairing secret */
    oz_invite_compute_mac(&inv, secret, inv.mac);
    uint8_t cb[200];
    size_t cl;
    oz_invite_to_cbor(&inv, cb, sizeof cb, &cl);
    oz_cbor_w w;
    oz_cbor_w_init(&w, ead, cap);
    oz_cbor_put_int(&w, -OZ_EAD_INVITE);
    oz_cbor_put_bstr(&w, cb, cl);
    return w.len;
}

static void members(void)
{
    uint8_t ead[256];
    size_t el = mint_invite(&owner, 0, 2, NULL, 0, 0x42, ead, sizeof ead);
    oz_link_open(&ble, 0);
    expect_int("member redeems invite", handshake(&member, &ble, ead, el, NULL), 0);
    expect_int("member unlocks at the door", call(&member, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0),
               OZ_R_UNLOCK_OK);
    expect_int("member can't list bonds", call(&member, &ble, OZ_COAP_GET, "bond", NULL, 0), OZ_R_FORBIDDEN);

    oz_link_open(&ble, 0);
    char why[32];
    expect_int("same invite, other key", handshake(&stranger, &ble, ead, el, why), 1);
    expect("replay reason", (const uint8_t *)why, strlen(why), "4d454d4245525f5245504c4159");   /* MEMBER_REPLAY */

    /* Members are on premises only (§4.1): no handshake over the relay. */
    member.slot = 1;
    oz_link_open(&relay, 1);
    expect_int("member refused over relay", handshake(&member, &relay, NULL, 0, why), 1);

    /* A tampered invite (label changed after signing) is refused. */
    el = mint_invite(&owner, 0, 2, NULL, 0, 0x43, ead, sizeof ead);
    for (size_t i = 0; i + 8 <= el; i++)
        if (!memcmp(ead + i, "Ba Ngoai", 8)) { ead[i] = 'X'; break; }
    oz_link_open(&ble, 0);
    expect_int("tampered invite refused", handshake(&stranger, &ble, ead, el, NULL), 1);

    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    expect_int("owner lists bonds", call(&owner, &ble, OZ_COAP_GET, "bond", NULL, 0), OZ_R_OK);
}

static void credentials(void)
{
    uint8_t b[96];
    oz_cbor_w w;
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);

    /* POST cred {0: epoch, 1: PIN, 3: "135790"} */
    size_t bl = body_epoch(b, sizeof b, 2);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)"135790", 6);
    bl += w.len;
    expect_int("add PIN", call(&owner, &ble, OZ_COAP_POST, "cred", b, bl), OZ_R_CRED_OK);
    int64_t id = resp_int(&owner, 1);
    expect_int("PIN opens the door", oz_lock_keypad_pin(&L, "135790"), 1);
    expect_int("wrong PIN doesn't", oz_lock_keypad_pin(&L, "135791"), 0);
    expect_int("same body again: stale epoch", call(&owner, &ble, OZ_COAP_POST, "cred", b, bl), OZ_R_STALE_EPOCH);

    /* §4.3 held-write attack: seal an add, hold it, delete, then deliver. */
    uint8_t add[96];
    size_t al = body_epoch(add, sizeof add, 2);
    oz_cbor_w_init(&w, add + al, sizeof add - al);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)"246802", 6);
    al += w.len;
    uint8_t held[300];
    oz_oscore_xchg hx;
    size_t hl = make_req(&owner, OZ_COAP_POST, "cred", add, al, NULL, held, sizeof held, &hx);
    char path[16];
    snprintf(path, sizeof path, "cred/%lld", (long long)id);
    bl = body_epoch(b, sizeof b, 0);
    expect_int("delete PIN", call(&owner, &ble, OZ_COAP_DELETE, path, b, bl), OZ_R_CRED_OK);
    expect_int("deleted PIN refused", oz_lock_keypad_pin(&L, "135790"), 0);
    deliver(&owner, &ble, held, hl, &hx);
    expect_int("held write is stale", owner.result, OZ_R_STALE_EPOCH);
    expect_int("held PIN never became live", oz_lock_keypad_pin(&L, "246802"), 0);

    /* Limited uses. */
    bl = body_epoch(b, sizeof b, 3);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)"111222", 6);
    oz_cbor_put_uint(&w, 6); oz_cbor_put_uint(&w, 1);
    bl += w.len;
    call(&owner, &ble, OZ_COAP_POST, "cred", b, bl);
    expect_int("one-use PIN works once", oz_lock_keypad_pin(&L, "111222"), 1);
    expect_int("…and only once", oz_lock_keypad_pin(&L, "111222"), 0);
}

static void relay_and_reboot(void)
{
    /* The owner sets up a relay context (persisted per bond). */
    oz_link_open(&relay, 1);
    expect_int("owner handshake over relay", handshake(&owner, &relay, NULL, 0, NULL), 0);
    int before = unlocks;
    uint8_t req[200];
    oz_oscore_xchg x;
    size_t n = make_req(&owner, OZ_COAP_POST, "lock/unlock", NULL, 0, NULL, req, sizeof req, &x);
    deliver(&owner, &relay, req, n, &x);
    expect_int("relay unlock → 4.01", owner.code, OZ_COAP_UNAUTHORIZED);
    expect_int("…with Echo", owner.has_echo, 1);
    expect_int("not actuated yet", unlocks, before);
    uint8_t echo[8];
    memcpy(echo, owner.echo, 8);
    n = make_req(&owner, OZ_COAP_POST, "lock/unlock", NULL, 0, echo, req, sizeof req, &x);
    deliver(&owner, &relay, req, n, &x);
    expect_int("repeat with Echo unlocks", owner.result, OZ_R_UNLOCK_OK);
    expect_int("actuated once", unlocks, before + 1);
    deliver(&owner, &relay, req, n, &x);
    expect_int("replayed unlock is dropped", owner.code, 0);
    expect_int("still actuated once", unlocks, before + 1);
    expect_int("reads need no Echo", call(&owner, &relay, OZ_COAP_GET, "lock/state", NULL, 0), OZ_R_OK);

    /* Reboot: state comes back from kv; the relay window is unknown. */
    uint8_t keep_pub[32];
    memcpy(keep_pub, L.pub, 32);
    boot_lock();
    expect_int("identity survives reboot", memcmp(L.pub, keep_pub, 32), 0);
    expect_int("still claimed", oz_lock_claimed(&L), 1);
    oz_link_open(&relay, 1);
    n = make_req(&owner, OZ_COAP_GET, "lock/state", NULL, 0, NULL, req, sizeof req, &x);
    deliver(&owner, &relay, req, n, &x);
    expect_int("first request after reboot → Echo", owner.has_echo, 1);
    expect_int("call with Echo works", call(&owner, &relay, OZ_COAP_GET, "lock/state", NULL, 0), OZ_R_OK);
}

static void roles(void)
{
    uint8_t b[96];
    oz_cbor_w w;
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);

    /* Set the clock (needed for issuer limits). */
    oz_cbor_w_init(&w, b, sizeof b);
    oz_cbor_put_map(&w, 1); oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, 1790000000);
    expect_int("owner sets time", call(&owner, &ble, OZ_COAP_PUT, "time", b, w.len), OZ_R_TIME_OK);
    oz_cbor_w_init(&w, b, sizeof b);
    oz_cbor_put_map(&w, 1); oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, 1700000000);
    expect_int("clock never goes back", call(&owner, &ble, OZ_COAP_PUT, "time", b, w.len), OZ_R_TIME_REJECTED);

    /* Enrol a service key and an issuer key (max 3 days). */
    size_t bl = body_epoch(b, sizeof b, 3);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_ROLE_SERVICE);
    oz_cbor_put_uint(&w, 2); oz_cbor_put_bstr(&w, service.pub, 32);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_tstr(&w, "ozlodge", 7);
    bl += w.len;
    expect_int("enrol service", call(&owner, &ble, OZ_COAP_POST, "bond", b, bl), OZ_R_BOND_OK);
    service.slot = (int)resp_int(&owner, 1);
    bl = body_epoch(b, sizeof b, 3);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_ROLE_ISSUER);
    oz_cbor_put_uint(&w, 2); oz_cbor_put_bstr(&w, issuer.pub, 32);
    oz_cbor_put_uint(&w, 4); oz_cbor_put_uint(&w, 3);
    bl += w.len;
    expect_int("enrol issuer", call(&owner, &ble, OZ_COAP_POST, "bond", b, bl), OZ_R_BOND_OK);
    issuer.slot = (int)resp_int(&owner, 1);

    /* Service: credentials yes, actuation never (§6). */
    oz_link_open(&relay, 1);
    expect_int("service over relay", handshake(&service, &relay, NULL, 0, NULL), 0);
    expect_int("service can't unlock", call(&service, &relay, OZ_COAP_POST, "lock/unlock", NULL, 0), OZ_R_FORBIDDEN);
    bl = body_epoch(b, sizeof b, 2);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)"555000", 6);
    bl += w.len;
    expect_int("service adds a PIN", call(&service, &relay, OZ_COAP_POST, "cred", b, bl), OZ_R_CRED_OK);

    /* Issuer: bound guest invites only. */
    oz_link_open(&ble, 0);
    expect_int("issuer session", handshake(&issuer, &ble, NULL, 0, NULL), 0);
    expect_int("issuer can't unlock", call(&issuer, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0), OZ_R_FORBIDDEN);

    uint8_t ead[256];
    char why[32];
    size_t el = mint_invite(&issuer, issuer.slot, 3, &guest, 1790000000 + 2 * 86400, 0x51, ead, sizeof ead);
    oz_link_open(&ble, 0);
    expect_int("bound invite, wrong key", handshake(&stranger, &ble, ead, el, why), 1);
    expect("bound reason", (const uint8_t *)why, strlen(why), "494e564954455f424f554e445f454c53455748455245");
    oz_link_open(&ble, 0);
    expect_int("bound invite, right key", handshake(&guest, &ble, ead, el, NULL), 0);
    expect_int("guest unlocks", call(&guest, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0), OZ_R_UNLOCK_OK);

    el = mint_invite(&issuer, issuer.slot, 3, &stranger, 1790000000 + 30 * 86400, 0x52, ead, sizeof ead);
    oz_link_open(&ble, 0);
    expect_int("issuer window over max_days refused", handshake(&stranger, &ble, ead, el, NULL), 1);
    el = mint_invite(&issuer, issuer.slot, 2, &stranger, 1790000000 + 86400, 0x53, ead, sizeof ead);
    oz_link_open(&ble, 0);
    expect_int("issuer unbound (v2) invite refused", handshake(&stranger, &ble, ead, el, NULL), 1);

    /* Revoking the issuer voids its unredeemed invites. */
    el = mint_invite(&issuer, issuer.slot, 3, &stranger, 1790000000 + 86400, 0x54, ead, sizeof ead);
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    char path[16];
    snprintf(path, sizeof path, "bond/%d", issuer.slot);
    bl = body_epoch(b, sizeof b, 0);
    expect_int("revoke issuer", call(&owner, &ble, OZ_COAP_DELETE, path, b, bl), OZ_R_REVOKE_OK);
    oz_link_open(&ble, 0);
    expect_int("invite from revoked issuer refused", handshake(&stranger, &ble, ead, el, NULL), 1);

    /* Bond #0 can't be revoked; a member can revoke only itself. */
    bl = body_epoch(b, sizeof b, 0);
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    expect_int("bond 0 not revocable", call(&owner, &ble, OZ_COAP_DELETE, "bond/0", b, bl), OZ_R_REVOKE_DENIED);
    oz_link_open(&ble, 0);
    handshake(&member, &ble, NULL, 0, NULL);
    snprintf(path, sizeof path, "bond/%d", service.slot);
    expect_int("member can't revoke others", call(&member, &ble, OZ_COAP_DELETE, path, b, bl), OZ_R_REVOKE_DENIED);
    snprintf(path, sizeof path, "bond/%d", member.slot);
    bl = body_epoch(b, sizeof b, 0);
    expect_int("member revokes itself", call(&member, &ble, OZ_COAP_DELETE, path, b, bl), OZ_R_REVOKE_OK);
    expect_int("revoked session is dead", call(&member, &ble, OZ_COAP_GET, "lock/state", NULL, 0), OZ_R_DENIED);
    oz_link_open(&ble, 0);
    expect_int("revoked member can't reconnect", handshake(&member, &ble, NULL, 0, NULL), 1);
}

int main(void)
{
    oz_host_kv_reset();
    boot_lock();
    app_new(&owner); app_new(&member); app_new(&stranger);
    app_new(&issuer); app_new(&guest); app_new(&service);

    first_pairing();
    members();
    credentials();
    relay_and_reboot();
    roles();
    pair_lockout();
    return report("lock");
}
