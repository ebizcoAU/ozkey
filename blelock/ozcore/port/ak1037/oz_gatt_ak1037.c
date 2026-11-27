/* oz_gatt_ak1037 — see oz_gatt_ak1037.h.
 *
 * Follows the pattern of the SDK's Nordic DFU sample (service_dfu.c): a
 * T_BT_ATTREC table with 128-bit UUIDs, recv_cb for writes, read_cb for
 * reads, notify through com_ble_server_notify_ext().
 *
 * UNVERIFIED until the dev kit (P4):
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
#include <stdio.h>
#include <string.h>

#include "rtthread.h"
#include "rthw.h"
#include "com_ble.h"
#include "dev_tk.h"
#include "hal_touch.h"
#include "hal_rfid.h"
#include "hal_sys.h"
#include "hal_sysclock.h"
#include "oz_kv_lfs.h"

#include "oz_port.h"
#include "oz_adv.h"
#include "oz_ble.h"
#include "oz_keypad.h"
#include "oz_motor.h"
#include "oz_card.h"
#include "oz_doorio.h"
#include "oz_events.h"
#include "oz_gatt_ak1037.h"
#include "oz_board_ak1037.h"
#include "oz_port_ak1037.h"
#include "oz_wdt_ak1037.h"

static const T_BT_ATTREC gatt_table[] = {
    { arPrimaryService, arLongUUID, OZ_BLE_UUID_SERVICE },
    { arCharacteristic, arLongUUID | arRead, OZ_BLE_UUID_INFO },
    { arChrValue, 0, AK_NULL },
    { arCharacteristic, arLongUUID | arWrite | arNotify, OZ_BLE_UUID_OZKIE },
    { arChrValue, 0, AK_NULL },
    { arChrClientConfiguration, 0, AK_NULL },
    /* RELAY-AK-1: `relay` (ozkey-48 rev 6 §2.3), the bridge's link. */
    { arCharacteristic, arLongUUID | arWrite | arNotify, OZ_BLE_UUID_RELAY },
    { arChrValue, 0, AK_NULL },
    { arChrClientConfiguration, 0, AK_NULL },
};
#define TABLE_LEN      (sizeof gatt_table / sizeof gatt_table[0])
#define REC_INFO       ((const void *)&gatt_table[2])
#define REC_OZKIE      ((const void *)&gatt_table[4])
#define REC_RELAY      ((const void *)&gatt_table[7])
/* The SDK counts only the notify-capable characteristics, from 0
 * (ble_gatt.c ble_gatt_usersrv_is_att_idx_valid); OZKIE is the only one. 4 was
 * a guess: on the chip every notify failed "send_att_rec_id overflow 4"
 * (NOTIFY-IDX-1, 2026-11-18). */
#define NOTIFY_REC_IDX 0
#define NOTIFY_RELAY_IDX 1              /* the second notify characteristic (RELAY-AK-1) */
enum { CH_OZKIE = 0, CH_RELAY = 1 };

/* ── BLE context → OZKIE thread ──────────────────────────────────────────── */

enum { EV_CONNECT = 1, EV_DISCONNECT, EV_MTU, EV_FRAME, EV_PAIR_CODE, EV_TOUCH, EV_KEY, EV_CARD, EV_PAD };
static char pad_char(int pad);          /* touch pad bit -> key (TOUCH-MAP-1) */
static void rfid_thread_start(uint8_t prio);   /* the on-chip RFID reader's own thread (RFID-THREAD-1) */
static volatile uint8_t rfid_hold = 1; /* the OZKIE thread: no read now (motor moving, tones) */
#ifdef OZ_DEBUG_HOOKS
/* RFID-THREAD-1: the OZKIE thread's own work per pass (after its queue wait),
 * to show on the chip that card reads no longer stall it (oz.rfidstat). */
static volatile uint32_t ozp_n, ozp_max, ozp_over50;
#endif
static void tones(int n, uint32_t gap_ms);  /* queue n beeps (LOCK-IO-1 patterns) */

/* The door's own inputs (LOCK-IO-1). Policy numbers: the board's doorio
 * (BOARD-PROFILE-1; the EVB: OZ_DOORIO_BENCH), copied into dio at start. */
static oz_doorio dio;

/* Beep patterns (LOCK-IO-1): each beep is OZ_BEEP_MS; the queue holds start
 * times. 1 = a key or a card read, 2 = open, 3 quick = wrong, 5 = door left
 * open, back to back = the tamper alarm. */
#define OZ_TONE_Q 8
static uint32_t tone_q[OZ_TONE_Q];
static int tone_n;
static uint32_t now_ms(void);
static int beeping;                     /* the key beep (BEEP-1), on the OZKIE thread only */
static uint32_t beep_off_at;

