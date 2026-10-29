/* test_ak1037_gatt — the AK1037 lock image on the host: oz.start
 * (oz_lockapp_ak1037.c), the GATT service and the OZKIE thread
 * (oz_gatt_ak1037.c) and the real port (oz_port_ak1037.c), against the SDK
 * mocks (ozkey-49 P5-A).
 *
 * A simulated phone claims the lock through the mocked BLE stack only: GATT
 * writes and reads, link events, notifications. Each of its frames goes
 * through the stack's callback, the message queue and the OZKIE thread, which
 * runs in lockstep (mock_sdk.h). Along the way: the advertising mode on every
 * change, MTU 23 fragmentation, notify back-pressure (BUFFER_FULL), a
 * notify that fails outright, writes the service must ignore, a full queue,
 * and an advertising update the stack refuses.
 */
#include <stdlib.h>
#include <unistd.h>

#include "oz_test.h"
#include "oz_port.h"
#include "oz_lock.h"
#include "oz_cbor.h"
#include "oz_coap.h"
#include "oz_edhoc.h"
#include "oz_oscore.h"
#include "oz_frame.h"
#include "oz_adv.h"
#include "oz_ble.h"
#include "oz_gatt_ak1037.h"
#include "mock_sdk.h"

#define LINK ((void *)0x1234)

/* gatt_table rows (oz_gatt_ak1037.c) */
#define REC_INFO  (&mock_gatt_table[2])
#define REC_OZKIE (&mock_gatt_table[4])

/* ── the phone's GATT client ───────────────────────────────────────────── */

static size_t mtu = 23;
static oz_reasm got;
static int got_done, got_bad, notify_full, notify_fail;
static long notifies, notify_attempts;

static T_eBLE_RESULT on_notify(void *link, const uint8_t *f, uint16_t n, uint16_t idx)
{
    int d;
    notify_attempts++;
    if (notify_full > 0) {
        notify_full--;
        return eBLE_RESULT_BUFFER_FULL;
    }
    if (notify_fail)
        return eBLE_RESULT_SEND_ERR;
    notifies++;
    if (link != LINK || idx != 4 || n > mtu - 3 || oz_reasm_feed(&got, f, n, &d))
        got_bad++;
    else
        got_done |= d;
    return eBLE_RESULT_SUCCESS;
}

/* Write one message frame by frame; the thread takes each as it arrives.
 * Returns the reassembled reply's length (0: none) in out. */
static size_t exchange(uint8_t type, const uint8_t *msg, size_t len, uint8_t *out, size_t cap)
{
    unsigned count;
    got_done = 0;
    oz_reasm_reset(&got);
    if (oz_frag_count(len, mtu, &count))
        return 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t f[520];
        size_t n;
        oz_frag(type, msg, len, mtu, i, f, sizeof f, &n);
        mock_ble_write(LINK, REC_OZKIE, f, (uint16_t)n);
        mock_pass();
    }
    if (!got_done || got.len > cap)
        return 0;
    memcpy(out, got.buf, got.len);
    return got.len;
}

/* ── the phone's app ───────────────────────────────────────────────────── */

static uint8_t app_priv[32], app_pub[32], app_ccs[OZ_CCS_LEN], app_kccs[64];
static size_t app_kccs_len;
static oz_edhoc e;
static oz_oscore_ctx osc;
static uint16_t mid;
static uint8_t lock_pub[32], lock_ccs[OZ_CCS_LEN];
static char lock_id[32];
static int lock_claimed = -1;

static int read_info(void)
{
    uint8_t b[128];
    uint16_t n = mock_ble_read(LINK, REC_INFO, b, sizeof b);
    oz_cbor_r r;
    oz_cbor_r_init(&r, b, n);
    size_t pairs = oz_cbor_get_map(&r);
    for (size_t i = 0; i < pairs && !r.err; i++) {
        uint64_t k = oz_cbor_get_uint(&r);
        size_t l;
        const uint8_t *v;
        if (k == 1 && (v = oz_cbor_get_tstr(&r, &l)) && l < sizeof lock_id) {
            memcpy(lock_id, v, l);
            lock_id[l] = 0;
        } else if (k == 3 && (v = oz_cbor_get_bstr(&r, &l)) && l == 32) {
            memcpy(lock_pub, v, 32);
        } else if (k == 4) {
            lock_claimed = (int)oz_cbor_get_uint(&r);
        } else {
            (void)oz_cbor_skip(&r, &l);
        }
    }
    return r.err ? -1 : (int)n;
}

