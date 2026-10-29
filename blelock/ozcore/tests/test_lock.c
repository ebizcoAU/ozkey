/* Lock engine end to end: a simulated app (EDHOC initiator + OSCORE client)
 * against oz_lock, in process. Each block names the ozkey-48 rule it checks. */
#include "oz_lock.h"
#include "oz_cbor.h"
#include "oz_port_host.h"
#include "oz_test.h"
#include "oz_ble.h"

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

/* ── the GATT path (oz_ble), ozkey-48 §2.1 ─────────────────────────────────
 * framed_mtu 0 calls the engine directly. Otherwise every message on the BLE
 * link is fragmented at that MTU, written frame by frame into oz_ble, and the
 * reply comes back through its tx as notifications, reassembled here. The
 * relay link is never framed (MQTT carries whole messages). */
static size_t framed_mtu;
static oz_ble gatt;
static oz_reasm got;
static int got_done, got_bad;
static long frames_in, frames_out;

static int capture(void *ctx, const uint8_t *f, size_t n)
{
    int d;
    (void)ctx;
    frames_out++;
    if (n > framed_mtu - 3 || oz_reasm_feed(&got, f, n, &d)) {
        got_bad++;
        return OZ_E_ARG;
    }
    got_done |= d;
    return OZ_OK;
}

static void take(uint8_t *lt, uint8_t *out, size_t cap, size_t *ol)
{
    *ol = 0;
    if (got_done && got.len <= cap) {
        memcpy(out, got.buf, got.len);
        *ol = got.len;
        *lt = got.type;
    }
    got_done = 0;
    oz_reasm_reset(&got);
}

static int lock_rx(oz_link *k, uint8_t type, const uint8_t *in, size_t len,
                   uint8_t *lt, uint8_t *out, size_t cap, size_t *ol)
{
    if (!framed_mtu || k != &ble)
        return oz_lock_rx(&L, k, type, in, len, lt, out, cap, ol);
    unsigned count;
    int rc = oz_frag_count(len, framed_mtu, &count);
    for (unsigned i = 0; !rc && i < count; i++) {
        uint8_t f[600];
        size_t n;
        rc = oz_frag(type, in, len, framed_mtu, i, f, sizeof f, &n);
        frames_in++;
        if (!rc)
            rc = oz_ble_rx(&gatt, f, n);
    }
    take(lt, out, cap, ol);
    return rc;
}