/* Start the next queued tone if it is due and none plays. Called by the loop,
 * and at once after a wrong card/PIN so the beep comes before the kv write
 * (KV-LAT-1). */
static void tone_kick(void)
{
    if (beeping || !tone_n || (int32_t)(now_ms() - tone_q[0]) < 0)
        return;
    if (oz_ak1037_beep_start()) {
        beep_off_at = now_ms() + OZ_BEEP_MS;
        beeping = 1;
    } else {
        oz_port_log("beep: start failed (DAC refused), %u more queued", (unsigned)(tone_n - 1));
    }
    memmove(tone_q, tone_q + 1, (size_t)(--tone_n) * sizeof tone_q[0]);
}
#define OZ_BL_FLASH_MS 300u         /* backlight flash per press (TOUCH-LED-1) */

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
static void post_frame(const void *data, uint16_t len, uint32_t chr)
{
    static msg_t m;
    m.h.ev = EV_FRAME;
    m.h.len = len;
    m.h.arg = chr;                      /* CH_OZKIE or CH_RELAY */
    memcpy(m.data, data, len);
    if (rt_mq_send(&mq, &m, offsetof(msg_t, data) + len) != RT_EOK)
        oz_port_log("gatt: queue full, frame dropped");
}

/* A card's identity, from the reader's thread (or the shell): the same
 * layout as msg_t up to its 16 bytes of data, on the caller's stack. */
static int post_card_ev(const uint8_t *id, size_t len, uint32_t fresh);
static int post_card(const uint8_t *id, size_t len) { return post_card_ev(id, len, 0); }
static int post_card_ev(const uint8_t *id, size_t len, uint32_t fresh)
{
    struct {
        hdr_t   h;
        uint8_t data[OZ_CARD_ID_MAX];
    } m;
    if (len == 0 || len > OZ_CARD_ID_MAX)
        return OZ_E_ARG;
    m.h.ev = EV_CARD;
    m.h.len = (uint16_t)len;
    m.h.arg = fresh;
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
    if (!info || (info->p_att_rec != REC_OZKIE && info->p_att_rec != REC_RELAY))
        return;
    if (info->buf_size == 0 || info->buf_size > FRAME_MAX)
        return;                         /* the reassembler would refuse it anyway */
    post_frame(info->p_buf, info->buf_size, info->p_att_rec == REC_RELAY ? CH_RELAY : CH_OZKIE);
}

/* `info` is read from the BLE context, so it serves a snapshot the OZKIE
 * thread refreshes; it never calls into the lock. */
/* INFO-SIZE-1 (2026-11-20): was 96; a release version "ozkie-ak1037-0.1.0+<sha7>"
 * made INFO 97 bytes, oz_lock_info() refused it and the lock served an EMPTY
 * INFO (unclaimable, unidentifiable). 160 leaves room for a longer version. */
static uint8_t info_snap[160];
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
/* RELAY-AK-1: ONE relay link. The SDK keeps one connection (uMaxInitLinks 1),
 * so its owner is that connection, once it may own it (oz_lock_relay_may_own:
 * an authenticated bridge, or the owner while no bridge is enrolled). */
static oz_link relay_link;
static oz_ble relay_ble;
static int relay_owned;
static uint32_t relay_unauth;
static oz_adv adv;
static oz_keypad keypad;
static oz_motor motor;
static oz_card card;

/* TICK-RATE-1: the OS tick is 1 kHz on this SDK (board.c: hal_timer_init(
 * TIMER_CORE_ID, 1000, rt_tick_increase); measured 999 ticks/s on the EVB),
 * but rtconfig.h says RT_TICK_PER_SECOND 100, so rt_tick_from_millisecond()
 * and rt_thread_mdelay() wait a tenth of what they are asked. Time comes from
 * the SDK's ms counter, and waits are converted at the real rate. */
#define OZ_AK1037_TICK_HZ 1000u
static uint32_t now_ms(void) { return (uint32_t)get_ms_tick_func(); }
static rt_int32_t ms_ticks(uint32_t ms) { return (rt_int32_t)((ms * OZ_AK1037_TICK_HZ + 999u) / 1000u); }

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