static int app_lock_cred(void *app, const uint8_t *id, size_t len, oz_edhoc_cred *out)
{
    static const uint8_t lock_idc[4] = { 0xa1, 0x04, 0x41, 0x00 };
    (void)app;
    if (len != 4 || memcmp(id, lock_idc, 4))
        return OZ_E_NOT_FOUND;
    oz_ccs_for(lock_pub, lock_ccs);
    out->cred = lock_ccs;
    out->cred_len = OZ_CCS_LEN;
    memcpy(out->pub, lock_pub, 32);
    return OZ_OK;
}

static void app_new(void)
{
    oz_port_random(app_priv, 32);
    oz_port_x25519_public_key(app_pub, app_priv);
    oz_ccs_for(app_pub, app_ccs);
    oz_cbor_w w;
    oz_cbor_w_init(&w, app_kccs, sizeof app_kccs);
    oz_cbor_put_map(&w, 1);
    oz_cbor_put_uint(&w, 14);
    oz_cbor_put_raw(&w, app_ccs, OZ_CCS_LEN);
    app_kccs_len = w.len;
}

static int handshake(void)
{
    uint8_t m1[128], m2[256], m3[512], lo[256];
    size_t m1l, m2l, m3l, lol;
    oz_edhoc_init(&e, 1);
    e.c_own.len = 1;
    e.c_own.b[0] = 0x05;
    e.own.cred = app_ccs;
    e.own.cred_len = OZ_CCS_LEN;
    e.own.id_cred = app_kccs;
    e.own.id_cred_len = app_kccs_len;
    memcpy(e.own_priv, app_priv, 32);
    e.get_cred = app_lock_cred;

    oz_edhoc_i_msg1(&e, NULL, 0, m1, sizeof m1, &m1l);
    m2l = exchange(OZ_FRAME_EDHOC, m1, m1l, m2, sizeof m2);
    expect_int("message_2 came back", m2l > 0, 1);
    expect_int("message_2 needed several notifications", notifies > 1, 1);
    if (!m2l || oz_edhoc_i_msg2(&e, m2, m2l, lo, sizeof lo, &lol))
        return -1;
    oz_edhoc_i_msg3(&e, NULL, 0, m3, sizeof m3, &m3l);
    if (exchange(OZ_FRAME_EDHOC, m3, m3l, lo, sizeof lo))
        return -1;                      /* an EDHOC error */
    uint8_t sec[16], salt[8];
    oz_edhoc_cid sid, rid;
    oz_edhoc_oscore(&e, sec, salt, &sid, &rid);
    oz_oscore_derive(&osc, sec, 16, salt, 8, sid.b, sid.len, rid.b, rid.len, NULL, 0);
    return 0;
}

static size_t request(uint8_t method, const char *path, uint8_t *out, size_t cap, oz_oscore_xchg *x)
{
    oz_coap_msg m;
    oz_coap_init(&m, OZ_COAP_CON, method, ++mid);
    m.tkl = 1;
    m.token[0] = (uint8_t)mid;
    oz_coap_set_path(&m, path);
    size_t n = 0;
    oz_oscore_protect_req(&osc, &m, out, cap, &n, x);
    return n;
}

/* The response to x in the last reassembled notification: its CoAP code and
 * the result field, or -1. */
static int response(const uint8_t *in, size_t n, oz_oscore_xchg *x, int *result)
{
    static uint8_t pt[512];
    oz_coap_msg resp;
    int64_t piv;
    *result = -1;
    if (!n || oz_oscore_verify_resp(&osc, x, in, n, pt, sizeof pt, &resp, &piv))
        return -1;
    if (resp.payload_len) {
        oz_cbor_r r;
        oz_cbor_r_init(&r, resp.payload, resp.payload_len);
        if (oz_cbor_get_map(&r) && oz_cbor_get_uint(&r) == 0)
            *result = (int)oz_cbor_get_uint(&r);
    }
    return resp.code;
}

