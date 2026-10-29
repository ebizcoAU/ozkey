/* oz_gatt_ak1037 — see oz_gatt_ak1037.h.
 *
 * Follows the pattern of the SDK's Nordic DFU sample (service_dfu.c): a
 * T_BT_ATTREC table with 128-bit UUIDs, recv_cb for writes, read_cb for
 * reads, notify through com_ble_server_notify_ext().
 *
 * UNVERIFIED until the dev kit (P4):
 *   - NOTIFY_REC_IDX: what att_rec_idx counts is undocumented (the DFU sample
 *     passes 0). Anyka question 5, ozkey-49.
 *   - A long info read (over MTU − 1) would need read-blob offsets; info is
 *     well under 23 bytes of overhead, so a single read is assumed.
 *   - Whether the stack restarts advertising by itself after a disconnect.
 *     Harmless either way: the OZKIE thread re-applies its mode on every change.
 *   - Advertising current at OZ_GATT_ADV_INTERVAL (P6).
 *
 * Advertising (oz_adv.h) is driven from the OZKIE thread only: the stack's
 * default advertising is off, and the mode is re-applied whenever it changes.
 */
#include <stddef.h>
#include <string.h>

#include "rtthread.h"
#include "rthw.h"
#include "com_ble.h"

#include "oz_port.h"
#include "oz_adv.h"
#include "oz_ble.h"
#include "oz_keypad.h"
#include "oz_motor.h"
#include "oz_card.h"
#include "oz_gatt_ak1037.h"
#include "oz_port_ak1037.h"

static const T_BT_ATTREC gatt_table[] = {
    { arPrimaryService, arLongUUID, OZ_BLE_UUID_SERVICE },
    { arCharacteristic, arLongUUID | arRead, OZ_BLE_UUID_INFO },
    { arChrValue, 0, AK_NULL },
    { arCharacteristic, arLongUUID | arWrite | arNotify, OZ_BLE_UUID_OZKIE },
    { arChrValue, 0, AK_NULL },
    { arChrClientConfiguration, 0, AK_NULL },
};
#define TABLE_LEN      (sizeof gatt_table / sizeof gatt_table[0])
#define REC_INFO       ((const void *)&gatt_table[2])
#define REC_OZKIE      ((const void *)&gatt_table[4])
#define NOTIFY_REC_IDX 4                /* UNVERIFIED, see above */

/* ── BLE context → OZKIE thread ──────────────────────────────────────────── */

enum { EV_CONNECT = 1, EV_DISCONNECT, EV_MTU, EV_FRAME, EV_PAIR_CODE, EV_TOUCH, EV_KEY, EV_CARD };

#define FRAME_MAX (BLE_MTU_MAX_SIZE - 3)
typedef struct {
    uint8_t  ev;
    uint16_t len;
    uint32_t arg;
} hdr_t;
typedef struct {
    hdr_t   h;
    uint8_t data[FRAME_MAX];
} msg_t;

#define QUEUE_DEPTH 8
#define SLOT_SIZE   ((sizeof(msg_t) + 7u) & ~7u)
static struct rt_messagequeue mq;
static uint8_t mq_pool[QUEUE_DEPTH * (SLOT_SIZE + 8)];

static void *volatile conn;             /* SDK link handle, NULL when idle */

/* Events without data, from any thread: a small message on the caller's stack. */
static void post(uint8_t ev, uint32_t arg)
{
    hdr_t h = { ev, 0, arg };
    if (rt_mq_send(&mq, &h, sizeof h) != RT_EOK)
        oz_port_log("gatt: queue full, event %u dropped", (unsigned)ev);
}

/* A written frame, from the BLE stack's callback only (one at a time there),
 * so one static buffer keeps the 0.5 KB off the stack's own thread. */
static void post_frame(const void *data, uint16_t len)
{
    static msg_t m;
    m.h.ev = EV_FRAME;
    m.h.len = len;
    m.h.arg = 0;
    memcpy(m.data, data, len);
    if (rt_mq_send(&mq, &m, offsetof(msg_t, data) + len) != RT_EOK)
        oz_port_log("gatt: queue full, frame dropped");
}

/* A card's identity, from the reader's thread (or the shell): the same
 * layout as msg_t up to its 16 bytes of data, on the caller's stack. */
static int post_card(const uint8_t *id, size_t len)
{
    struct {
        hdr_t   h;
        uint8_t data[OZ_CARD_ID_MAX];
    } m;
    if (len == 0 || len > OZ_CARD_ID_MAX)
        return OZ_E_ARG;
    m.h.ev = EV_CARD;
    m.h.len = (uint16_t)len;
    m.h.arg = 0;
    memcpy(m.data, id, len);
    if (rt_mq_send(&mq, &m, offsetof(msg_t, data) + len) != RT_EOK) {
        oz_port_log("gatt: queue full, card dropped");
        return OZ_E_HW;
    }
    return OZ_OK;
}

