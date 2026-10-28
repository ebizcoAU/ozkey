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
 *   - Advertising per CONTRACT.md (service UUID, busy flag, touch window) is
 *     not done here yet; the stack's default advertising is on.
 */
#include <stddef.h>
#include <string.h>

#include "rtthread.h"
#include "rthw.h"
#include "com_ble.h"

#include "oz_port.h"
#include "oz_ble.h"
#include "oz_gatt_ak1037.h"

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

enum { EV_CONNECT = 1, EV_DISCONNECT, EV_MTU, EV_FRAME, EV_PAIR_CODE };

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

static void ozkie_thread(void *param)
{
    oz_lock *L = param;
    static msg_t m;
    int connected = 0;
    refresh_info(L);
    for (;;) {
        if (rt_mq_recv(&mq, &m, sizeof m, (rt_int32_t)rt_tick_from_millisecond(100)) == RT_EOK) {
            switch (m.h.ev) {
            case EV_CONNECT:
                oz_ble_connect(&session);
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
                if (connected)
                    oz_ble_disconnect(&session);
                connected = 0;
                break;
            default:
                break;
            }
        }
        if (connected)
            (void)oz_ble_tick(&session);
    }
}

int oz_gatt_ak1037_pair_code(uint32_t code)
{
    post(EV_PAIR_CODE, code);
    return OZ_OK;
}

int oz_gatt_ak1037_start(oz_lock *L, uint8_t thread_priority)
{
    if (rt_mq_init(&mq, "ozkie", mq_pool, SLOT_SIZE, sizeof mq_pool, RT_IPC_FLAG_FIFO) != RT_EOK)
        return OZ_E_HW;
    oz_ble_init(&session, L, &link, notify, NULL);

    T_BLE_INIT_PARAM init;
    memset(&init, 0, sizeof init);
    init.role_type = eBLE_ROLE_SERVER;
    init.recv_cb = on_write;
    init.read_cb = on_read;
    init.evt_handle_cb = on_event;
    init.adv_def_on = 1;
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