/* ── advertising, as the stack was left ────────────────────────────────── */

static void expect_adv(const char *what, oz_adv_mode mode)
{
    char name[80];
    uint8_t d[OZ_ADV_MAX], r[OZ_ADV_MAX];
    snprintf(name, sizeof name, "%s: advertising on", what);
    expect_int(name, mock_adv_enabled, mode != OZ_ADV_OFF);
    if (mode == OZ_ADV_OFF)
        return;
    snprintf(name, sizeof name, "%s: type", what);
    expect_int(name, mock_adv_type, mode == OZ_ADV_BUSY ? eBLE_ADV_SCAN_IND : eBLE_ADV_IND);
    snprintf(name, sizeof name, "%s: interval", what);
    expect_int(name, mock_adv_min == 800 && mock_adv_max == 800, 1);
    size_t dl = oz_adv_data(d), rl = oz_adv_scan_rsp(r, mode == OZ_ADV_BUSY, lock_id);
    snprintf(name, sizeof name, "%s: ADV bytes", what);
    expect_int(name, dl == mock_adv_data_len && !memcmp(d, mock_adv_data, dl), 1);
    snprintf(name, sizeof name, "%s: scan response bytes", what);
    expect_int(name, rl && rl == mock_scan_rsp_len && !memcmp(r, mock_scan_rsp, rl), 1);
}

/* ── the scenario ──────────────────────────────────────────────────────── */

static void boot(void)
{
    mock_flash_blank();
    mock_ms = 5000;
    mock_on_notify = on_notify;
    expect_int("oz.start exists", mock_msh_run("oz.start"), 0);
    expect_int("BLE initialised once", mock_ble_inits, 1);
    expect_int("server role", mock_ble.role_type, eBLE_ROLE_SERVER);
    expect_int("stack's own advertising off", mock_ble.adv_def_on, 0);
    expect_int("callbacks set", mock_ble.recv_cb && mock_ble.read_cb && mock_ble.evt_handle_cb, 1);
    expect_int("GATT table rows", mock_gatt_len, 6);
    expect("service UUID", mock_gatt_table[0].p_data, 16, "010000000000" "4b434f4c" "01" "00324b5a4f");
    expect("ozkie UUID", mock_gatt_table[3].p_data, 16, "010000000000" "4b434f4c" "03" "00324b5a4f");
    mock_wait_parked();

    expect_int("info read", read_info() > 0, 1);
    expect("device id from the BLE address", (const uint8_t *)lock_id, strlen(lock_id),
           "6f7a6b2d" "6163626565363361636162 38");     /* ozk-acbee63acab8 */
    expect_int("unclaimed", lock_claimed, 0);
    uint8_t b[4];
    expect_int("info truncated to the buffer", mock_ble_read(LINK, REC_INFO, b, sizeof b), 4);
    expect_int("other rows read nothing", mock_ble_read(LINK, REC_OZKIE, b, sizeof b), 0);

    mock_pass();
    expect_adv("boot", OZ_ADV_OPEN);

    expect_int("second oz.start refused (no second thread)", mock_msh_run("oz.start"), 0);
    expect_int("still one BLE init", mock_ble_inits, 1);
}