static void on_write(void *handle, T_BT_ATTRECV_CB *info, uint32_t info_len)
{
    (void)handle;
    (void)info_len;
    if (!info || info->p_att_rec != REC_OZKIE)
        return;
    if (info->buf_size == 0 || info->buf_size > FRAME_MAX)
        return;                         /* the reassembler would refuse it anyway */
    post_frame(info->p_buf, info->buf_size);
}

/* `info` is read from the BLE context, so it serves a snapshot the OZKIE
 * thread refreshes; it never calls into the lock. */
static uint8_t info_snap[96];
static size_t info_snap_len;

static void on_read(void *handle, T_BT_ATTRECV_CB *info, uint32_t info_len)
{
    (void)handle;
    (void)info_len;
    if (!info)
        return;
    info->buf_used = 0;
    if (info->p_att_rec != REC_INFO)
        return;
    rt_base_t level = rt_hw_interrupt_disable();
    size_t n = info_snap_len < info->buf_size ? info_snap_len : info->buf_size;
    memcpy(info->p_buf, info_snap, n);
    rt_hw_interrupt_enable(level);
    info->buf_used = (uint16_t)n;
}

static uint16_t on_event(void *handle, T_eBT_MSG msg, int32_t param1, int32_t param2)
{
    (void)param2;
    switch (msg) {
    case eBT_MSG_LE_CONNECTION_SUCC:
        conn = handle;
        com_ble_set_mtu_size(handle, 247);  /* ozkey-48 §2.1: apps negotiate 247 */
        post(EV_CONNECT, 0);
        break;
    case eBT_MSG_LE_MTU_EXCHANGED:          /* param1 = MTU (ba_msg_att.c) */
        post(EV_MTU, (uint32_t)param1);
        break;
    case eBT_MSG_LE_LINK_DISCONNECTED:
        conn = AK_NULL;
        post(EV_DISCONNECT, 0);
        break;
    default:
        break;
    }
    return eBLE_RESULT_SUCCESS;
}

/* ── the OZKIE thread ────────────────────────────────────────────────────── */

static struct rt_thread thread;
static uint8_t thread_stack[OZ_GATT_THREAD_STACK] __attribute__((aligned(8)));
static oz_link link;
static oz_ble session;
static oz_adv adv;
static oz_keypad keypad;
static oz_motor motor;
static oz_card card;

static uint32_t now_ms(void) { return (uint32_t)rt_tick_get() * (1000u / RT_TICK_PER_SECOND); }

/* Stop, set type and both payloads, start. The type can only change while
 * stopped (HCI), and the controller has already stopped a connectable
 * advertisement by itself when a link came up. Returns 0 when applied. */
static int adv_apply(const oz_lock *L, oz_adv_mode mode)
{
    static uint8_t data[OZ_ADV_MAX], rsp[OZ_ADV_MAX];
    static T_BLE_ADDR no_peer;
    com_ble_set_adv_enable(eBLE_DISABLE);
    if (mode == OZ_ADV_OFF)
        return 0;
    T_BT_ADV_DATA d, r;
    memset(&d, 0, sizeof d);
    memset(&r, 0, sizeof r);
    d.custom_data.p_data = data;        /* raw AD structures, copied as they are */
    d.custom_data.data_len = (uint32_t)oz_adv_data(data);
    r.custom_data.p_data = rsp;
    r.custom_data.data_len = (uint32_t)oz_adv_scan_rsp(rsp, mode == OZ_ADV_BUSY, L->device_id);
    if (r.custom_data.data_len == 0) {
        oz_port_log("adv: device_id %s has no short id", L->device_id);
        return -1;
    }
    uint8_t type = mode == OZ_ADV_BUSY ? eBLE_ADV_SCAN_IND : eBLE_ADV_IND;
    if (com_ble_set_adv_interval(type, OZ_GATT_ADV_INTERVAL, OZ_GATT_ADV_INTERVAL, 0, no_peer) != eBLE_RESULT_SUCCESS
        || com_ble_set_adv_data(&d, &r) != eBLE_RESULT_SUCCESS
        || com_ble_set_adv_enable(eBLE_ENABLE) != eBLE_RESULT_SUCCESS)
        return -1;
    oz_port_log("adv: %s", mode == OZ_ADV_BUSY ? "busy (scan_ind)" : "open (ind)");
    return 0;
}