static int lock_pair_code(oz_link *k, uint32_t code, uint8_t *lt, uint8_t *out, size_t cap, size_t *ol)
{
    if (!framed_mtu || k != &ble)
        return oz_lock_pair_code(&L, k, code, lt, out, cap, ol);
    int rc = oz_ble_pair_code(&gatt, code);
    take(lt, out, cap, ol);
    return rc;
}
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
    lock_rx(k, OZ_FRAME_EDHOC, m1, m1l, &lt, m2, sizeof m2, &m2l);
    if (oz_edhoc_i_msg2(&a->e, m2, m2l, lo, sizeof lo, &lol)) {
        if (why) snprintf(why, 32, "msg2 refused");
        return 1;
    }
    oz_edhoc_i_msg3(&a->e, ead3, ead3_len, m3, sizeof m3, &m3l);
    lock_rx(k, OZ_FRAME_EDHOC, m3, m3l, &lt, lo, sizeof lo, &lol);
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
    lock_rx(k, OZ_FRAME_OSCORE, req, rl, &lt, out, sizeof out, &ol);
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
    lock_rx(&ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    expect_int("pair request is held", (long long)ol, 0);
    uint32_t code;
    oz_edhoc_pair_code(&owner.e, &code);
    lock_pair_code(&ble, (code + 1) % 1000000u, &lt, out, sizeof out, &ol);
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
    lock_rx(&ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    oz_edhoc_pair_code(&owner.e, &code);
    lock_pair_code(&ble, code, &lt, out, sizeof out, &ol);
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
        lock_pair_code(&ble, 1000000u, &lt, out, sizeof out, &ol);   /* never valid */
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
    lock_rx(&ble, OZ_FRAME_OSCORE, req, n, &lt, out, sizeof out, &ol);
    uint32_t code;
    oz_edhoc_pair_code(&a->e, &code);
    lock_pair_code(&ble, code, &lt, out, sizeof out, &ol);
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

/* Keypad PINs (P5-B): anti-peep, the wrong-PIN limit, its persistence. */
static size_t body_pin(uint8_t *b, size_t cap, const char *pin)
{
    oz_cbor_w w;
    size_t bl = body_epoch(b, cap, 2);
    oz_cbor_w_init(&w, b + bl, cap - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)pin, strlen(pin));
    return bl + w.len;
}

static void keypad_limit(void)
{
    uint8_t b[96];
    size_t bl;
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    bl = body_pin(b, sizeof b, "48261");
    expect_int("5-digit PIN refused", call(&owner, &ble, OZ_COAP_POST, "cred", b, bl) == OZ_R_CRED_OK, 0);
    bl = body_pin(b, sizeof b, "482613");
    expect_int("add PIN 482613", call(&owner, &ble, OZ_COAP_POST, "cred", b, bl), OZ_R_CRED_OK);
    /* A one-use PIN: once spent, typing it is a miss like any other. */
    oz_cbor_w w;
    bl = body_epoch(b, sizeof b, 3);
    oz_cbor_w_init(&w, b + bl, sizeof b - bl);
    oz_cbor_put_uint(&w, 1); oz_cbor_put_uint(&w, OZ_CRED_PIN);
    oz_cbor_put_uint(&w, 3); oz_cbor_put_bstr(&w, (const uint8_t *)"777777", 6);
    oz_cbor_put_uint(&w, 6); oz_cbor_put_uint(&w, 1);
    bl += w.len;
    expect_int("add one-use PIN 777777", call(&owner, &ble, OZ_COAP_POST, "cred", b, bl), OZ_R_CRED_OK);
    expect_int("777777 opens once", oz_lock_keypad_pin(&L, "777777"), OZ_PIN_OPEN);

    int before = unlocks;
    expect_int("anti-peep: PIN inside other digits", oz_lock_keypad_pin(&L, "994826131"), OZ_PIN_OPEN);
    expect_int("anti-peep: at the very end", oz_lock_keypad_pin(&L, "000000000482613"), OZ_PIN_OPEN);
    expect_int("16 digits is the most", oz_lock_keypad_pin(&L, "1111111111482613"), OZ_PIN_OPEN);
    expect_int("17 digits refused, PIN or not", oz_lock_keypad_pin(&L, "11111111111482613"), OZ_PIN_WRONG);
    expect_int("a prefix is not the PIN", oz_lock_keypad_pin(&L, "48261"), OZ_PIN_WRONG);
    expect_int("non-digits refused", oz_lock_keypad_pin(&L, "4826a13"), OZ_PIN_WRONG);
    expect_int("4th miss: the spent PIN", oz_lock_keypad_pin(&L, "777777"), OZ_PIN_WRONG);
    expect_int("no lockout after 4 misses", oz_lock_keypad_lockout_ms(&L) == 0, 1);
    expect_int("5th miss", oz_lock_keypad_pin(&L, "0001"), OZ_PIN_WRONG);
    uint64_t left = oz_lock_keypad_lockout_ms(&L);
    expect_int("5 misses: 60 s lockout", left > 59000 && left <= 60000, 1);
    expect_int("locked out: the right PIN is not checked", oz_lock_keypad_pin(&L, "482613"), OZ_PIN_LOCKED_OUT);
    expect_int("…and the door stayed shut", unlocks - before, 3);

    /* A reboot (battery pulled) restores the lockout from kv. */
    boot_lock();
    left = oz_lock_keypad_lockout_ms(&L);
    expect_int("lockout survives a reboot", left > 59000 && left <= 60000, 1);
    expect_int("still not checked after the reboot", oz_lock_keypad_pin(&L, "482613"), OZ_PIN_LOCKED_OUT);

    /* An owner unlock from the app clears it. */
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    expect_int("owner unlocks from the app", call(&owner, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0),
               OZ_R_UNLOCK_OK);
    expect_int("owner unlock clears the lockout", oz_lock_keypad_lockout_ms(&L) == 0, 1);
    expect_int("PIN opens again", oz_lock_keypad_pin(&L, "482613"), OZ_PIN_OPEN);

    /* An unreadable record fails closed: locked out at the cap. */
    oz_port_kv_set("pl", (const uint8_t *)"\x07", 1);
    boot_lock();
    left = oz_lock_keypad_lockout_ms(&L);
    expect_int("bad record: locked out 30 min", left > 1799000 && left <= 1800000, 1);
    expect_int("bad record: nothing checked", oz_lock_keypad_pin(&L, "482613"), OZ_PIN_LOCKED_OUT);
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    call(&owner, &ble, OZ_COAP_POST, "lock/unlock", NULL, 0);
    expect_int("owner clears it", oz_lock_keypad_pin(&L, "482613"), OZ_PIN_OPEN);
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

/* ── events (§5.1): Observe over BLE and the relay ────────────────────── */

typedef struct {
    oz_oscore_xchg x;          /* the registering request's exchange */
    uint8_t token;
    uint32_t last_obs;
} watch_t;

/* GET events, Observe `observe` (−1 none), optional "after=N" (−1 none).
 * Repeats once with Echo when asked. Returns the result code. */
static int events_get(app_t *a, oz_link *k, int observe, long after, watch_t *wt)
{
    uint8_t req[200];
    uint8_t echo[8];
    int with_echo = 0;
    for (int round = 0; round < 2; round++) {
        oz_coap_msg m;
        char q[24];
        uint8_t ob = (uint8_t)observe;
        oz_coap_init(&m, OZ_COAP_CON, OZ_COAP_GET, ++a->mid);
        m.tkl = 1;
        m.token[0] = (uint8_t)(0x80 | a->mid);
        if (observe >= 0)
            oz_coap_add_opt(&m, OZ_COAP_OPT_OBSERVE, &ob, observe ? 1 : 0);
        oz_coap_set_path(&m, "events");
        if (after >= 0) {
            snprintf(q, sizeof q, "after=%ld", after);
            oz_coap_add_opt(&m, OZ_COAP_OPT_URI_QUERY, (const uint8_t *)q, strlen(q));
        }
        if (with_echo)
            oz_coap_add_opt(&m, OZ_COAP_OPT_ECHO, echo, 8);
        oz_oscore_xchg x;
        size_t n = 0;
        oz_oscore_protect_req(&a->osc, &m, req, sizeof req, &n, &x);
        if (deliver(a, k, req, n, &x))
            return -1;
        if (!a->has_echo) {
            if (wt) {
                wt->x = x;
                wt->token = m.token[0];
                const oz_coap_opt *o = oz_coap_find(&a->resp, OZ_COAP_OPT_OBSERVE);
                wt->last_obs = o ? (o->len ? o->val[o->len - 1] : 0) : 0xffffffffu;
            }
            return a->result;
        }
        memcpy(echo, a->echo, 8);
        with_echo = 1;
    }
    return -1;
}

typedef struct {
    int64_t seq, type, basis, utc, actor, d1, d2;
} ev_t;

/* One notification from the lock on link k, verified as the app does.
 * Returns 1 with *ev, 0 when nothing was sent, −1 when it doesn't verify,
 * isn't bound to the registration, or its Observe value didn't increase. */
static int notification(app_t *a, oz_link *k, watch_t *wt, ev_t *ev)
{
    uint8_t out[600], lt;
    size_t ol = 0;
    oz_lock_events_poll(&L, k, &lt, out, sizeof out, &ol);
    if (ol && framed_mtu && k == &ble) {            /* as the GATT path sends it */
        oz_ble_send(&gatt, lt, out, ol);
        take(&lt, out, sizeof out, &ol);
    }
    if (!ol)
        return 0;
    int64_t piv;
    oz_coap_msg r;
    if (lt != OZ_FRAME_OSCORE || oz_oscore_verify_resp(&a->osc, &wt->x, out, ol, a->pt, sizeof a->pt, &r, &piv))
        return -1;
    const oz_coap_opt *o = oz_coap_find(&r, OZ_COAP_OPT_OBSERVE);
    uint32_t obs = 0;
    for (size_t i = 0; o && i < o->len; i++)
        obs = obs << 8 | o->val[i];
    if (!o || r.tkl != 1 || r.token[0] != wt->token || r.code != OZ_COAP_CONTENT || piv < 0 ||
        r.type != OZ_COAP_NON || (wt->last_obs != 0xffffffffu && obs <= wt->last_obs))
        return -1;
    wt->last_obs = obs;
    memset(ev, 0xff, sizeof *ev);
    oz_cbor_r c;
    oz_cbor_r_init(&c, r.payload, r.payload_len);
    size_t pairs = oz_cbor_get_map(&c);
    for (size_t i = 0; i < pairs && !c.err; i++) {
        uint64_t key = oz_cbor_get_uint(&c);
        if (key == 6) {
            size_t dp = oz_cbor_get_map(&c);
            for (size_t j = 0; j < dp && !c.err; j++) {
                uint64_t dk = oz_cbor_get_uint(&c);
                int64_t v = (int64_t)oz_cbor_get_uint(&c);
                if (dk == 1) ev->d1 = v;
                if (dk == 2) ev->d2 = v;
            }
            continue;
        }
        int64_t v = oz_cbor_get_int(&c);
        if (key == 1) ev->seq = v;
        if (key == 2) ev->type = v;
        if (key == 3) ev->basis = v;
        if (key == 4) ev->utc = v;
        if (key == 5) ev->actor = v;
    }
    return c.err ? -1 : 1;
}

/* A keypad lockout: wait out any running one, then miss until it locks. */
static void raise_lockout(void)
{
    oz_host_advance_ms(oz_lock_keypad_lockout_ms(&L) + 1);
    while (!oz_lock_keypad_lockout_ms(&L))
        (void)oz_lock_keypad_pin(&L, "000000");
}

static void events(void)
{
    watch_t wb, wr;
    ev_t ev;
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    oz_host_advance_ms(oz_lock_keypad_lockout_ms(&L) + 1);
    (void)oz_lock_keypad_pin(&L, "482613");      /* a success: the limit starts afresh */

    /* keypad_limit raised one event (seq 0), which reserved 0..31 in kv; two
     * reboots since then lost the RAM ring and start numbering at 32. */
    expect_int("plain GET", events_get(&owner, &ble, -1, -1, NULL), OZ_R_OK);
    expect_int("…next seq after the reboots", resp_int(&owner, 1), 32);
    expect_int("…nothing held", resp_int(&owner, 2), 32);
    expect_int("register (BLE)", events_get(&owner, &ble, 0, -1, &wb), OZ_R_OK);
    expect_int("…answer carries Observe", wb.last_obs != 0xffffffffu, 1);
    expect_int("…2.05", owner.code, OZ_COAP_CONTENT);
    expect_int("nothing yet", notification(&owner, &ble, &wb, &ev), 0);

    /* Five misses: the lockout reaches the owner's app (PM ruling). */
    raise_lockout();
    expect_int("lockout notified", notification(&owner, &ble, &wb, &ev), 1);
    expect_int("…seq 32", (long long)ev.seq, 32);
    expect_int("…keypad lockout", (long long)ev.type, OZ_EV_KEYPAD_LOCKOUT);
    expect_int("…utc basis (clock set)", ev.basis == 1 && ev.utc >= 1790000000, 1);
    expect_int("…no actor", (long long)ev.actor, -1);
    expect_int("…level 1, 60 s", ev.d1 == 1 && ev.d2 == 60, 1);
    expect_int("once only", notification(&owner, &ble, &wb, &ev), 0);
    raise_lockout();
    expect_int("next lockout", notification(&owner, &ble, &wb, &ev), 1);
    expect_int("…seq 33, level 2, 120 s", ev.seq == 33 && ev.d1 == 2 && ev.d2 == 120, 1);

    /* Admin only for now: the service bond gets FORBIDDEN, not a stream. */
    oz_link_open(&relay, 1);
    handshake(&service, &relay, NULL, 0, NULL);
    expect_int("service: not yet", events_get(&service, &relay, 0, -1, NULL), OZ_R_FORBIDDEN);
    expect_int("…nothing sent to it", notification(&service, &relay, &wr, &ev), 0);

    /* The relay: the owner's own registration, from after=32. The app keeps
     * one OSCORE context per link. */
    oz_oscore_ctx on_ble = owner.osc, on_relay;
    handshake(&owner, &relay, NULL, 0, NULL);
    on_relay = owner.osc;
    expect_int("register (relay), after=32", events_get(&owner, &relay, 0, 32, &wr), OZ_R_OK);
    expect_int("relay gets 33 only", notification(&owner, &relay, &wr, &ev) == 1 && ev.seq == 33, 1);
    expect_int("…and then nothing", notification(&owner, &relay, &wr, &ev), 0);

    /* 20 lockouts unread: each reader gets one gap marker, then the 16 held. */
    for (int i = 0; i < 20; i++)
        raise_lockout();
    int64_t first_held = 34 + 20 - OZ_EVENTS_RING;
    expect_int("relay: gap first", notification(&owner, &relay, &wr, &ev), 1);
    expect_int("…gap from 34, 4 missed",
               ev.type == OZ_EV_GAP && ev.seq == 34 && ev.d1 == first_held - 34, 1);
    int n = 0, ok = 1;
    while (notification(&owner, &relay, &wr, &ev) == 1) {
        ok &= ev.type == OZ_EV_KEYPAD_LOCKOUT && ev.seq == first_held + n;
        n++;
    }
    expect_int("…then the 16 held, in order", n == OZ_EVENTS_RING && ok, 1);
    on_relay = owner.osc;
    owner.osc = on_ble;
    expect_int("BLE reader: its own gap", notification(&owner, &ble, &wb, &ev) == 1 && ev.type == OZ_EV_GAP, 1);
    n = 0;
    while (notification(&owner, &ble, &wb, &ev) == 1)
        n++;
    expect_int("…and its own 16", n, OZ_EVENTS_RING);
    expect_int("lockouts capped at 30 min", (long long)ev.d2, 1800);

    /* A cursor ahead of the lock waits; deregistering stops the stream. */
    expect_int("re-register after=999", events_get(&owner, &ble, 0, 999, &wb), OZ_R_OK);
    expect_int("…nothing", notification(&owner, &ble, &wb, &ev), 0);
    expect_int("re-register, up to date", events_get(&owner, &ble, 0, (long)L.events.next_seq - 1, &wb), OZ_R_OK);
    expect_int("deregister (Observe 1)", events_get(&owner, &ble, 1, -1, NULL), OZ_R_OK);
    raise_lockout();
    expect_int("deregistered: nothing", notification(&owner, &ble, &wb, &ev), 0);
    on_ble = owner.osc;
    owner.osc = on_relay;
    expect_int("the relay still gets it", notification(&owner, &relay, &wr, &ev), 1);
    owner.osc = on_ble;

    /* A BLE registration ends with the session. */
    expect_int("register again", events_get(&owner, &ble, 0, -1, &wb), OZ_R_OK);
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    raise_lockout();
    expect_int("new session: not registered", notification(&owner, &ble, &wb, &ev), 0);

    /* A new relay context replaces the old registration. */
    handshake(&owner, &relay, NULL, 0, NULL);
    expect_int("new relay context: not registered", notification(&owner, &relay, &wr, &ev), 0);

    /* A reboot loses the ring and the registrations; numbers jump, never repeat. */
    uint32_t before = L.events.next_seq;
    boot_lock();
    expect_int("after a reboot: past every seq used", (long long)(L.events.next_seq - before) > 0, 1);
    expect_int("…ring empty", L.events.count, 0);

    /* A factory reset forgets the numbering. */
    oz_link_open(&ble, 0);
    handshake(&owner, &ble, NULL, 0, NULL);
    expect_int("factory reset", call(&owner, &ble, OZ_COAP_POST, "dev/reset", NULL, 0), OZ_R_OK);
    uint8_t es[4];
    size_t esl;
    expect_int("…event numbering erased", oz_port_kv_get("es", es, sizeof es, &esl) != OZ_OK, 1);
    expect_int("…and restarts at 0", L.events.next_seq, 0);
}

static void run_suite(size_t mtu)
{
    framed_mtu = mtu;
    oz_ble_init(&gatt, &L, &ble, capture, NULL);
    oz_ble_set_mtu(&gatt, mtu ? mtu : OZ_FRAME_MIN_MTU);
    oz_reasm_reset(&got);
    unlocks = locks = prompt_on = 0;
    oz_host_kv_reset();
    boot_lock();
    app_new(&owner); app_new(&member); app_new(&stranger);
    app_new(&issuer); app_new(&guest); app_new(&service);

    int before = failures;
    first_pairing();
    members();
    credentials();
    keypad_limit();
    relay_and_reboot();
    roles();
    events();
    pair_lockout();
    if (mtu)
        printf("lock over GATT, MTU %zu: %ld frames in, %ld out, %d new failures\n",
               mtu, frames_in, frames_out, failures - before);
    frames_in = frames_out = 0;
}

int main(void)
{
    run_suite(0);       /* the engine, called directly */
    run_suite(23);      /* the same flows through oz_ble at the BLE default MTU */
    run_suite(247);     /* and at the MTU apps should negotiate */
    expect_int("GATT reply frames well-formed", got_bad, 0);
    return report("lock");
}