static void claim(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    mock_ble_event(LINK, eBT_MSG_LE_CONNECTION_SUCC, 0);
    expect_int("MTU 247 requested", mock_mtu_requested, 247);
    mock_pass();
    expect_adv("connected", OZ_ADV_BUSY);
    mock_ble_event(LINK, eBT_MSG_LE_MTU_EXCHANGED, 23);
    mock_pass();

    app_new();
    expect_int("handshake over GATT at MTU 23", handshake(), 0);

    /* POST /pair is held until the code is typed. */
    size_t n = request(OZ_COAP_POST, "pair", req, sizeof req, &x);
    expect_int("pair held", (long long)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out), 0);

    /* The answer meets a full send ring three times, then goes out. */
    uint32_t code;
    oz_edhoc_pair_code(&e, &code);
    char cmd[32];
    snprintf(cmd, sizeof cmd, "oz.key %06u#", (unsigned)code);   /* typed on the keypad */
    got_done = 0;
    oz_reasm_reset(&got);
    notify_full = 3;
    long attempts = notify_attempts, sent = notifies;
    uint32_t t0 = mock_ms;
    expect_int("oz.key exists", mock_msh_run(cmd), 0);
    for (int i = 0; i < 7; i++)             /* six digits and '#', one event each */
        mock_pass();
    expect_int("BUFFER_FULL retried, then sent", notify_attempts - attempts, (notifies - sent) + 3);
    expect_int("waited 5 ms per retry", (long long)(mock_ms - t0) >= 15, 1);
    expect_int("pair answered", got_done, 1);
    expect_int("right code -> 2.04", response(got.buf, got.len, &x, &result), OZ_COAP_CHANGED);
    expect_int("info says claimed", read_info() > 0 && lock_claimed == 1, 1);

    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    expect_int("unlock answered", response(out, ol, &x, &result), OZ_COAP_CHANGED);
    expect_int("unlocked", result, OZ_R_UNLOCK_OK);

    /* A notify the stack refuses outright: that reply is lost, nothing hangs. */
    notify_fail = 1;
    attempts = notify_attempts;
    n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    expect_int("refused notify: no reply", (long long)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out), 0);
    expect_int("refused notify: one attempt", notify_attempts - attempts, 1);
    notify_fail = 0;

    /* A ring that never drains: 50 tries, then give up. */
    notify_full = 1000;
    attempts = notify_attempts;
    n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    expect_int("full ring: 50 tries", notify_attempts - attempts, 50);
    notify_full = 0;

    expect_int("every notification well formed", got_bad, 0);
}

/* ── the keypad, through the shell and the OZKIE thread ────────────────── */

static char out_buf[1 << 16];
static int saved_fd = -1;
static FILE *cap_file;

static void capture_start(void)
{
    fflush(stdout);
    saved_fd = dup(1);
    cap_file = tmpfile();
    if (saved_fd < 0 || !cap_file || dup2(fileno(cap_file), 1) < 0)
        abort();
}

static void capture_end(void)
{
    fflush(stdout);
    dup2(saved_fd, 1);
    close(saved_fd);
    rewind(cap_file);
    size_t n = fread(out_buf, 1, sizeof out_buf - 1, cap_file);
    out_buf[n] = 0;
    fclose(cap_file);
    fputs(out_buf, stdout);
}

static int count(const char *s)
{
    int n = 0;
    for (const char *p = out_buf; (p = strstr(p, s)); p += strlen(s))
        n++;
    return n;
}

/* Press keys; the thread takes one event per key. */
static void keys(const char *k)
{
    char cmd[48];
    snprintf(cmd, sizeof cmd, "oz.key %s", k);
    mock_msh_run(cmd);
    for (size_t i = 0; i < strlen(k); i++)
        mock_pass();
}

static void keypad_lockout(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    capture_start();
    for (int i = 0; i < 4; i++)
        keys("0000#");
    capture_end();
    expect_int("4 wrong PINs", count("keypad: wrong"), 4);
    expect_int("no lockout yet", count("locked out"), 0);

    capture_start();
    keys("0000#");
    keys("4826#");
    capture_end();
    expect_int("5th: locked out for 60 s", count("lock: keypad locked out for 60 s"), 1);
    expect_int("next entry not checked", count("keypad: locked out, 60 s left, not checked"), 1);

    /* An owner unlock from the app clears it. */
    size_t n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    expect_int("owner unlock", response(out, ol, &x, &result) == OZ_COAP_CHANGED && result == OZ_R_UNLOCK_OK, 1);
    capture_start();
    keys("0000#");
    capture_end();
    expect_int("after the owner unlock: checked again", count("keypad: wrong"), 1);

    /* A half-typed entry is dropped after 10 s; '*' clears. */
    capture_start();
    keys("12");
    mock_ms += 10000;
    mock_pass();
    keys("34*");
    keys("#");
    capture_end();
    expect_int("idle entry dropped", count("keypad: entry timed out"), 1);
    expect_int("* cleared", count("keypad: cleared"), 1);
    expect_int("# on nothing checks nothing", count("keypad: wrong"), 0);
}

