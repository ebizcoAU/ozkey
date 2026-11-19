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
#include "oz_kv_lfs.h"
#include "oz_gatt_ak1037.h"
#include "rtthread.h"
#include "oz_board_ak1037.h"
#include "oz_port_ak1037.h"
#include "oz_motor.h"
#include "mock_sdk.h"
#include "hal_gpio.h"
#include "hal_sysclock.h"
#include "oz_wdt.h"
#include "oz_wdt_ak1037.h"

#define LINK ((void *)0x1234)

/* gatt_table rows (oz_gatt_ak1037.c) */
#define REC_INFO  (&mock_gatt_table[2])
#define REC_OZKIE (&mock_gatt_table[4])
#define REC_RELAY (&mock_gatt_table[7])   /* RELAY-AK-1 */

/* Which characteristic the client writes, and the notify index the lock must
 * answer on: ozkie = 0, relay = 1 (the SDK counts notify chars from 0). */
static const void *cur_rec;              /* NULL = REC_OZKIE */
static uint16_t cur_idx = 0;

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
    if (link != LINK || idx != cur_idx || n > mtu - 3   /* idx: the SDK counts notify chars from 0 */ || oz_reasm_feed(&got, f, n, &d))
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
        mock_ble_write(LINK, cur_rec ? cur_rec : REC_OZKIE, f, (uint16_t)n);
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
static char info_fw[48], info_model[32];   /* REL-VER-1 */
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
        } else if ((k == 2 || k == 5) && (v = oz_cbor_get_tstr(&r, &l)) && l < sizeof info_fw) {
            char *dst = k == 2 ? info_fw : info_model;
            memcpy(dst, v, l);
            dst[l] = 0;
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

/* The Echo of the last 4.01 (OSCORE B.1.2: the relay link asks for one before
 * its first request), sent by the next request_echo(). */
static uint8_t last_echo[8];
static int has_echo;

static size_t request_echo(uint8_t method, const char *path, const uint8_t *echo,
                           uint8_t *out, size_t cap, oz_oscore_xchg *x)
{
    oz_coap_msg m;
    oz_coap_init(&m, OZ_COAP_CON, method, ++mid);
    m.tkl = 1;
    m.token[0] = (uint8_t)mid;
    oz_coap_set_path(&m, path);
    if (echo)
        oz_coap_add_opt(&m, OZ_COAP_OPT_ECHO, echo, 8);
    size_t n = 0;
    oz_oscore_protect_req(&osc, &m, out, cap, &n, x);
    return n;
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
    const oz_coap_opt *eo = oz_coap_find(&resp, OZ_COAP_OPT_ECHO);
    has_echo = eo && eo->len == 8;
    if (has_echo)
        memcpy(last_echo, eo->val, 8);
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

static int keep_flash;                  /* safe mode: kv is already mounted */

static void boot(void)
{
    if (!keep_flash)
        mock_flash_blank();
    mock_ms = 5000;
    mock_on_notify = on_notify;
    expect_int("oz.start exists", mock_msh_run("oz.start"), 0);
    expect_int("BLE initialised once", mock_ble_inits, 1);
    expect_int("server role", mock_ble.role_type, eBLE_ROLE_SERVER);
    expect_int("stack's own advertising off", mock_ble.adv_def_on, 0);
    expect_int("callbacks set", mock_ble.recv_cb && mock_ble.read_cb && mock_ble.evt_handle_cb, 1);
    expect_int("GATT table rows", mock_gatt_len, 9);   /* + relay (RELAY-AK-1) */
    /* MOTOR-GPIO-1: motor_A = GPIO17, motor_B = GPIO18, driven LOW (stop,
     * lowest current) with pull-downs before anything can move. */
    expect_int("motor pins are outputs", mock_gpio_dir[17] == 0 && mock_gpio_dir[18] == 0, 1);
    expect_int("motor pins low", mock_gpio_level[17] == 0 && mock_gpio_level[18] == 0, 1);
    expect_int("motor pins pulled down", mock_gpio_pulldown[17] && mock_gpio_pulldown[18], 1);
    /* TOUCH-KEYPAD-1: the EVB's 15 on-chip touch pads, as the SDK sample sets
     * them for EVB V103 (keymark 0x7fff, sensitivity 60, wake 50). */
    expect_int("touch pads registered once", mock_tk_registered, 1);
    expect_int("all 15 pads enabled", (long long)mock_tk_keymark, 0x7fff);
    expect_int("pad sensitivity as the SDK's EVB V103", mock_tk_sen0 == 60 && mock_tk_sen_wk == 50, 1);
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
    char typed[16];
    snprintf(typed, sizeof typed, "%06u#", (unsigned)code);       /* typed on the keypad */
    got_done = 0;
    oz_reasm_reset(&got);
    notify_full = 3;
    long attempts = notify_attempts, sent = notifies;
    uint32_t t0 = mock_ms;
    int krc = OZ_OK;
    for (const char *k = typed; *k; k++)    /* the touch pads' path, not the shell (REL-SHELL-1) */
        if (oz_gatt_ak1037_key(*k) != OZ_OK)
            krc = -1;
    expect_int("keypad digits queued", krc, OZ_OK);
    for (int i = 0; i < 7; i++)             /* six digits and '#', one event each */
        mock_pass();
    expect_int("BUFFER_FULL retried, then sent", notify_attempts - attempts, (notifies - sent) + 3);
    expect_int("waited 5 ms per retry", (long long)(mock_ms - t0) >= 15, 1);
    expect_int("pair answered", got_done, 1);
    expect_int("right code -> 2.04", response(got.buf, got.len, &x, &result), OZ_COAP_CHANGED);
    expect_int("info says claimed", read_info() > 0 && lock_claimed == 1, 1);
    /* REL-VER-1: a release-shaped version and the board's model (CN-LAB-1 T3). */
    expect_int("INFO fw = ozkie-ak1037-<semver>+<sha>", !strncmp(info_fw, "ozkie-ak1037-0.1.0+", 19) && strlen(info_fw) > 19, 1);
    expect_int("INFO model = ak1037-evb", !strcmp(info_model, "ak1037-evb"), 1);

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

static void idle(uint32_t ms);

/* Press keys; the thread takes one event per key. */
static void keys(const char *k)
{
    for (const char *c = k; *c; c++)        /* the touch pads' path (REL-SHELL-1) */
        (void)oz_gatt_ak1037_key(*c);
    for (size_t i = 0; i < strlen(k); i++)
        mock_pass();
}

static void touch_pads(void)
{
    /* Pads to keys (TOUCH-MAP-1, measured on the EVB 2026-11-18): the SDK
     * reports a one-bit mask per pad, from bit 14 = "1" down: 1 2 3 4 5 6 7 8 9
     * = bits 14..6, '*' 5, '0' 4, '#' 3, then mute 2, bell 1, lock 0 (not keys).
     * Every press logs its value. "123456#" is one keypad entry (wrong on a
     * fresh lock). */
    {
        static const int pads[] = { 1 << 14, 1 << 13, 1 << 12, 1 << 11, 1 << 10, 1 << 9, 1 << 3 };
        capture_start();
        for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++) {
            mock_ms += 2000;            /* a person: ~2 s between keys (TICK-RATE-1) */
            mock_tk_press(pads[i]);
            mock_pass();
        }
        capture_end();
        expect_int("each pad press logged with its number", count("touch: pad "), 7);
        expect_int("2 s between keys does not drop the entry (10 s idle)", count("keypad: entry timed out"), 0);
        expect_int("bit 3 (8) is '#'", count("touch: pad 8 -> #"), 1);
        expect_int("bit 14 (16384) is '1'", count("touch: pad 16384 -> 1"), 1);
        expect_int("the pads reached the keypad (an entry, checked: wrong)", count("keypad: wrong"), 1);
    }
    /* PER-KEY-BL-1 (operator: light only the pressed key, it saves power).
     * The J7 map (LED-MAP-1, chip 2026-11-19): every pin lights one key when
     * driven HIGH and another when driven LOW, released = dark. So a press
     * drives ONE pin to ONE level for the flash, every other pin released. */
    {
        static const int bl[] = { 22, 24, 25, 26, 27, 28, 29, 34 };
        const size_t n = sizeof bl / sizeof bl[0];
        static const struct { int pad, gpio, level; const char *what; } one[] = {
            { 1 << 14, 22, 0, "'1' = GPIO22 low" },
            { 1 << 13, 24, 1, "'2' = GPIO24 high" },
            { 1 << 12, 24, 0, "'3' = GPIO24 low" },
            { 1 << 6,  26, 1, "'9' = GPIO26 high" },
            { 1 << 9,  26, 0, "'6' = GPIO26 low" },
            { 1 << 5,  28, 1, "'*' = GPIO28 high" },
            { 1 << 4,  28, 0, "'0' = GPIO28 low" },
            { 1 << 3,  29, 0, "'#' = GPIO29 low" },
            /* BL-BOTTOM-1 (chip 2026-11-19: bottom-left, -middle, -right
             * pressed in order gave masks 4, 2, 1, twice) */
            { 1 << 2,  34, 1, "bottom-left = GPIO34 high" },
            { 1 << 1,  34, 0, "bottom-middle = GPIO34 low" },
            { 1 << 0,  29, 1, "bottom-right = GPIO29 high" },
        };
        uint32_t t0 = mock_ms;
        while (mock_ms - t0 < 1000)
            mock_pass();                /* the presses above: let their flash end */
        int rel = 1;
        for (size_t i = 0; i < n; i++)
            rel &= mock_gpio_dir[bl[i]] == GPIO_DIR_INPUT;
        expect_int("backlight pins released at rest (all keys dark)", rel, 1);
        for (size_t k = 0; k < sizeof one / sizeof one[0]; k++) {
            char name[96];
            mock_ms += 2000;
            mock_tk_press(one[k].pad);
            mock_pass();                /* takes the press */
            int only = 1;
            for (int p = 0; p < 4; p++) {
                mock_pass();
                for (size_t i = 0; i < n; i++)
                    only &= bl[i] == one[k].gpio
                            ? mock_gpio_dir[bl[i]] == GPIO_DIR_OUTPUT && mock_gpio_level[bl[i]] == one[k].level
                            : mock_gpio_dir[bl[i]] == GPIO_DIR_INPUT;
            }
            snprintf(name, sizeof name, "a press lights only its own key: %s, the rest released", one[k].what);
            expect_int(name, only, 1);
            t0 = mock_ms;
            while (mock_ms - t0 < 600)
                mock_pass();
            rel = 1;
            for (size_t i = 0; i < n; i++)
                rel &= mock_gpio_dir[bl[i]] == GPIO_DIR_INPUT;
            snprintf(name, sizeof name, "…%s: dark again by itself", one[k].what);
            expect_int(name, rel, 1);
        }
        /* Two pads at once are no one key: nothing lights rather than a
         * wrong key. */
        mock_ms += 2000;
        mock_tk_press((1 << 2) | (1 << 0));
        mock_pass();
        mock_pass();
        rel = 1;
        for (size_t i = 0; i < n; i++)
            rel &= mock_gpio_dir[bl[i]] == GPIO_DIR_INPUT;
        expect_int("two pads at once light nothing", rel, 1);
        idle(1000);
    }
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

    /* The owner's app observes `events` (P5-E0) over this GATT session. */
    oz_coap_msg m;
    oz_oscore_xchg xo;
    oz_coap_init(&m, OZ_COAP_CON, OZ_COAP_GET, ++mid);
    m.tkl = 1;
    m.token[0] = 0xe0;
    oz_coap_add_opt(&m, OZ_COAP_OPT_OBSERVE, NULL, 0);
    oz_coap_set_path(&m, "events");
    size_t n = 0;
    oz_oscore_protect_req(&osc, &m, req, sizeof req, &n, &xo);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    expect_int("events observed", response(out, ol, &xo, &result) == OZ_COAP_CONTENT && result == OZ_R_OK, 1);

    got_done = 0;
    oz_reasm_reset(&got);
    capture_start();
    keys("0000#");
    keys("4826#");
    capture_end();
    /* The lockout went out as a notification from the thread's tick. */
    static uint8_t pt[256];
    oz_coap_msg note;
    int64_t piv;
    int ok = got_done && !oz_oscore_verify_resp(&osc, &xo, got.buf, got.len, pt, sizeof pt, &note, &piv);
    expect_int("lockout notified over GATT", ok, 1);
    oz_cbor_r c;
    oz_cbor_r_init(&c, ok ? note.payload : NULL, ok ? note.payload_len : 0);
    int type = -1;
    size_t pairs = oz_cbor_get_map(&c);
    for (size_t i = 0; i < pairs && !c.err; i++) {
        uint64_t k = oz_cbor_get_uint(&c);
        size_t sl;
        if (k == 2)
            type = (int)oz_cbor_get_uint(&c);
        else
            (void)oz_cbor_skip(&c, &sl);
    }
    expect_int("…a keypad lockout", type, OZ_EV_KEYPAD_LOCKOUT);
    expect_int("…bound to the registration", ok && note.tkl == 1 && note.token[0] == 0xe0, 1);
    expect_int("5th: locked out for 60 s", count("lock: keypad locked out for 60 s"), 1);
    expect_int("next entry not checked", count("keypad: locked out, 60 s left, not checked"), 1);

    /* An owner unlock from the app clears it. */
    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
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

/* ── the RFID reader (P5-E), via the shell and the OZKIE thread ─────────── */

static void card_reads(const char *hex, int reads, uint32_t every_ms)
{
    uint8_t id[16];
    size_t n = 0;
    for (const char *h = hex; h[0] && h[1] && n < sizeof id; h += 2) {
        char b[3] = { h[0], h[1], 0 };
        id[n++] = (uint8_t)strtoul(b, NULL, 16);
    }
    for (int i = 0; i < reads; i++) {
        (void)oz_gatt_ak1037_card(id, n);  /* the reader's path, not the shell (REL-SHELL-1) */
        mock_pass();
        mock_ms += every_ms;
    }
}

static void cards_at_reader(void)
{
    /* KV-LAT-1: the beep starts BEFORE the limit record is written (a write
     * can take ~530 ms on the chip), and the write is done on the same pass,
     * before anything else is read: the power-cut window stays one write. */
    idle(1000);                         /* quiet: an earlier right PIN's reset is flushed at idle */
    long p0 = mock_flash_progs;
    mock_progs_at_dac_open = -1;
    mock_ms += 2000;
    capture_start();
    card_reads("04a1b2c3d4e5f6", 1, 0);
    capture_end();
    expect_int("a wrong card", count("card: wrong"), 1);
    expect_int("kvlat: its first tone started before any flash write", mock_progs_at_dac_open, p0);
    expect_int("kvlat: …and the count was written on the same pass", mock_flash_progs > p0, 1);
    capture_start();
    card_reads("04a1b2c3d4e5f6", 19, 100);      /* still held: 2 s on the reader in all */
    capture_end();
    expect_int("a card held 2 s: one check", count("card: wrong"), 0);

    capture_start();
    mock_ms += 2000;
    card_reads("11223344", 1, 0);
    card_reads("11223345", 1, 0);
    card_reads("11223346", 1, 0);
    card_reads("11223347", 1, 0);
    capture_end();
    expect_int("4 more different cards: 4 checks", count("card: wrong"), 4);
    expect_int("…the 5th miss locked the reader out", count("lock: card reader locked out for 60 s"), 1);

    capture_start();
    card_reads("04a1b2c3d4e5f7", 1, 0);
    capture_end();
    expect_int("locked out: not checked", count("card: locked out, 60 s left, not checked"), 1);
}

/* ── the motor (P5-C), log-only driver: every move is unconfirmed ────── */

static void passes(int n)
{
    for (int i = 0; i < n; i++)
        mock_pass();
}

static void motor_moves(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    passes(200);                        /* earlier moves finish */
    mock_motor_states[0] = 0;
    capture_start();
    size_t n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    uint32_t t0 = mock_ms;
    passes(30);                         /* 300 ms: still driving */
    capture_end();
    expect_int("unlock answered", response(out, ol, &x, &result) == OZ_COAP_CHANGED && result == OZ_R_UNLOCK_OK, 1);
    expect_int("driving to unlock", count("motor: to unlock"), 1);
    const char *ua = strstr(out_buf, "motor: to unlock @");
    unsigned long ta_unlock = ua ? strtoul(ua + strlen("motor: to unlock @"), NULL, 10) : 0;
    expect_int("not done at 300 ms", count("motor: unlock unconfirmed"), 0);

    capture_start();
    passes(20);
    capture_end();
    expect_int("done after the drive time", count("motor: unlock unconfirmed"), 1);
    expect_int("then brake", count("motor: brake"), 1);
    expect_int("drive ran ~400 ms", (long long)(mock_ms - t0) >= OZ_MOTOR_DRIVE_MS - 300, 1);
    /* BENCH-1: each motor line carries the lock's own ms tick ("@<ms> ms"),
     * so the drive time is measurable from the serial log, not a host clock. */
    {
        const char *b = strstr(out_buf, "motor: brake @");
        unsigned long tb = b ? strtoul(b + strlen("motor: brake @"), NULL, 10) : 0;
        expect_int("motor lines carry the tick", ta_unlock > 0 && b != NULL, 1);
        expect_int("drive time from the log = drive_ms (one 10 ms step of slack)",
                   tb >= ta_unlock + OZ_MOTOR_DRIVE_MS && tb <= ta_unlock + OZ_MOTOR_DRIVE_MS + 10, 1);
    }
    /* Roy (Anyka): A high B low turns it, both high brake, both low stop. */
    const char *s1 = strstr(mock_motor_states, "10 ");
    const char *s2 = s1 ? strstr(s1, "11 ") : NULL;
    const char *s3 = s2 ? strstr(s2, "00 ") : NULL;
    expect_int("pins: unlock drive (A=1 B=0), then brake (1 1), then stop (0 0)", s3 != NULL, 1);
    const char *rev = strstr(mock_motor_states, "01 ");
    expect_int("pins: never the lock direction during an unlock", s2 && (!rev || rev > s2), 1);
    expect_int("pins: stopped after the move", mock_gpio_level[17] == 0 && mock_gpio_level[18] == 0, 1);

    /* Lock during the rest, then unlock: the latest wins, the lock never runs. */
    capture_start();
    n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    passes(200);
    capture_end();
    expect_int("queued lock replaced", count("motor: to lock"), 0);
    expect_int("…by the unlock", count("motor: unlock unconfirmed"), 1);

    /* Idle: the lock runs at once, and the unlock waits its turn. */
    capture_start();
    n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    passes(200);
    capture_end();
    const char *a = strstr(out_buf, "motor: lock unconfirmed");
    const char *b = strstr(out_buf, "motor: unlock unconfirmed");
    expect_int("lock, then unlock", a && b && a < b, 1);
    expect_int("ends off", count("motor: off") >= 2, 1);
    const char *last_off = NULL;
    for (const char *p = out_buf; (p = strstr(p, "motor: ")); p += 7)
        last_off = p;
    expect_int("…and stays off", last_off && !strncmp(last_off, "motor: off", 10), 1);

    /* The move reached the lock (oz_lock_actuated), not only the log. */
    capture_start();
    mock_msh_run("oz.state");
    capture_end();
    expect_int("oz.state: unlocked, last move unconfirmed",
               count("oz.state: unlocked, last move unconfirmed"), 1);
}

/* ── the battery (P5-F), on the mocked SAR ADC ────────────────────────── */

/* Placeholder scaling (oz_port_ak1037.c): mV = raw × 2400 × 3 / 4095. */
/* BATT-CAL-1 (chip 2026-11-19): pack 6340 mV (multimeter) read raw 2350 (mean of
 * 13), so the pack full scale (VREF x DIV) is 11049 mV. */
#define RAW(mv) ((int)((mv) * 4095u / 11049u) + 1)

static void battery_reads(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    passes(200);                        /* motor idle */
    capture_start();
    mock_adc_raw = RAW(4490);
    mock_ms += 10u * 60u * 1000u;       /* the next rest reading is due */
    passes(2);
    size_t n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    capture_end();
    /* BATT-AIN-1 (Anyka motor driver schematic V1.0, 2026-11-19): VOL_DET =
     * VIN x 1M/(2M+1M), wired to J8 row 2 right = GPIO39 = AIN5 (GPIO14/AIN1
     * is not on J8). */
    expect_int("ADC on AIN5 (GPIO39, J8 row 2 right)", mock_adc_channel, 1 << 5);
    expect_int("rest reading: critical", count("lock: battery CRITICAL"), 1);
    expect_int("lock refused", response(out, ol, &x, &result) == OZ_COAP_SERVICE_UNAVAILABLE &&
               result == OZ_R_BATTERY_LOW, 1);
    expect_int("…the motor not driven", count("motor: to lock"), 0);

    /* Fresh batteries; then one that sags under the motor. */
    capture_start();
    mock_adc_raw = RAW(5950);
    mock_ms += 10u * 60u * 1000u;
    passes(2);
    capture_end();
    expect_int("new batteries: ok", count("lock: battery ok"), 1);
    capture_start();
    mock_gpio_in[GPIO31] = 0;           /* LOCK-IO-1: door held open, so no auto-relock moves the motor here */
    mock_adc_raw = RAW(3900);           /* what it reads while the motor pulls */
    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    (void)exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    passes(20);
    mock_adc_raw = RAW(5200);           /* and recovers at rest */
    passes(200);
    mock_ms += 10u * 60u * 1000u;
    passes(2);
    capture_end();
    expect_int("the unlock ran", count("motor: unlock unconfirmed"), 1);
    expect_int("the sag made it critical", count("lock: battery CRITICAL"), 1);
    expect_int("recovered at rest: still critical", count("lock: battery ok"), 0);
    mock_gpio_in[GPIO31] = 1;

    /* An ADC error is no reading: still critical, nothing reported. */
    capture_start();
    mock_adc_raw = 0xffff;
    mock_ms += 10u * 60u * 1000u;
    passes(2);
    capture_end();
    expect_int("ADC error: no level change", count("lock: battery"), 0);
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

/* ── F-AK-WDT-1 SAFE mode: the 3rd watchdog reset in a row ───────────── */

/* Before oz.start: two watchdog resets already counted in kv, and this boot
 * is the third. */
static void safe_boot(void)
{
    mock_flash_blank();
    uint8_t two = 2;
    expect_int("count preset", oz_port_kv_set("bg", &two, 1), OZ_OK);
    mock_reset_status = HAL_CORE_WDT_RST;
    expect_int("3rd watchdog reset: SAFE", oz_ak1037_wdt_boot(), 1);
    expect_int("watchdog kept on", mock_wdt_enabled, 1);
}

static void safe_moves(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    passes(200);                        /* claim()'s moves end */
    capture_start();
    size_t n = request(OZ_COAP_POST, "lock/lock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    passes(200);
    mock_msh_run("oz.state");
    capture_end();
    /* F-AK-WDT-2: oz_lock refuses it (5.03 SAFE_MODE), so the port's own
     * guard is never reached. */
    expect_int("lock refused: 5.03 SAFE_MODE",
               response(out, ol, &x, &result) == OZ_COAP_SERVICE_UNAVAILABLE && result == OZ_R_SAFE_MODE, 1);
    expect_int("the motor never drove to lock", count("motor: to lock"), 0);
    expect_int("refusal logged", count("lock: LOCK refused: SAFE MODE"), 1);
    expect_int("state: not fully locked (not a bolt it never threw)", count("oz.state: not fully locked"), 1);

    /* Never lock anyone out: unlocking still drives the motor. */
    capture_start();
    n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    passes(200);
    mock_msh_run("oz.state");
    capture_end();
    expect_int("unlock OK", response(out, ol, &x, &result) == OZ_COAP_CHANGED && result == OZ_R_UNLOCK_OK, 1);
    expect_int("driven to unlock", count("motor: to unlock"), 1);
    expect_int("state: unlocked", count("oz.state: unlocked, last move unconfirmed"), 1);
}

/* ── oz.wdthang (OZ_DEBUG_HOOKS only) ──────────────────────────────────── */

/* The command's name, built at run time so this file never holds it as a
 * string: the release-binary scan (CMake) must find it only where the hook
 * is compiled in. */
static const char *hook_name(void)
{
    static char n[16];
    static volatile const char rev[] = "gnahtdw.zo";
    size_t len = sizeof rev - 1;
    for (size_t i = 0; i < len; i++)
        n[i] = rev[len - 1 - i];
    n[len] = 0;
    return n;
}

/* REL-SHELL-1 (PM 2026-11-20, security blocker): "oz.key" / "oz.card" /
 * "oz.code" inject keypad digits, a card UID or the pairing code from the
 * UART, without the keypad or the card. Built at run time (as hook_name())
 * so the release-binary scan sees them only where the commands exist. */
static const char *inject_name(int which)
{
    static char n[3][16];
    static volatile const char rev[3][8] = { "yek.zo", "drac.zo", "edoc.zo" };
    size_t len = strlen((const char *)rev[which]);
    for (size_t i = 0; i < len; i++)
        n[which][i] = rev[which][len - 1 - i];
    n[which][len] = 0;
    return n[which];
}

/* Release and every default build: the shell has no such command. */
/* "oz.wipe", built at run time for the same reason as hook_name(). */
/* "oz.led", built backwards so this file's release binary never holds it. */
static const char *led_name(void)
{
    static char n[16];
    static volatile const char rev[] = "del.zo";
    size_t len = sizeof rev - 1;
    for (size_t i = 0; i < len; i++)
        n[i] = rev[len - 1 - i];
    n[len] = 0;
    return n;
}

#ifdef OZ_DEBUG_HOOKS
/* LED-MAP-1 (bench): one J7 pin at a time, high / low / released, so the
 * operator can tell which key lights (each pin drives a pair of key LEDs). */
static void led_map(void)
{
    char cmd[32];
    capture_start();
    expect_int("led: the command exists", mock_msh_run(led_name()), 0);
    capture_end();
    expect_int("…without arguments it explains", count("usage:"), 1);
    snprintf(cmd, sizeof cmd, "%s 3 h", led_name());
    (void)mock_msh_run(cmd);
    expect_int("pin 3 (GPIO26) driven high", mock_gpio_dir[26] == GPIO_DIR_OUTPUT && mock_gpio_level[26] == 1, 1);
    snprintf(cmd, sizeof cmd, "%s 3 l", led_name());
    (void)mock_msh_run(cmd);
    expect_int("…low", mock_gpio_dir[26] == GPIO_DIR_OUTPUT && mock_gpio_level[26] == 0, 1);
    snprintf(cmd, sizeof cmd, "%s 3 off", led_name());
    (void)mock_msh_run(cmd);
    expect_int("…released", mock_gpio_dir[26], GPIO_DIR_INPUT);
    capture_start();
    snprintf(cmd, sizeof cmd, "%s 8 h", led_name());
    (void)mock_msh_run(cmd);
    capture_end();
    expect_int("pin 8 refused", count("usage:"), 1);
}
#endif

static const char *wipe_name(void)
{
    static char n[16];
    static volatile const char rev[] = "epiw.zo";
    size_t len = sizeof rev - 1;
    for (size_t i = 0; i < len; i++)
        n[i] = rev[len - 1 - i];
    n[len] = 0;
    return n;
}

/* AUTOSTART-1: a lock starts itself at power-on (the operator took a board
 * waiting for oz.start for a dead one). oz_ak1037_app_poll() runs from the
 * main loop with the ms tick; the lock starts once, OZ_AUTOSTART_MS in. */
void oz_ak1037_app_poll(uint32_t now_ms);
static void autostart(void)
{
    mock_flash_blank();
    mock_on_notify = on_notify;
    capture_start();
    oz_ak1037_app_poll(1000);
    oz_ak1037_app_poll(2999);
    capture_end();
    expect_int("not before 3 s", mock_ble_inits == 0 && count("automatic start") == 0, 1);
    capture_start();
    oz_ak1037_app_poll(3000);
    capture_end();
    expect_int("at 3 s: started by itself", count("oz.start: automatic start") == 1 && mock_ble_inits == 1, 1);
    expect_int("…an UNCLAIMED lock", count("UNCLAIMED"), 1);
    if (mock_ble_inits != 1)
        return;                         /* no thread to wait for */
    mock_wait_parked();
    capture_start();
    oz_ak1037_app_poll(4000);
    oz_ak1037_app_poll(100000);
    capture_end();
    expect_int("once only", count("automatic start") == 0 && mock_ble_inits == 1, 1);
    capture_start();
    (void)mock_msh_run("oz.start");
    capture_end();
    expect_int("a typed oz.start is harmless", count("already running"), 1);
}

static const char *batt_name(void)
{
    static char n[16];
    static volatile const char rev[] = "ttab.zo";
    size_t len = sizeof rev - 1;
    for (size_t i = 0; i < len; i++)
        n[i] = rev[len - 1 - i];
    n[len] = 0;
    return n;
}

#ifdef OZ_DEBUG_HOOKS
/* BATT-SENSE-1: the bench (debug) build has battery sensing OFF: the EVB's
 * vol_check is not wired, and a floating ADC read as "CRITICAL, 0%", which
 * refused app LOCK moves. oz.batt prints the raw ADC to measure the divider. */

static void batt_bench(void)
{
    capture_start();
    boot();
    capture_end();
    expect_int("boot says sensing is off (bench build)", count("battery: sense OFF"), 1);
    capture_start();
    mock_adc_raw = 12;                  /* a floating pin */
    mock_ms += 10u * 60u * 1000u;
    passes(3);
    capture_end();
    expect_int("no CRITICAL from an unwired pin", count("lock: battery CRITICAL"), 0);
    capture_start();
    mock_adc_raw = 2047;
    expect_int("oz.batt exists", mock_msh_run(batt_name()), 0);
    capture_end();
    expect_int("…prints the raw reading", count("oz.batt: raw 2047"), 1);
    expect_int("…on AIN5", mock_adc_channel, 1 << 5);
    expect_int("…says it is calibrated at one point", count("1-point cal 6340 mV"), 1);
    capture_start();
    mock_adc_raw = 2350;                /* the chip's mean at the multimeter's 6.34 V */
    expect_int("oz.batt again", mock_msh_run(batt_name()), 0);
    capture_end();
    expect_int("…raw 2350 is the 6.34 V pack (calibration point)", count("pack 6340 mV"), 1);
}
#endif


/* "oz.rfidstat" and "oz.flashlat", built backwards like the others so the
 * release binary scan only finds them where they are compiled in. */
static const char *rev_name(volatile const char *rev, size_t len, char *n)
{
    for (size_t i = 0; i < len; i++)
        n[i] = rev[len - 1 - i];
    n[len] = 0;
    return n;
}
static const char *rfidstat_name(void)
{
    static char n[16];
    static volatile const char rev[] = "tatsdifr.zo";
    return rev_name(rev, sizeof rev - 1, n);
}
static const char *flashlat_name(void)
{
    static char n[16];
    static volatile const char rev[] = "talhsalf.zo";
    return rev_name(rev, sizeof rev - 1, n);
}

#ifdef OZ_DEBUG_HOOKS
/* RFID-TIME-1 (PM GO 2026-11-19): the chip showed card polls of 243 ms and
 * 1006 ms (Anyka Q12). Debug builds log every poll of 100 ms or more with its
 * time, and oz.rfidstat prints a histogram of all polls (reset clears it). */
static void rfidstat(void)
{
    char cmd[40];
    boot();
    mock_rfid_cost_ms = 30;
    capture_start();
    idle(3000);
    mock_rfid_cost_ms = 300;
    idle(600);
    mock_rfid_cost_ms = 30;
    idle(600);
    capture_end();
    expect_int("a 300 ms poll is logged with its time", count("rfid: slow poll 300 ms @"), 1);
    expect_int("…a 30 ms poll is not", count("rfid: slow poll 30 ms"), 0);
    capture_start();
    expect_int("oz.rfidstat exists", mock_msh_run(rfidstat_name()), 0);
    capture_end();
    expect_int("…prints the slowest poll", count("max=300 ms"), 1);
    expect_int("…counts the 30 ms polls under 50", count("<50:0 "), 0);
    expect_int("…and the 300 ms poll in 250-499", count("250-499:1 "), 1);
    /* RFID-THREAD-1: the OZKIE thread's own work per pass, for the chip run:
     * with the reads on the reader thread, even 300 ms reads leave no OZKIE
     * pass of 50 ms or more. */
    expect_int("…and the OZKIE pass times", count("ozkie pass: n="), 1);
    expect_int("…no OZKIE pass >= 50 ms with 300 ms reads", count(" over50=0 "), 1);
    snprintf(cmd, sizeof cmd, "%s reset", rfidstat_name());
    capture_start();
    expect_int("oz.rfidstat reset", mock_msh_run(cmd), 0);
    expect_int("…then empty", mock_msh_run(rfidstat_name()), 0);
    capture_end();
    expect_int("…n=0 after reset (reset prints the cleared table, then the plain call)", count("oz.rfidstat: n=0 "), 2);
    expect_int("…the OZKIE pass table too", count("ozkie pass: n=0 "), 2);
    mock_rfid_cost_ms = 0;
}

/* AK-FLASH-LAT-1 (PM GO 2026-11-19): a card miss once took ~1.85 s on the
 * chip, likely the limit-record write. oz.flashlat N times N kv writes of the
 * size of a limit record on the OZKIE thread (the only kv writer), erases the
 * test key, and prints min/mean/max for the writes. */
static void flashlat(void)
{
    char cmd[40];
    uint8_t b[64];                      /* larger than the 16-byte test value */
    size_t l;
    boot();
    claim();
    mock_flash_prog_cost_ms = 20;
    snprintf(cmd, sizeof cmd, "%s 5", flashlat_name());
    capture_start();
    expect_int("oz.flashlat exists", mock_msh_run(cmd), 0);
    passes(3);
    capture_end();
    expect_int("…5 writes timed", count("oz.flashlat: 5 writes, set min="), 1);
    expect_int("…the 20 ms page write shows", count("max=0 ms"), 0);
    expect_int("…nothing failed", count("FAILED"), 0);
    expect_int("…the test key is gone", oz_kv_lfs_get("dbgflat", b, sizeof b, &l) == OZ_OK, 0);
    snprintf(cmd, sizeof cmd, "%s 999", flashlat_name());
    capture_start();
    (void)mock_msh_run(cmd);
    passes(3);
    capture_end();
    expect_int("…a large N is capped at 50", count("oz.flashlat: 50 writes, set min="), 1);
    mock_flash_prog_cost_ms = 0;
    {
        uint8_t out[600], req[300];
        oz_oscore_xchg x;
        int result = -1;
        passes(200);                    /* motor idle */
        size_t n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
        size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
        expect_int("…the claimed lock still unlocks after", response(out, ol, &x, &result) == OZ_COAP_CHANGED &&
                   result == OZ_R_UNLOCK_OK, 1);
    }
}
#endif

/* BOARD-PROFILE-1: one record per lock model; a record oz_board_valid()
 * rejects never touches the motor pins (LOCK-MODELS-1 §1, PM sign-off). */
static void board_profile(void)
{
    const oz_board *evb = oz_ak1037_board();
    expect_int("board: the EVB is the active board", evb == &OZ_BOARD_EVB_AK1037D, 1);
    expect_int("board: its model is ak1037-evb", evb->model && !strcmp(evb->model, "ak1037-evb"), 1);
    expect_int("board: the EVB record is valid", oz_board_valid(evb), OZ_BOARD_OK);
    expect_int("board: NULL refused", oz_board_valid(NULL), OZ_BOARD_E_NULL);

    static oz_board b;
#define BAD(what, change, why) do { b = *evb; change; \
        expect_int("board: refused: " what, oz_board_valid(&b), why); } while (0)
    BAD("no model", b.model = NULL, OZ_BOARD_E_MODEL);
    BAD("an empty model", b.model = "", OZ_BOARD_E_MODEL);
    BAD("both motor wires on one pin", b.motor_b = b.motor_a, OZ_BOARD_E_MOTOR_PINS);
    BAD("a motor wire on a switch input", b.motor_a = b.sw_pin[OZ_DIO_SW_DOOR], OZ_BOARD_E_MOTOR_PINS);
    BAD("a drive time past the hard cap", b.motor.drive_ms = OZ_MOTOR_MAX_DRIVE_MS + 1, OZ_BOARD_E_MOTOR_TIME);
    BAD("a zero drive time", b.motor.drive_ms = 0, OZ_BOARD_E_MOTOR_TIME);
    BAD("a sensor timeout past the hard cap", b.motor.timeout_ms = OZ_MOTOR_MAX_DRIVE_MS + 1, OZ_BOARD_E_MOTOR_TIME);
    BAD("a zero divider", b.batt_div = 0, OZ_BOARD_E_BATT);
    BAD("an absurd battery full scale", b.batt_vref_mv = 50, OZ_BOARD_E_BATT);
    BAD("two switches on one pin", b.sw_pin[OZ_DIO_SW_EXIT] = b.sw_pin[OZ_DIO_SW_DOOR], OZ_BOARD_E_SW_PINS);
    BAD("a backlight index past the pins", b.bl_key[3].pin = OZ_BOARD_BL_PINS, OZ_BOARD_E_BL);
#undef BAD

    /* A corrupt record: the motor pins are never configured or driven. */
    b = *evb;
    b.motor_b = b.motor_a;
    oz_ak1037_board_set(&b);
    capture_start();
    oz_ak1037_motor_pins_init();
    oz_ak1037_motor_pins(1, 0);
    oz_ak1037_motor_pins(0, 1);
    capture_end();
    expect_int("board: invalid -> motor pins never set up", mock_gpio_dir[17] == -1 && mock_gpio_dir[18] == -1, 1);
    expect_int("board: invalid -> motor pins never driven", mock_gpio_level[17] == 0 && mock_gpio_level[18] == 0, 1);
    expect_int("board: invalid -> said once, with the reason", count("board: INVALID profile (3), motor refused"), 1);
    oz_ak1037_board_set(evb);
    oz_ak1037_motor_pins_init();
    oz_ak1037_motor_pins(1, 0);
    expect_int("board: valid again -> A drives", mock_gpio_dir[17] == GPIO_DIR_OUTPUT && mock_gpio_level[17] == 1, 1);
    oz_ak1037_motor_pins(0, 0);
}

static void no_hooks(void)
{
    expect_int("no debug hook in the shell", mock_msh_run(hook_name()), -1);
    expect_int("no wipe in the shell", mock_msh_run(wipe_name()), -1);
    expect_int("no led map in the shell", mock_msh_run(led_name()), -1);
    expect_int("no battery probe in the shell", mock_msh_run(batt_name()), -1);
    expect_int("no rfid stats in the shell", mock_msh_run(rfidstat_name()), -1);
    expect_int("no flash timer in the shell", mock_msh_run(flashlat_name()), -1);
    expect_int("no keypad injection in the shell", mock_msh_run(inject_name(0)), -1);
    expect_int("no card injection in the shell", mock_msh_run(inject_name(1)), -1);
    expect_int("no pairing-code injection in the shell", mock_msh_run(inject_name(2)), -1);
}

#ifdef OZ_DEBUG_HOOKS
/* REL-SHELL-1 positive control: the bench (_dbg) images keep the three
 * commands (blelink.py types the pairing code and PINs with them). */
static void inject_cmds(void)
{
    char cmd[64];
    boot();
    capture_start();
    for (int i = 0; i < 3; i++)
        expect_int("debug: the injection command exists", mock_msh_run(inject_name(i)), 0);
    capture_end();
    expect_int("…each explains its usage without an argument", count("usage:"), 3);
    capture_start();
    snprintf(cmd, sizeof cmd, "%s 0000#", inject_name(0));
    mock_msh_run(cmd);
    for (int i = 0; i < 5; i++)
        mock_pass();
    capture_end();
    expect_int("debug: typed digits reach the keypad", count("keypad: wrong"), 1);
    capture_start();
    mock_ms += 2000;
    snprintf(cmd, sizeof cmd, "%s 04a1b2c3", inject_name(1));
    mock_msh_run(cmd);
    mock_pass();
    snprintf(cmd, sizeof cmd, "%s 04a", inject_name(1));                 /* odd length */
    mock_msh_run(cmd);
    snprintf(cmd, sizeof cmd, "%s 0g", inject_name(1));                  /* not hex */
    mock_msh_run(cmd);
    snprintf(cmd, sizeof cmd, "%s 00112233445566778899aabbccddeeff00", inject_name(1));   /* 17 bytes */
    mock_msh_run(cmd);
    capture_end();
    expect_int("debug: a typed card reaches the reader", count("card: wrong"), 1);
    expect_int("debug: bad ids refused by the shell", count("usage:"), 3);
}
#endif

#ifdef OZ_DEBUG_HOOKS
/* OZ-WIPE-1 (PM GO 2026-11-18): debug builds only, serial shell only. Erases
 * the lock's kv (identity, bonds) BEFORE oz.start, so the next oz.start is a
 * fresh UNCLAIMED lock; refused while the lock runs (no race with its writes). */
static void wipe(void)
{
    char cmd[32];
    uint8_t v = 7, got[4];
    size_t gl = 0;
    mock_flash_blank();
    expect_int("wipe: a key before oz.start", oz_port_kv_set("wt", &v, 1), OZ_OK);
    capture_start();
    expect_int("wipe: the command exists", mock_msh_run(wipe_name()), 0);
    capture_end();
    expect_int("wipe: without CONFIRM it only explains", count("usage:"), 1);
    expect_int("…and erases nothing", oz_port_kv_get("wt", got, sizeof got, &gl), OZ_OK);
    snprintf(cmd, sizeof cmd, "%s CONFIRM", wipe_name());
    capture_start();
    expect_int("wipe CONFIRM runs", mock_msh_run(cmd), 0);
    capture_end();
    expect_int("…says it erased", count("erased"), 1);
    expect_int("…the key is gone", oz_port_kv_get("wt", got, sizeof got, &gl), OZ_E_NOT_FOUND);
    keep_flash = 1;
    boot();                             /* oz.start after the wipe: "unclaimed" is checked there */
    claim();
    /* AUTOSTART-1: the lock now runs from power-on, so the bench wipe works
     * while it runs: the OZKIE thread (the only kv writer) erases, then the
     * chip resets, so nothing races and RAM never outlives the kv. */
    expect_int("a key again", oz_port_kv_set("wt", &v, 1), OZ_OK);
    capture_start();
    expect_int("wipe while running: runs", mock_msh_run(cmd), 0);
    mock_pass();
    capture_end();
    expect_int("…erased by the lock thread", oz_port_kv_get("wt", got, sizeof got, &gl), OZ_E_NOT_FOUND);
    expect_int("…then the chip resets", mock_chip_resets, 1);
    expect_int("…and says so", count("oz.wipe: identity and bonds erased; restarting"), 1);
}

/* Debug build: the command stops the OZKIE thread's check-ins, so the main
 * loop stops feeding and the watchdog would reset the lock. */
static void wdthang(void)
{
    mock_flash_blank();
    mock_reset_status = HAL_AO_POR_RST;
    expect_int("power-on boot: NORMAL", oz_ak1037_wdt_boot(), 0);
    keep_flash = 1;
    boot();
    long f0 = mock_wdt_feeds;
    for (int i = 0; i < 10; i++) {
        passes(1);
        oz_ak1037_wdt_service();
    }
    expect_int("fed while the OZKIE thread checks in", mock_wdt_feeds - f0 > 0, 1);
    capture_start();
    expect_int("the debug hook exists", mock_msh_run(hook_name()), 0);
    capture_end();
    expect_int("…and says what it does", count("stops checking in"), 1);
    long f1 = mock_wdt_feeds;
    for (int i = 0; i < 60; i++) {      /* the thread runs on: about 6 s of passes */
        passes(1);
        oz_ak1037_wdt_service();
    }
    expect_int("a few feeds until the thread is 2 s late, then none",
               mock_wdt_feeds - f1 > 0 && mock_wdt_feeds - f1 < 30, 1);
    long f2 = mock_wdt_feeds;
    for (int i = 0; i < 60; i++) {
        passes(1);
        oz_ak1037_wdt_service();
    }
    expect_int("…no feeds at all: the watchdog will reset the lock", mock_wdt_feeds - f2, 0);
}
#endif

/* LOCK-ID-1 (chip, 2026-11-18): oz.start read the BLE address before the
 * stack had set it and named the lock ozk-000000000000, which every unit
 * would share. An all-zero address is never an ID: no lock, no advertising,
 * and oz.start works once the address is there. */
static void zero_addr(void)
{
    mock_flash_blank();
    mock_ms = 5000;
    mock_on_notify = on_notify;
    mock_dev_addr_stays_zero = 1;
    expect_int("zero address: oz.start runs", mock_msh_run("oz.start"), 0);
    expect_int("zero address: the stack is up", mock_ble_inits, 1);
    expect_int("zero address: nothing advertised", mock_adv_applies, 0);
    mock_dev_addr_stays_zero = 0;
    expect_int("address set: oz.start again", mock_msh_run("oz.start"), 0);
    expect_int("address set: still one BLE init", mock_ble_inits, 1);
    mock_wait_parked();
    expect_int("address set: info read", read_info() > 0, 1);
    expect("address set: device id from the BLE address", (const uint8_t *)lock_id, strlen(lock_id),
           "6f7a6b2d" "6163626565363361636162 38");     /* ozk-acbee63acab8 */
}

/* RFID-READER-1: the SDK's on-chip reader feeds the card path. Polled every
 * 250 ms from the lock thread (not while the motor moves); a card held on the
 * antenna is one check; a slow poll is logged with its time. */
/* BEEP-1 (operator): every key press and every card read beeps (an 80 ms,
 * 2 kHz tone on the speaker DAC, amplifier GPIOAO5 on) and flashes the
 * backlight; the DAC is closed again by itself. */
static void beeps(void)
{
    static const uint8_t uid[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint32_t t0 = mock_ms;
    while (mock_ms - t0 < 1000)
        mock_pass();
    expect_int("speaker amplifier on (GPIOAO5 high)", mock_gpio_dir[GPIOAO5] == GPIO_DIR_OUTPUT && mock_gpio_level[GPIOAO5] == 1, 1);
    expect_int("DAC closed at rest", mock_dac_on, 0);
    int p0 = mock_dac_plays;
    mock_tk_press(1 << 14);
    mock_pass();
    expect_int("a key press plays one tone", mock_dac_plays - p0, 1);
    expect_int("…80 ms at 16 kHz, 16-bit (2560 bytes)", mock_dac_rate == 16000 && mock_dac_len == 2560, 1);
    t0 = mock_ms;
    while (mock_ms - t0 < 300)
        mock_pass();
    expect_int("…and the DAC is closed again by itself", mock_dac_on, 0);

    p0 = mock_dac_plays;
    mock_rfid_card(uid, 4);
    int lit = 0;
    t0 = mock_ms;
    while (mock_ms - t0 < 300) {
        mock_pass();
        lit |= mock_gpio_dir[22] == GPIO_DIR_OUTPUT;
    }
    expect_int("a card read flashes the backlight", lit, 1);
    mock_rfid_card(NULL, 0);
    t0 = mock_ms;
    while (mock_ms - t0 < 1000)
        mock_pass();
    expect_int("a card read: a tone, then the wrong pattern (3 tones)", mock_dac_plays - p0, 4);
    /* BEEP-RFID-1: a tone the DAC refuses is logged, not lost silently. */
    capture_start();
    mock_dac_open_fail = 1;
    mock_tk_press(1 << 14);
    mock_pass();
    mock_pass();
    mock_dac_open_fail = 0;
    idle(1000);
    capture_end();
    expect_int("a refused DAC open is logged", count("beep: start failed") >= 1, 1);
    expect_int("every DAC call was one the SDK accepts", mock_dac_bad, 0);
    expect_int("amplifier callback registered (no 'pa ctrl callback is empty!')", mock_dac_pa_missing, 0);
}

/* LOCK-IO-1: the bench's three momentary switches, active LOW with pull-ups:
 * GPIO30 tamper, GPIO31 door (pressed = open on the bench), AOGPIO9 exit. */
static void hold(int pin, uint32_t ms)
{
    mock_gpio_in[pin] = 0;
    uint32_t t0 = mock_ms;
    while (mock_ms - t0 < ms)
        mock_pass();
    mock_gpio_in[pin] = 1;
}

static void idle(uint32_t ms)
{
    uint32_t t0 = mock_ms;
    while (mock_ms - t0 < ms)
        mock_pass();
}

static void door_io(void)
{
    int in = 1;
    static const int pins[3] = { GPIO30, GPIO31, GPIOAO9 };
    for (int i = 0; i < 3; i++)
        in &= mock_gpio_dir[pins[i]] == GPIO_DIR_INPUT && mock_gpio_pullup[pins[i]];
    expect_int("switch pins are inputs with pull-ups", in, 1);
    idle(2000);

    capture_start();
    hold(GPIOAO9, 300);                 /* exit */
    idle(1500);
    capture_end();
    expect_int("exit button: unlock", count("lock: exit button") == 1 && count("motor: to unlock @") == 1, 1);

    capture_start();
    hold(GPIO31, 1000);                 /* the door opens, then closes */
    idle(4000);
    capture_end();
    expect_int("door opened and closed", count("door: opened") == 1 && count("door: closed") == 1, 1);
    expect_int("…with the lock's ms tick", count("door: opened @") == 1 && count("door: closed @") == 1, 1);
    expect_int("…no relock within 4 s of closing", count("lock: auto-relock"), 0);
    capture_start();
    idle(2500);
    capture_end();
    expect_int("…auto-relock 5 s after closing", count("lock: auto-relock") == 1 && count("motor: to lock @") == 1, 1);

    capture_start();
    int p0 = mock_dac_plays;
    hold(GPIO30, 300);                  /* tamper */
    idle(3000);
    capture_end();
    expect_int("tamper: recorded", count("lock: TAMPER"), 1);
    expect_int("…the alarm sounds (many tones in 3 s)", mock_dac_plays - p0 >= 8, 1);
    capture_start();
    hold(GPIOAO9, 300);                 /* PM: exit during the alarm */
    idle(1500);
    capture_end();
    expect_int("exit during the tamper alarm unlocks (PM)", count("lock: exit button") == 1 && count("motor: to unlock @") == 1, 1);
    idle(20000);                        /* alarm over, relocked (unopened) */

    /* BEEP-RFID-1 (chip 2026-11-19): 2 of the 5 left-open tones heard, and an
     * RFID poll took 243 ms. A poll while a tone plays keeps it on; the next
     * one starts at once, and the tones run together. Polls here cost 243 ms. */
    capture_start();
    mock_rfid_cost_ms = 243;
    mock_dac_timing_reset();
    int p1 = mock_dac_plays;
    hold(GPIO31, 32000);                /* the door left open (32 s: the tones end) */
    mock_rfid_cost_ms = 0;
    capture_end();
    expect_int("door left open 30 s: warned", count("lock: door left open"), 1);
    expect_int("…and never relocked while open (PM)", count("lock: auto-relock"), 0);
    expect_int("…5 tones", mock_dac_plays - p1, 5);
    expect_int("…each tone at most 100 ms with 243 ms RFID polls (BEEP-RFID-1)", mock_dac_longest_ms <= 100, 1);
    expect_int("…at least 100 ms of silence between tones (BEEP-RFID-1)", mock_dac_shortest_gap_ms >= 100, 1);
    idle(20000);
}

/* RELAY-AK-1: the `relay` characteristic (ozkey-48 rev 6 §2.3), so a bridge
 * (or, while none is enrolled, the owner) can reach the lock through it. The
 * AK1037 SDK keeps ONE connection, so the relay owner is that connection. */
static void relay_link(void)
{
    uint8_t out[600], req[300];
    oz_oscore_xchg x;
    int result;

    mock_ble_event(LINK, eBT_MSG_LE_CONNECTION_SUCC, 0);
    mock_pass();
    mock_ble_event(LINK, eBT_MSG_LE_MTU_EXCHANGED, 247);
    mock_pass();
    mtu = 247;

    /* No bridge enrolled: the rev 6 rule lets the connection take `relay`
     * (oz_lock_relay_may_own; once a bridge is enrolled only it may, which
     * test_lock covers). The owner talks to the lock through it. */
    cur_rec = REC_OZKIE;
    cur_idx = 0;
    expect_int("owner session on ozkie", handshake(), 0);
    capture_start();
    cur_rec = REC_RELAY;
    cur_idx = 1;
    expect_int("EDHOC on the relay link, answered on notify index 1", handshake(), 0);
    size_t n = request(OZ_COAP_POST, "lock/unlock", req, sizeof req, &x);
    size_t ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    /* The relay link's first request: 4.01 + Echo, and nothing done (B.1.2). */
    expect_int("first relay request: 4.01 + Echo", response(out, ol, &x, &result) == OZ_COAP_UNAUTHORIZED && has_echo, 1);
    n = request_echo(OZ_COAP_POST, "lock/unlock", last_echo, req, sizeof req, &x);
    ol = exchange(OZ_FRAME_OSCORE, req, n, out, sizeof out);
    expect_int("unlock through the relay, with the Echo", response(out, ol, &x, &result) == OZ_COAP_CHANGED && result == OZ_R_UNLOCK_OK, 1);
    idle(1500);
    capture_end();
    expect_int("…the relay owned once", count("gatt: relay owned"), 1);
    expect_int("…the motor turned", count("motor: to unlock @"), 1);
    expect_int("every notify on a notify characteristic", mock_notify_bad_idx, 0);

    capture_start();
    mock_ble_event(LINK, eBT_MSG_LE_LINK_DISCONNECTED, 0);
    mock_pass();
    capture_end();
    expect_int("disconnect releases the relay", count("gatt: relay released"), 1);
    cur_rec = REC_OZKIE;
    cur_idx = 0;
    mtu = 23;
    idle(20000);                        /* the auto-relock after the unlock */
}

static void rfid_reader(void)
{
    static const uint8_t uid[4] = { 0xde, 0xad, 0xbe, 0xef };
    mock_ms += 31u * 60u * 1000u;       /* past any card lockout from above */
    expect_int("reader started once", mock_rfid_inited, 1);
    mock_pass();
    int r0 = mock_rfid_reads;
    uint32_t t0 = mock_ms;
    while (mock_ms - t0 < 1000)
        mock_pass();
    int polls = mock_rfid_reads - r0;
    expect_int("polled about every 250 ms (3-5 in 1 s)", polls >= 3 && polls <= 5, 1);

    capture_start();
    mock_rfid_card(uid, 4);
    t0 = mock_ms;
    while (mock_ms - t0 < 2000)
        mock_pass();                    /* held 2 s on the antenna */
    mock_rfid_card(NULL, 0);
    capture_end();
    expect_int("a card held 2 s: one check", count("card: wrong") + count("card: locked out"), 1);
    expect_int("…logged without its id (release log, PM)", count("rfid: card read (4 bytes)"), 1);
    expect_int("…the full id is not in the log", count("deadbeef"), 0);

    capture_start();
    mock_rfid_cost_ms = 30;
    t0 = mock_ms;
    while (mock_ms - t0 < 600)
        mock_pass();
    mock_rfid_cost_ms = 0;
    capture_end();
    expect_int("a 30 ms poll is logged once (new maximum)", count("rfid: slowest poll 30 ms"), 1);
    /* RFID-THREAD-1 (Anyka Q6: no non-blocking read; read on a lower-priority
     * thread): a read blocks only the reader, never the OZKIE thread. */
    expect_int("rfid: a reader thread \"ozrfid\" exists", mock_thread_priority("ozrfid") >= 0, 1);
    expect_int("rfid: …below the OZKIE thread's priority",
               mock_thread_priority("ozrfid") > mock_thread_priority("ozkie"), 1);
    expect_int("rfid: …above idle", mock_thread_priority("ozrfid") < RT_THREAD_PRIORITY_MAX - 1, 1);
    expect_int("rfid: the OZKIE thread never reads the antenna", mock_reads_off_reader, 0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "zeroaddr")) {
        zero_addr();
        return report("ak1037_gatt_zeroaddr");
    }
    if (argc > 1 && !strcmp(argv[1], "board")) {
        board_profile();
        return report("ak1037_gatt_board");
    }
    if (argc > 1 && !strcmp(argv[1], "nohooks")) {
        no_hooks();
        return report("ak1037_gatt_nohooks");
    }
    if (argc > 1 && !strcmp(argv[1], "autostart")) {
        autostart();
        return report("ak1037_gatt_autostart");
    }
    if (argc > 1 && !strcmp(argv[1], "batt")) {
#ifdef OZ_DEBUG_HOOKS
        batt_bench();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_batt");
    }
    if (argc > 1 && !strcmp(argv[1], "rfidstat")) {
#ifdef OZ_DEBUG_HOOKS
        rfidstat();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_rfidstat");
    }
    if (argc > 1 && !strcmp(argv[1], "flashlat")) {
#ifdef OZ_DEBUG_HOOKS
        flashlat();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_flashlat");
    }
    if (argc > 1 && !strcmp(argv[1], "led")) {
#ifdef OZ_DEBUG_HOOKS
        led_map();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_led");
    }
    if (argc > 1 && !strcmp(argv[1], "wipe")) {
#ifdef OZ_DEBUG_HOOKS
        wipe();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_wipe");
    }
    if (argc > 1 && !strcmp(argv[1], "inject")) {
#ifdef OZ_DEBUG_HOOKS
        inject_cmds();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_inject");
    }
    if (argc > 1 && !strcmp(argv[1], "wdthang")) {
#ifdef OZ_DEBUG_HOOKS
        wdthang();
#else
        expect_int("built with OZ_DEBUG_HOOKS", 0, 1);
#endif
        return report("ak1037_gatt_wdthang");
    }
    if (argc > 1 && !strcmp(argv[1], "safe")) {
        safe_boot();
        keep_flash = 1;
        boot();
        claim();
        safe_moves();
        return report("ak1037_gatt_safe");
    }
    boot();
    touch_pads();
    claim();
    keypad_lockout();
    cards_at_reader();
    motor_moves();
    battery_reads();
    junk_and_limits();
    disconnect_and_adv();
    rfid_reader();
    beeps();
    door_io();
    relay_link();
    expect_int("every notify went to a notify characteristic (SDK index check)", mock_notify_bad_idx, 0);
    expect_int("flash stayed in OZKV", mock_flash_out_of_part, 0);
    expect_int("NOR rule kept", mock_nor_violations, 0);
    expect_int("SKE only with interrupts off", mock_irq_violations, 0);
    expect_int("the engine did the AES", mock_ske_blocks > 1, 1);
    /* P5-D: the OZKIE thread's deepest stack over everything above (claim,
     * EDHOC, kv writes, keypad lockout), on the host. Release builds hold it
     * to the thread's budget as a tripwire. It covers only the paths run
     * here; the RV32 worst case is the static report (ozkey-49 P5-D). */
    size_t peak = mock_stack_peak("ozkie");
    printf("ozkie thread stack peak (host): %u of %u B\n", (unsigned)peak, (unsigned)OZ_GATT_THREAD_STACK);
#ifdef OZ_STACK_TRIPWIRE
    expect_int("OZKIE stack measured", peak > 0, 1);
    expect_int("OZKIE stack within its budget", peak <= OZ_GATT_THREAD_STACK, 1);
#endif
    /* The OZKIE thread stays parked in rt_mq_recv; exit ends it. */
    return report("ak1037_gatt");
}