/* ctx: NULL for `ozkie`, &relay_idx for `relay`. */
static const uint16_t ozkie_idx = NOTIFY_REC_IDX, relay_idx = NOTIFY_RELAY_IDX;
static int notify(void *ctx, const uint8_t *frame, size_t len)
{
    const uint16_t idx = ctx ? *(const uint16_t *)ctx : ozkie_idx;
    void *h = conn;
    if (!h)
        return OZ_E_HW;
    /* The stack's send ring can fill on a burst of fragments: wait it out. */
    for (int tries = 0; tries < 50; tries++) {
        T_eBLE_RESULT r = com_ble_server_notify_ext(h, (uint8_t *)frame, (uint16_t)len, idx);
        if (r == eBLE_RESULT_SUCCESS)
            return OZ_OK;
        if (r != eBLE_RESULT_BUFFER_FULL)
            return OZ_E_HW;
        rt_thread_delay((rt_tick_t)ms_ticks(5));
    }
    return OZ_E_HW;
}

static void refresh_info(const oz_lock *L)
{
    uint8_t buf[sizeof info_snap];
    size_t n = 0;
    int rc = oz_lock_info(L, buf, sizeof buf, &n);
    if (rc) {                           /* never silently: an empty INFO = an unclaimable lock */
        oz_port_log("gatt: INFO does not fit its %u-byte buffer (%d): the lock can't be identified",
                    (unsigned)sizeof buf, rc);
        return;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(info_snap, buf, n);
    info_snap_len = n;
    rt_hw_interrupt_enable(level);
}

void oz_gatt_ak1037_pair_prompt(int on) { oz_keypad_pairing(&keypad, on); }

/* F-AK-WDT-1 SAFE mode (a watchdog reset loop): unlocking is never refused
 * (the oz_power rule: never lock anyone out), but a LOCK move is, in case the
 * motor is what keeps resetting the lock. Since F-AK-WDT-2 oz_lock refuses
 * LOCK requests itself (5.03 SAFE_MODE); this stays as the last guard. The refusal ends the move as
 * JAMMED, so the lock reports "not fully locked" rather than a bolt it never
 * threw. Runs on the OZKIE thread, as motor_step does. */
static int safe_refused;

void oz_gatt_ak1037_actuate(int unlock)
{
    if (!unlock && oz_ak1037_wdt_safe()) {
        safe_refused = 1;
        return;
    }
    if (unlock)
        oz_doorio_unlocked(&dio, now_ms());     /* starts the auto-relock clock */
    else
        oz_doorio_locked(&dio);
    oz_motor_command(&motor, unlock, now_ms());
}

/* The motor driver (MOTOR-GPIO-1): the kit's motor board on GPIO17/18. There
 * is no bolt sensor yet, so every move ends OZ_MOVE_UNCONFIRMED; oz_motor caps
 * every drive at OZ_MOTOR_MAX_DRIVE_MS. Which way is "unlock" on a real lock
 * body is a wiring question: swap motor_A/B if it turns the wrong way. */
static void motor_drive(uint8_t out)
{
    static const char *const name[] = { "off", "to unlock", "to lock", "brake" };
    static const uint8_t ab[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    oz_ak1037_motor_pins(ab[out & 3][0], ab[out & 3][1]);
    /* The lock's own ms tick, so the bench can measure a move from the
     * serial log (BENCH-1: a relayed log arrives in bursts). */
    oz_port_log("motor: %s @%lu ms", name[out & 3], (unsigned long)now_ms());
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
    if (safe_refused) {
        safe_refused = 0;
        oz_lock_actuated(L, 0, OZ_MOVE_JAMMED);
        oz_port_log("motor: lock refused: SAFE MODE after repeated watchdog resets");
    }
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
    if (r == OZ_CARD_OPEN) {
        oz_port_log("card: open");
        tones(2, 80);
    } else if (r == OZ_CARD_LOCKED_OUT) {
        oz_port_log("card: locked out, %u s left, not checked",
                    (unsigned)((oz_lock_card_lockout_ms(L) + 999u) / 1000u));
        tones(3, 40);
    } else {
        oz_port_log("card: wrong");
        tones(3, 40);
        tone_kick();                    /* KV-LAT-1: the beep first, */
        oz_lock_flush(L);               /* then the count, before anything else is read */
    }
}

static void key_in(oz_lock *L, char key, int connected)
{
    uint32_t code = 0;
    switch (oz_keypad_key(&keypad, key, now_ms(), &code)) {
    case OZ_KP_PIN: {
        int r = oz_lock_keypad_pin(L, keypad.entry);
        oz_keypad_done(&keypad);
        if (r == OZ_PIN_OPEN) {
            oz_port_log("keypad: open");
            tones(2, 80);
        } else if (r == OZ_PIN_LOCKED_OUT) {
            oz_port_log("keypad: locked out, %u s left, not checked",
                        (unsigned)((oz_lock_keypad_lockout_ms(L) + 999u) / 1000u));
            tones(3, 40);
        } else {
            oz_port_log("keypad: wrong");
            tones(3, 40);
            tone_kick();                /* KV-LAT-1: the beep first, */
            oz_lock_flush(L);           /* then the count, before the next key is taken */
        }
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

#ifdef OZ_DEBUG_HOOKS
/* oz.wdthang (debug builds only, PM GO 2026-11-07): the OZKIE thread keeps
 * running but stops checking in, as a stuck handler would. The main loop then
 * stops feeding and the watchdog resets the lock: the bench recipe for SAFE
 * mode (ozkey-48 §6.1). Never in a release build. */
static volatile int dbg_hang;
void oz_gatt_ak1037_debug_hang(void) { dbg_hang = 1; }

/* oz.wipe while the lock runs (AUTOSTART-1): the OZKIE thread, the only kv
 * writer, erases between two passes, then the chip resets, so RAM state never
 * outlives the kv it came from. Debug builds only. */
static volatile int dbg_wipe;
void oz_gatt_ak1037_debug_wipe_reset(void) { dbg_wipe = 1; }

/* oz.flashlat (AK-FLASH-LAT-1): the request; the thread runs it. */
static volatile int dbg_flat_n;
void oz_gatt_ak1037_debug_flashlat(int n) { dbg_flat_n = n; }

/* N kv writes of a 16-byte value (about a limit record), each timed by the
 * ms tick, then the test key erased. On the OZKIE thread only. */
static void flashlat_run(int n, int wdt_id)
{
    static const uint8_t v[16] = { 0x5a };
    uint32_t mn = UINT32_MAX, mx = 0, sum = 0;
    if (oz_kv_lfs_namespace("oz") != OZ_OK) {
        oz_port_log("oz.flashlat: FAILED (kv namespace)");
        return;
    }
    for (int i = 0; i < n; i++) {
        uint32_t t0 = now_ms();
        if (oz_kv_lfs_set("dbgflat", v, sizeof v) != OZ_OK) {
            oz_port_log("oz.flashlat: FAILED at write %d", i + 1);
            (void)oz_kv_lfs_erase("dbgflat");
            return;
        }
        uint32_t dt = now_ms() - t0;
        sum += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
        oz_ak1037_wdt_checkin(wdt_id);  /* 50 slow writes must not trip the watchdog */
    }
    uint32_t t0 = now_ms();
    int er = oz_kv_lfs_erase("dbgflat");
    oz_port_log("oz.flashlat: %d writes, set min=%lu mean=%lu max=%lu ms, erase %lu ms%s", n,
                (unsigned long)mn, (unsigned long)(sum / (uint32_t)n), (unsigned long)mx,
                (unsigned long)(now_ms() - t0), er == OZ_OK ? "" : " (erase FAILED)");
}
#endif

static void ozkie_thread(void *param)
{
    oz_lock *L = param;
    static msg_t m;
    int connected = 0;
    uint32_t connected_at = 0;      /* now_ms at connect; wraps, subtract only */
    int kicked = 0;
    uint32_t squat_drops = 0;
    int applied = -1, failed = 0;
    int wdt_id = oz_ak1037_wdt_add(OZ_GATT_WDT_MS);
#ifdef OZ_DEBUG_HOOKS
    uint32_t pass_t0 = 0;
#endif
    int bl_on = 0;                  /* 0 dark; 1/2 all keys, toggling (card, alarm); 3 one key (PER-KEY-BL-1) */
    uint32_t bl_off_at = 0;
    refresh_info(L);
    for (;;) {
#ifdef OZ_DEBUG_HOOKS
        if (dbg_wipe) {
            dbg_wipe = 0;
            if (oz_kv_lfs_namespace("oz") == OZ_OK && oz_kv_lfs_wipe() == OZ_OK) {
                oz_port_log("oz.wipe: identity and bonds erased; restarting");
                hal_clock_chip_reset();
            } else {
                oz_port_log("oz.wipe: FAILED (kv); nothing reset");
            }
        }
        if (dbg_flat_n > 0) {
            int n = dbg_flat_n;
            dbg_flat_n = 0;
            flashlat_run(n, wdt_id);
        }
        if (!dbg_hang)
#endif
        oz_ak1037_wdt_checkin(wdt_id);
        /* 10 ms while the motor moves (oz_motor_step's cadence) or the
         * backlight flashes (its toggle), else 100. */
        rt_int32_t wait = motor.phase == OZ_MOTOR_IDLE && !bl_on && !beeping && !tone_n ? 100 : 10;  /* 10 ms: motor, flash or beep */
#ifdef OZ_DEBUG_HOOKS
        if (pass_t0) {                  /* the previous pass's work, its wait excluded */
            uint32_t dt = now_ms() - pass_t0;
            ozp_n++;
            if (dt > ozp_max)
                ozp_max = dt;
            if (dt >= 50)
                ozp_over50++;
        }
#endif
        int got = rt_mq_recv(&mq, &m, sizeof m, ms_ticks((uint32_t)wait)) == RT_EOK;
#ifdef OZ_DEBUG_HOOKS
        pass_t0 = now_ms();
#endif
        if (got) {
            switch (m.h.ev) {
            case EV_CONNECT:
                oz_ble_connect(&session);
                if (!connected)
                    oz_adv_link(&adv, 1);
                connected = 1;
                connected_at = now_ms();
                kicked = 0;
                break;
            case EV_MTU:
                oz_ble_set_mtu(&session, m.h.arg);
                if (relay_owned)
                    oz_ble_set_mtu(&relay_ble, m.h.arg);
                break;
            case EV_FRAME:
                if (!connected)
                    break;
                if (m.h.arg == CH_RELAY) {
                    if (!relay_owned && !oz_lock_relay_may_own(L, &link)) {
                        if (++relay_unauth == 1 || relay_unauth % 100 == 0)
                            oz_port_log("gatt: relay write from an unauthenticated connection dropped (%lu)",
                                        (unsigned long)relay_unauth);
                        break;
                    }
                    if (!relay_owned) {
                        relay_owned = 1;
                        oz_ble_connect_as(&relay_ble, 1);
                        oz_ble_set_mtu(&relay_ble, session.mtu);
                        oz_port_log("gatt: relay owned");
                    }
                    (void)oz_ble_rx(&relay_ble, m.data, m.h.len);
                } else {
                    (void)oz_ble_rx(&session, m.data, m.h.len);
                }
                refresh_info(L);        /* a claim changes the state field */
                break;
            case EV_PAIR_CODE:
                if (connected)
                    (void)oz_ble_pair_code(&session, m.h.arg);
                refresh_info(L);
                break;
            case EV_DISCONNECT:
                if (relay_owned) {
                    oz_ble_disconnect(&relay_ble);  /* half-done EDHOC goes; contexts stay in L */
                    relay_owned = 0;
                    oz_port_log("gatt: relay released (disconnect)");
                }
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
            case EV_PAD: {
                char c = pad_char((int)m.h.arg);
                /* The operator sees every press: its own key only (PER-KEY-BL-1),
                 * unless all keys are already flashing (a card, the tamper alarm). */
                if (bl_on != 1 && bl_on != 2) {
                    bl_on = oz_ak1037_backlight_pad((int)m.h.arg) ? 3 : 0;
                    bl_off_at = now_ms() + OZ_BL_FLASH_MS;
                }
                tones(1, 0);                    /* …and hears it (BEEP-1) */
                oz_port_log("touch: pad %d -> %c", (int)m.h.arg, c ? c : '-');
                if (c)
                    key_in(L, c, connected);
                break;
            }
            case EV_CARD:
                card_in(L, m.data, m.h.len);
                if (m.h.arg) {                  /* the reader: a new card on the antenna */
                    if (bl_on != 1 && bl_on != 2) {     /* all keys flash + a beep */
                        bl_on = 1;              /* (an alarm flash already on keeps its end) */
                        bl_off_at = now_ms() + OZ_BL_FLASH_MS;
                    }
                    tones(1, 0);
                }
                break;
            default:
                break;
            }
        }
        if (connected)
            (void)oz_ble_tick(&session);
        if (relay_owned)
            (void)oz_ble_tick(&relay_ble);
        /* Taken under the rev 6 rule (the owner, no bridge yet): given back as
         * soon as its owner may no longer hold it (a bridge was enrolled). */
        if (relay_owned && !oz_lock_relay_may_own(L, &link)) {
            oz_ble_disconnect(&relay_ble);
            relay_owned = 0;
            oz_port_log("gatt: relay released: its owner is not an authenticated bridge");
        }
        /* App-slot squat timeout (F1-BRIDGE-BOND-1 (B), ozkey-48 rev 7 §2.3): the
         * one connection here IS the app slot. 🟡 com_ble_disconnect(0) — link
         * index 0 for the single peripheral link — is unverified until the kit. */
        if (connected && !kicked &&
            oz_lock_link_squatting(L, &link, relay_owned, (uint32_t)(now_ms() - connected_at))) {
            kicked = 1;
            ++squat_drops;
            oz_port_log("gatt: connection dropped: no authenticated session in %u s (%lu so far)",
                        OZ_LOCK_AUTH_GRACE_MS / 1000u, (unsigned long)squat_drops);
            (void)com_ble_disconnect(0);
        }
        motor_step(L);
        /* BEEP-RFID-1: no antenna read while the motor moves or a tone plays
         * or waits. RFID-THREAD-1: the reads themselves run on the reader
         * thread; this only tells it when not to. */
        rfid_hold = !(motor.phase == OZ_MOTOR_IDLE && !beeping && !tone_n);
        /* LOCK-IO-1: the door switches. The exit button unlocks ALWAYS (PM). */
        {
            uint8_t pr[OZ_DIO_SW_N];
            oz_ak1037_switches(pr);
            int d = oz_doorio_step(&dio, pr, now_ms());
            if (d & OZ_DIO_EXIT_UNLOCK) {
                oz_lock_exit_button(L);
                tones(2, 80);
            }
            if (d & OZ_DIO_DOOR_OPENED)
                oz_port_log("door: opened @%lu ms", (unsigned long)now_ms());
            if (d & OZ_DIO_DOOR_CLOSED)
                oz_port_log("door: closed @%lu ms", (unsigned long)now_ms());
            if (d & OZ_DIO_RELOCK)
                oz_lock_auto_relock(L);
            if (d & OZ_DIO_TAMPER) {
                oz_lock_door_event(L, OZ_EV_TAMPER, 0);
                bl_off_at = now_ms() + dio.cfg.alarm_ms;
                bl_on = 1;
            }
            if (d & OZ_DIO_ALARM_END)
                oz_port_log("lock: tamper alarm over");
            if (d & OZ_DIO_LEFT_OPEN) {
                oz_lock_door_event(L, OZ_EV_DOOR_LEFT_OPEN, dio.cfg.left_open_ms / 1000u);
                tones(5, 120);
            }
            if (oz_doorio_alarm(&dio) && tone_n == 0 && !beeping)
                tones(1, 0);                /* the alarm: beeps back to back */
        }
        if (beeping && (int32_t)(now_ms() - beep_off_at) >= 0) {
            oz_ak1037_beep_stop();
            beeping = 0;
        }
        tone_kick();
        /* KV-LAT-1: a right credential's limit reset is written when nothing
         * moves or beeps (losing it to a power cut fails safe). */
        if (motor.phase == OZ_MOTOR_IDLE && !beeping && !tone_n)
            oz_lock_flush(L);
        battery_rest(L);
        if (oz_keypad_tick(&keypad, now_ms()) == OZ_KP_CLEARED)
            oz_port_log("keypad: entry timed out");
        if (bl_on && (int32_t)(now_ms() - bl_off_at) >= 0) {
            oz_ak1037_backlight(OZ_BL_OFF);
            bl_on = 0;
        } else if (bl_on == 1 || bl_on == 2) {
            bl_on = bl_on == 1 ? 2 : 1;     /* toggle: both LEDs of each pair */
            oz_ak1037_backlight(bl_on == 1 ? OZ_BL_HIGH : OZ_BL_LOW);
        }

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

/* ── On-chip touch pads (TOUCH-KEYPAD-1) ────────────────────────────────── */

/* The EVB's 15 pads, as the SDK sample sets them for EVB_MBD_AK1037D V103
 * (board_mcu_anyka.h: TOUCH_SEN 60, TOUCH_WK_SEN 50, key mark 0x7fff). */
#define OZ_TK_KEYMARK 0x7fffu
#define OZ_TK_SEN     60
#define OZ_TK_WK_SEN  50

/* Pad -> key (TOUCH-MAP-1, measured on the EVB 2026-11-18): the SDK reports a
 * one-bit mask per pad, from bit 14 = "1" down: keys 1-9 = bits 14..6, '*' bit
 * 5, '0' bit 4, '#' bit 3; mute (2), bell (1) and lock (0) are not keys yet.
 * Anything not a single known bit (two pads at once) is ignored, but logged. */
static char pad_char(int pad)
{
    static const char by_bit[15] = { 0, 0, 0, '#', '0', '*', '9', '8', '7', '6', '5', '4', '3', '2', '1' };
    for (int b = 0; b < 15; b++)
        if (pad == (1 << b))
            return by_bit[b];
    return 0;
}

/* From the SDK's touch scan (may be interrupt context): post only. */
static void on_pad(int fun, int pad)
{
    if (fun == TOUCH_KEY_PRESS)
        post(EV_PAD, (uint32_t)pad);
}

static void touch_start(void)
{
    static tk_dev_opt_t tk;
    tk_dev_param_t p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 15; i++)
        p.sen[i] = OZ_TK_SEN;
    p.sen_wk = OZ_TK_WK_SEN;
    p.callback = on_pad;
    if (dev_tk_register_inner(&tk) == AK_FALSE || !tk.init) {
        oz_port_log("touch: driver registration failed; keypad via the shell only");
        return;
    }
    oz_ak1037_backlight_init();         /* off, before any press can light it */
    (void)tk.init(&p, OZ_TK_KEYMARK);
    oz_port_log("touch: 15 pads on");
}

/* ── On-chip RFID reader (RFID-READER-1) ─────────────────────────────────── */

/* hal_rfid_read_card() runs one read on the antenna and blocks while it does
 * (13 ms mean, 71 ms max measured; no non-blocking read: Anyka Q6). RFID-
 * THREAD-1: so it runs on its own thread, one priority below the OZKIE thread
 * (Anyka's advice), every OZ_RFID_POLL_MS, never while the OZKIE thread holds
 * it (motor, tones). Each read is posted as EV_CARD (arg 1 = a new card on the
 * antenna); the card path (oz_card_seen) makes a held card one check. The
 * slowest poll so far is logged (from 20 ms). */
#define OZ_RFID_POLL_MS  250u
#define OZ_RFID_SLOW_MS  20u
static uint32_t rfid_next, rfid_slowest;
#ifdef OZ_DEBUG_HOOKS
/* RFID-TIME-1: every poll's time, for Anyka Q12 (polls of 243 and 1006 ms
 * seen on the chip). Buckets: <50, 50-99, 100-249, 250-499, >=500 ms. */
static volatile uint32_t rst_n, rst_sum, rst_max, rst_hist[5];

void oz_gatt_ak1037_debug_rfidstat(int reset)
{
    if (reset) {
        rst_n = rst_sum = rst_max = 0;
        for (int i = 0; i < 5; i++)
            rst_hist[i] = 0;
        ozp_n = ozp_max = ozp_over50 = 0;
    }
    printf("oz.rfidstat: n=%lu mean=%lu ms max=%lu ms | <50:%lu 50-99:%lu 100-249:%lu 250-499:%lu >=500:%lu \r\n",
           (unsigned long)rst_n, (unsigned long)(rst_n ? rst_sum / rst_n : 0), (unsigned long)rst_max,
           (unsigned long)rst_hist[0], (unsigned long)rst_hist[1], (unsigned long)rst_hist[2],
           (unsigned long)rst_hist[3], (unsigned long)rst_hist[4]);
    printf("ozkie pass: n=%lu max=%lu ms over50=%lu \r\n",
           (unsigned long)ozp_n, (unsigned long)ozp_max, (unsigned long)ozp_over50);
}
#endif
static uint8_t  rfid_last[OZ_CARD_ID_MAX];
static size_t   rfid_last_len;

#define OZ_RFID_TICK_MS  50u          /* the reader thread's wake-up */
#define OZ_RFID_WDT_MS   5000u        /* a read hung this long resets the chip (1006 ms seen once) */
#define OZ_RFID_STACK    2048u

/* One reader iteration (the reader thread; the host mock steps it). */
void oz_gatt_ak1037_rfid_step(void)
{
    uint8_t id[32];
    uint32_t t0 = now_ms();
    if (rfid_hold || (int32_t)(t0 - rfid_next) < 0)
        return;
    int32_t n = hal_rfid_read_card(CARD_TYPE_A, id);
    uint32_t dt = now_ms() - t0;
    rfid_next = now_ms() + OZ_RFID_POLL_MS;
    if (dt >= OZ_RFID_SLOW_MS && dt > rfid_slowest) {
        rfid_slowest = dt;
        oz_port_log("rfid: slowest poll %lu ms", (unsigned long)dt);
    }
#ifdef OZ_DEBUG_HOOKS
    rst_n++;
    rst_sum += dt;
    if (dt > rst_max)
        rst_max = dt;
    rst_hist[dt < 50 ? 0 : dt < 100 ? 1 : dt < 250 ? 2 : dt < 500 ? 3 : 4]++;
    if (dt >= 100)
        oz_port_log("rfid: slow poll %lu ms @%lu ms (%s)", (unsigned long)dt, (unsigned long)t0,
                    n > 0 ? "card" : "no card");
#endif
    if (n <= 0 || n > (int32_t)OZ_CARD_ID_MAX) {
        rfid_last_len = 0;              /* the antenna is free again */
        return;
    }
    int fresh = 0;
    if ((size_t)n != rfid_last_len || memcmp(id, rfid_last, (size_t)n) != 0) {
        char hex[2 * OZ_CARD_ID_MAX + 1];
        for (int32_t i = 0; i < n; i++) {
            static const char h[] = "0123456789abcdef";
            hex[2 * i] = h[id[i] >> 4];
            hex[2 * i + 1] = h[id[i] & 15];
        }
        hex[2 * n] = 0;
#ifdef OZ_DEBUG_HOOKS
        oz_port_log("rfid: card %s", hex);          /* the full id: bench image only */
#else
        (void)hex;
        oz_port_log("rfid: card read (%d bytes)", (int)n);  /* release: no id in the log (PM) */
#endif
        fresh = 1;
        memcpy(rfid_last, id, (size_t)n);
        rfid_last_len = (size_t)n;
    }
    (void)post_card_ev(id, (size_t)n, (uint32_t)fresh);
}

static struct rt_thread rfid_thread_tcb;
static uint8_t rfid_stack[OZ_RFID_STACK] __attribute__((aligned(8)));

static void rfid_thread(void *param)
{
    (void)param;
    int wdt_id = oz_ak1037_wdt_add(OZ_RFID_WDT_MS);
    for (;;) {
        rt_thread_delay((rt_tick_t)ms_ticks(OZ_RFID_TICK_MS));
        oz_ak1037_wdt_checkin(wdt_id);
        oz_gatt_ak1037_rfid_step();
    }
}

static void rfid_start(void)
{
    (void)hal_rfid_init();
    oz_port_log("rfid: reader on");
}

/* After the OZKIE thread exists (EV_CARD goes to its queue). */
static void rfid_thread_start(uint8_t prio)
{
    uint8_t p = prio + 1u < RT_THREAD_PRIORITY_MAX - 1u ? (uint8_t)(prio + 1u) : (uint8_t)(RT_THREAD_PRIORITY_MAX - 2u);
    if (rt_thread_init(&rfid_thread_tcb, "ozrfid", rfid_thread, NULL, rfid_stack, sizeof rfid_stack, p, 10) != RT_EOK) {
        oz_port_log("rfid: reader thread FAILED, no cards");
        return;
    }
    rt_thread_startup(&rfid_thread_tcb);
}

static void tones(int n, uint32_t gap_ms)
{
    uint32_t t = now_ms();
    if (tone_n && (int32_t)(tone_q[tone_n - 1] + OZ_BEEP_MS + gap_ms - t) > 0)
        t = tone_q[tone_n - 1] + OZ_BEEP_MS + gap_ms;
    for (int i = 0; i < n && tone_n < OZ_TONE_Q; i++) {
        tone_q[tone_n++] = t;
        t += OZ_BEEP_MS + gap_ms;
    }
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

/* The BLE stack, without the lock: the stack sets the device address during
 * its own init, so the lock's ID can only be read after this (LOCK-ID-1).
 * Its own advertising stays off, so nothing can connect before the OZKIE
 * thread runs; events in between wait in the queue. Runs once. */
int oz_gatt_ak1037_radio_up(void)
{
    static int up;
    if (up)
        return OZ_OK;
    oz_ak1037_motor_pins_init();        /* main_ozkie did it at boot; again is harmless */
    if (rt_mq_init(&mq, "ozkie", mq_pool, SLOT_SIZE, sizeof mq_pool, RT_IPC_FLAG_FIFO) != RT_EOK)
        return OZ_E_HW;
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
    /* AK-SDK-2-1: SDK V2.00 takes the local MTU after init and before any
     * connection, per link (its samples: link 0, right here). V1.02 asks for
     * "after the service", which this also is. The connect-time request below
     * stays until a V2.00 burn shows whether it is still needed. */
    if (com_ble_set_mtu_size(com_ble_get_conn_handle(0), 247) != 1)
        oz_port_log("gatt: MTU 247 not set before connect");
    up = 1;
    return OZ_OK;
}

int oz_gatt_ak1037_start(oz_lock *L, uint8_t thread_priority)
{
    if (oz_gatt_ak1037_radio_up() != OZ_OK)
        return OZ_E_HW;
    if (oz_ak1037_wdt_safe())
        oz_lock_set_safe(L, oz_ak1037_wdt_resets());   /* F-AK-WDT-2: refusals + the event */
    oz_ble_init(&session, L, &link, notify, NULL);
    oz_ble_init(&relay_ble, L, &relay_link, notify, (void *)&relay_idx);
    oz_adv_init(&adv, OZ_GATT_ADV_ALWAYS, now_ms());
    oz_keypad_init(&keypad);
    oz_motor_init(&motor, &oz_ak1037_board()->motor);   /* BOARD-PROFILE-1 */
    oz_card_init(&card);
    oz_ak1037_speaker_init();
    oz_ak1037_switches_init();
    oz_doorio_init(&dio, &oz_ak1037_board()->doorio);
    touch_start();
    rfid_start();
    if (rt_thread_init(&thread, "ozkie", ozkie_thread, L, thread_stack, sizeof thread_stack,
                       thread_priority, 10) != RT_EOK)
        return OZ_E_HW;
    rt_thread_startup(&thread);
    rfid_thread_start(thread_priority);  /* RFID-THREAD-1: after the queue's reader exists */
    return OZ_OK;
}