static void junk_and_limits(void)
{
    uint8_t f[BLE_MTU_MAX_SIZE];
    long sent = notifies;
    memset(f, 0x41, sizeof f);

    /* Writes the service must drop: wrong row, empty, too long. The largest
     * legal frame is MTU max − 3. */
    mock_ble_write(LINK, REC_INFO, f, 20);
    mock_ble_write(LINK, REC_OZKIE, f, 0);
    mock_ble_write(LINK, REC_OZKIE, f, BLE_MTU_MAX_SIZE - 2);
    mock_ble_write(LINK, REC_OZKIE, f, BLE_MTU_MAX_SIZE - 3);  /* queued; a bad frame */
    mock_pass();
    expect_int("junk writes: no reply", notifies - sent, 0);

    /* A burst over the queue depth: the extras are dropped, the rest run. */
    for (int i = 0; i < 12; i++)
        mock_ble_write(LINK, REC_OZKIE, f, 30);
    for (int i = 0; i < 12; i++)
        mock_pass();
    expect_int("burst: no reply, no crash", notifies - sent, 0);
}

static void disconnect_and_adv(void)
{
    long sent = notifies;
    mock_ble_event(LINK, eBT_MSG_LE_LINK_DISCONNECTED, 0);
    mock_pass();
    expect_adv("disconnected", OZ_ADV_OPEN);

    /* Frames after the link is gone go nowhere. */
    uint8_t f[8] = { 0 };
    mock_ble_write(LINK, REC_OZKIE, f, sizeof f);
    mock_pass();
    expect_int("no notify without a link", notifies - sent, 0);

    /* The stack refuses the new advertising data: retried on every pass. */
    mock_adv_data_fail = 1;
    mock_ble_event(LINK, eBT_MSG_LE_CONNECTION_SUCC, 0);
    mock_pass();
    expect_int("refused update: not advertising", mock_adv_enabled, 0);
    mock_pass();
    expect_int("still refused", mock_adv_enabled, 0);
    mock_adv_data_fail = 0;
    mock_pass();
    expect_adv("retried", OZ_ADV_BUSY);
    mock_ble_event(LINK, eBT_MSG_LE_LINK_DISCONNECTED, 0);
    mock_pass();
    expect_adv("reconnect cycle", OZ_ADV_OPEN);

    /* A touch with always-advertise changes nothing; an hour later, still on. */
    long applies = mock_adv_applies;
    expect_int("oz.touch exists", mock_msh_run("oz.touch"), 0);
    mock_pass();
    mock_ms += 3600u * 1000u;
    mock_pass();
    expect_int("always advertising: no re-apply", mock_adv_applies, applies);
    expect_adv("an hour later", OZ_ADV_OPEN);
}

int main(void)
{
    boot();
    claim();
    keypad_lockout();
    junk_and_limits();
    disconnect_and_adv();
    expect_int("flash stayed in OZKV", mock_flash_out_of_part, 0);
    expect_int("NOR rule kept", mock_nor_violations, 0);
    expect_int("SKE only with interrupts off", mock_irq_violations, 0);
    expect_int("the engine did the AES", mock_ske_blocks > 1, 1);
    /* P5-D: the OZKIE thread's deepest stack over everything above (claim,
     * EDHOC, kv writes, keypad lockout), on the host. Release builds hold it
     * to the thread's budget as a tripwire. It covers only the paths run
     * here; the RV32 worst case is the static report (ozkey-49 P5-D). */
    size_t peak = mock_stack_peak("ozkie");
    printf("ozkie thread stack peak (host): %zu of %u B\n", peak, (unsigned)OZ_GATT_THREAD_STACK);
#ifdef OZ_STACK_TRIPWIRE
    expect_int("OZKIE stack measured", peak > 0, 1);
    expect_int("OZKIE stack within its budget", peak <= OZ_GATT_THREAD_STACK, 1);
#endif
    /* The OZKIE thread stays parked in rt_mq_recv; exit ends it. */
    return report("ak1037_gatt");
}