static int notify(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    void *h = conn;
    if (!h)
        return OZ_E_HW;
    /* The stack's send ring can fill on a burst of fragments: wait it out. */
    for (int tries = 0; tries < 50; tries++) {
        T_eBLE_RESULT r = com_ble_server_notify_ext(h, (uint8_t *)frame, (uint16_t)len, NOTIFY_REC_IDX);
        if (r == eBLE_RESULT_SUCCESS)
            return OZ_OK;
        if (r != eBLE_RESULT_BUFFER_FULL)
            return OZ_E_HW;
        rt_thread_mdelay(5);
    }
    return OZ_E_HW;
}

static void refresh_info(const oz_lock *L)
{
    uint8_t buf[sizeof info_snap];
    size_t n = 0;
    if (oz_lock_info(L, buf, sizeof buf, &n))
        return;
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(info_snap, buf, n);
    info_snap_len = n;
    rt_hw_interrupt_enable(level);
}

void oz_gatt_ak1037_pair_prompt(int on) { oz_keypad_pairing(&keypad, on); }

void oz_gatt_ak1037_actuate(int unlock) { oz_motor_command(&motor, unlock, now_ms()); }

/* The motor driver: log-only until OZ-7 gives the driver IC, its pins and a
 * bolt sensor. Nothing is sensed, so every move ends OZ_MOVE_UNCONFIRMED. */
static void motor_drive(uint8_t out)
{
    static const char *const name[] = { "off", "to unlock", "to lock", "brake" };
    oz_port_log("motor: %s", name[out & 3]);
}

/* The battery (P5-F): a reading at rest every OZ_GATT_BATT_MS, and the lowest
 * one each 10 ms step while the motor drives, reported when the move ends. */
#define OZ_GATT_BATT_MS (10u * 60u * 1000u)

static void battery_rest(oz_lock *L)
{
    static uint32_t last;
    static int once;
    if (motor.phase != OZ_MOTOR_IDLE || (once && now_ms() - last < OZ_GATT_BATT_MS))
        return;
    once = 1;
    last = now_ms();
    oz_lock_power_rest(L, oz_ak1037_battery_mv());
}

static void motor_step(oz_lock *L)
{
    static uint8_t driven = OZ_MOTOR_OFF;
    static uint16_t load_min;
    int unlock = 0;
    if (motor.phase == OZ_MOTOR_DRIVING) {
        uint16_t mv = oz_ak1037_battery_mv();
        if (mv && (!load_min || mv < load_min))
            load_min = mv;
    }
    int r = oz_motor_step(&motor, now_ms(), OZ_BOLT_NONE, 0, &unlock);
    if (motor.out != driven) {
        driven = motor.out;
        motor_drive(driven);
    }
    if (r != OZ_MOVE_NONE) {
        oz_lock_power_load(L, load_min);
        load_min = 0;
        oz_lock_actuated(L, unlock, r);
        oz_port_log("motor: %s %s", unlock ? "unlock" : "lock",
                    r == OZ_MOVE_OK ? "ok" : r == OZ_MOVE_JAMMED ? "JAMMED" : "unconfirmed");
    }
}

/* One key, on the OZKIE thread. Results are logged until the panel and buzzer
 * are wired (P5). */
static void card_in(oz_lock *L, const uint8_t *id, size_t len)
{
    if (!oz_card_seen(&card, id, len, now_ms()))
        return;                         /* the same card, still on the reader */
    int r = oz_lock_card(L, id, len);
    if (r == OZ_CARD_OPEN)
        oz_port_log("card: open");
    else if (r == OZ_CARD_LOCKED_OUT)
        oz_port_log("card: locked out, %u s left, not checked",
                    (unsigned)((oz_lock_card_lockout_ms(L) + 999u) / 1000u));
    else
        oz_port_log("card: wrong");
}

static void key_in(oz_lock *L, char key, int connected)
{
    uint32_t code = 0;
    switch (oz_keypad_key(&keypad, key, now_ms(), &code)) {
    case OZ_KP_PIN: {
        int r = oz_lock_keypad_pin(L, keypad.entry);
        oz_keypad_done(&keypad);
        if (r == OZ_PIN_OPEN)
            oz_port_log("keypad: open");
        else if (r == OZ_PIN_LOCKED_OUT)
            oz_port_log("keypad: locked out, %u s left, not checked",
                        (unsigned)((oz_lock_keypad_lockout_ms(L) + 999u) / 1000u));
        else
            oz_port_log("keypad: wrong");
        break;
    }
    case OZ_KP_PAIR_CODE:
        if (connected)
            (void)oz_ble_pair_code(&session, code);
        else
            oz_port_log("keypad: pairing code but no app connected");
        refresh_info(L);
        break;
    case OZ_KP_CLEARED:
        oz_port_log("keypad: cleared");
        break;
    default:
        break;
    }
}

static void ozkie_thread(void *param)
{
    oz_lock *L = param;
    static msg_t m;
    int connected = 0;
    int applied = -1, failed = 0;
    refresh_info(L);
    for (;;) {
        /* 10 ms while the motor moves (oz_motor_step's cadence), else 100. */
        rt_int32_t wait = motor.phase == OZ_MOTOR_IDLE ? 100 : 10;
        if (rt_mq_recv(&mq, &m, sizeof m, (rt_int32_t)rt_tick_from_millisecond(wait)) == RT_EOK) {
            switch (m.h.ev) {
            case EV_CONNECT:
                oz_ble_connect(&session);
                if (!connected)
                    oz_adv_link(&adv, 1);
                connected = 1;
                break;
            case EV_MTU:
                oz_ble_set_mtu(&session, m.h.arg);
                break;
            case EV_FRAME:
                if (connected)
                    (void)oz_ble_rx(&session, m.data, m.h.len);
                refresh_info(L);        /* a claim changes the state field */
                break;
            case EV_PAIR_CODE:
                if (connected)
                    (void)oz_ble_pair_code(&session, m.h.arg);
                refresh_info(L);
                break;
            case EV_DISCONNECT:
                if (connected) {
                    oz_ble_disconnect(&session);
                    oz_adv_link(&adv, 0);
                }
                connected = 0;
                break;
            case EV_TOUCH:
                oz_adv_touch(&adv, now_ms());
                break;
            case EV_KEY:
                key_in(L, (char)m.h.arg, connected);
                break;
            case EV_CARD:
                card_in(L, m.data, m.h.len);
                break;
            default:
                break;
            }
        }
        if (connected)
            (void)oz_ble_tick(&session);
        motor_step(L);
        battery_rest(L);
        if (oz_keypad_tick(&keypad, now_ms()) == OZ_KP_CLEARED)
            oz_port_log("keypad: entry timed out");

        /* Claiming the lock or a window closing changes the mode too, so it is
         * recomputed every pass, not only on events. A failed apply is retried
         * each pass and logged once. */
        oz_adv_mode mode = oz_adv_mode_at(&adv, oz_lock_claimed(L), now_ms());
        if ((int)mode != applied) {
            if (adv_apply(L, mode) == 0) {
                applied = (int)mode;
                failed = 0;
            } else if (!failed) {
                oz_port_log("adv: could not apply mode %d, retrying", (int)mode);
                failed = 1;
            }
        }
    }
}

int oz_gatt_ak1037_pair_code(uint32_t code)
{
    post(EV_PAIR_CODE, code);
    return OZ_OK;
}

int oz_gatt_ak1037_key(char key)
{
    post(EV_KEY, (uint32_t)(uint8_t)key);
    return OZ_OK;
}

int oz_gatt_ak1037_card(const uint8_t *id, size_t len) { return post_card(id, len); }

int oz_gatt_ak1037_touch(void)
{
    post(EV_TOUCH, 0);
    return OZ_OK;
}

int oz_gatt_ak1037_start(oz_lock *L, uint8_t thread_priority)
{
    if (rt_mq_init(&mq, "ozkie", mq_pool, SLOT_SIZE, sizeof mq_pool, RT_IPC_FLAG_FIFO) != RT_EOK)
        return OZ_E_HW;
    oz_ble_init(&session, L, &link, notify, NULL);
    oz_adv_init(&adv, OZ_GATT_ADV_ALWAYS, now_ms());
    oz_keypad_init(&keypad);
    oz_motor_init(&motor, NULL);
    oz_card_init(&card);

    T_BLE_INIT_PARAM init;
    memset(&init, 0, sizeof init);
    init.role_type = eBLE_ROLE_SERVER;
    init.recv_cb = on_write;
    init.read_cb = on_read;
    init.evt_handle_cb = on_event;
    init.adv_def_on = 0;                /* the OZKIE thread decides */
    if (com_ble_init(&init) != eBLE_RESULT_SUCCESS) {
        oz_port_log("gatt: BLE init failed");
        return OZ_E_HW;
    }
    if (com_ble_set_gatt_service((T_BT_ATTREC *)gatt_table, TABLE_LEN, eBLE_ROLE_SERVER) != eBLE_RESULT_SUCCESS) {
        oz_port_log("gatt: service registration failed");
        return OZ_E_HW;
    }
    if (rt_thread_init(&thread, "ozkie", ozkie_thread, L, thread_stack, sizeof thread_stack,
                       thread_priority, 10) != RT_EOK)
        return OZ_E_HW;
    rt_thread_startup(&thread);
    return OZ_OK;
}
