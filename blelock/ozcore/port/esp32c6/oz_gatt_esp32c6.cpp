/* oz_gatt_esp32c6 — see oz_gatt_esp32c6.h.
 *
 * Arduino-ESP32 3.3.11 BLE library. On the C6 it builds the NimBLE backend
 * (CONFIG_NIMBLE_ENABLED, confirmed for the v1 sketches in
 * ozdoorlock_core.h); the Bluedroid callbacks are kept for other targets.
 *
 * Two connections (ozkey-48 rev 6 §2.3, as v1 since 2026-08-30): slot for the
 * bridge, slot for the app at the door, never more. Each connection has its
 * own session link on `ozkie`. There is ONE relay link, on `relay`, owned by
 * the first connection that writes to it until that connection drops; the
 * other connection's `relay` writes are dropped and counted. Notifications go
 * to one connection (ble_gatts_notify_custom), never to every subscriber.
 * NimBLE only: the per-connection handles come from its callbacks.
 *
 * Advertising lessons carried over from v1 (ozdoorlock_core.h, 2026-08-04..06):
 *   - setAdvertisementType() forwards the raw byte to the backend untranslated,
 *     and NimBLE's conn_mode numbers differ from Bluedroid's PDU types. Use the
 *     backend's own constants (OZ_ADV_TYPE_* below).
 *   - Setting scan-response data restarts advertising asynchronously with
 *     whatever type is set at completion time. So: stop, set the TYPE FIRST,
 *     then both payloads, then start. One function (adv_apply) does it all.
 */
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern "C" {
#include "oz_port.h"
#include "oz_adv.h"
#include "oz_ble.h"
#include "oz_keypad.h"
#include "oz_motor.h"
#include "oz_card.h"
}
#include "oz_gatt_esp32c6.h"

#if !defined(CONFIG_NIMBLE_ENABLED)
#error "oz_gatt_esp32c6 needs the NimBLE backend (per-connection handles and notify)"
#endif
#define OZ_ADV_TYPE_IND      BLE_GAP_CONN_MODE_UND /* connectable */
#define OZ_ADV_TYPE_SCAN_IND BLE_GAP_CONN_MODE_NON /* scannable, not connectable */

#define MAX_CONNS 2                     /* bridge + app at the door; never more */
enum { CH_OZKIE = 0, CH_RELAY = 1 };

/* ── BLE callbacks → OZKIE task ──────────────────────────────────────────── */

enum { EV_CONNECT = 1, EV_DISCONNECT, EV_MTU, EV_FRAME, EV_PAIR_CODE, EV_TOUCH, EV_KEY, EV_CARD };

#define FRAME_MAX 244                   /* MTU 247 − 3 (ozkey-48 §2.1) */
typedef struct {
    uint8_t  ev;
    uint8_t  chr;                       /* EV_FRAME: CH_OZKIE or CH_RELAY */
    uint16_t conn;                      /* NimBLE connection handle */
    uint16_t len;
    uint32_t arg;
    uint8_t  data[FRAME_MAX];
} msg_t;

#define QUEUE_DEPTH 8
static QueueHandle_t mq;
static volatile int conn_up;            /* connections up (a racy read, for the shell) */
static volatile int bridge_link;        /* 0 none, 1 relay without bridge auth, 2 authenticated bridge (racy, display) */

static void post_c(uint8_t ev, uint16_t conn, uint8_t chr, uint32_t arg, const uint8_t *data, size_t len);
static void post(uint8_t ev, uint32_t arg, const uint8_t *data, size_t len)
{
    post_c(ev, 0, 0, arg, data, len);
}

static void post_c(uint8_t ev, uint16_t conn, uint8_t chr, uint32_t arg, const uint8_t *data, size_t len)
{
    static msg_t m;                     /* callers are serialised per source; see below */
    msg_t local;
    msg_t *p = (ev == EV_FRAME) ? &m : &local; /* frames come from the BLE host task only */
    p->ev = ev;
    p->conn = conn;
    p->chr = chr;
    p->arg = arg;
    p->len = (uint16_t)len;
    if (len)
        memcpy(p->data, data, len);
    if (xQueueSend(mq, p, 0) != pdTRUE)
        oz_port_log("gatt: queue full, event %u dropped", (unsigned)ev);
}

static BLEServer *server;
static BLECharacteristic *chr_info, *chr_ozkie, *chr_relay;

class ServerCb : public BLEServerCallbacks {
    void onConnect(BLEServer *, ble_gap_conn_desc *d) override { post_c(EV_CONNECT, d->conn_handle, 0, 0, NULL, 0); }
    void onDisconnect(BLEServer *, ble_gap_conn_desc *d) override { post_c(EV_DISCONNECT, d->conn_handle, 0, 0, NULL, 0); }
    void onMtuChanged(BLEServer *, ble_gap_conn_desc *d, uint16_t mtu) override { post_c(EV_MTU, d->conn_handle, 0, mtu, NULL, 0); }
};

class FrameCb : public BLECharacteristicCallbacks {
    uint8_t ch;
public:
    explicit FrameCb(uint8_t c) : ch(c) {}
    void onWrite(BLECharacteristic *c, ble_gap_conn_desc *d) override
    {
        size_t n = c->getLength();
        if (n == 0 || n > FRAME_MAX)
            return;                     /* the reassembler would refuse it anyway */
        post_c(EV_FRAME, d->conn_handle, ch, 0, c->getData(), n);
    }
};

/* ── the OZKIE task ──────────────────────────────────────────────────────── */

typedef struct {
    int      used;
    uint16_t h;                         /* NimBLE connection handle */
    size_t   mtu;
    oz_link  link;                      /* this connection's session link */
    oz_ble   ble;
    uint32_t at;                        /* connected at (now_ms, wraps: subtract only) */
    int      kicked;                    /* squat disconnect already requested */
} conn_t;

typedef struct { BLECharacteristic **chr; int idx; } tx_t;

static conn_t conns[MAX_CONNS];
static tx_t   tx_session[MAX_CONNS];
static oz_link relay_link_;
static oz_ble  relay_ble;
static tx_t    tx_relay = { &chr_relay, -1 };
static int     relay_owner = -1;        /* index into conns, −1 = none */
static uint32_t relay_refused;          /* `relay` writes from the non-owner */
static uint32_t relay_unauth;           /* `relay` writes refused: not an authenticated bridge */
static uint32_t squat_drops;            /* connections dropped by the 30 s squat timeout */
static oz_adv adv;
static oz_keypad keypad;
static oz_motor motor;
static oz_card card;

static uint32_t now_ms(void) { return (uint32_t)oz_port_monotonic_ms(); }

static int adv_apply(const oz_lock *L, oz_adv_mode mode)
{
    uint8_t data[OZ_ADV_MAX], rsp[OZ_ADV_MAX];
    BLEAdvertising *a = BLEDevice::getAdvertising();
    a->stop();
    if (mode == OZ_ADV_OFF)
        return 0;
    size_t dn = oz_adv_data(data);
    size_t rn = oz_adv_scan_rsp(rsp, mode == OZ_ADV_BUSY, L->device_id);
    if (rn == 0) {
        oz_port_log("adv: device_id %s has no short id", L->device_id);
        return -1;
    }
    delay(20);                          /* let the stop settle (v1 lesson) */
    a->setAdvertisementType(mode == OZ_ADV_BUSY ? OZ_ADV_TYPE_SCAN_IND : OZ_ADV_TYPE_IND);
    a->setMinInterval(OZ_GATT_ADV_INTERVAL);
    a->setMaxInterval(OZ_GATT_ADV_INTERVAL);
    BLEAdvertisementData d, r;
    d.addData((char *)data, dn);        /* raw AD structures, byte for byte */
    r.addData((char *)rsp, rn);
    if (!a->setAdvertisementData(d) || !a->setScanResponseData(r))
        return -1;
    delay(20);
    if (!a->start())
        return -1;
    oz_port_log("adv: %s", mode == OZ_ADV_BUSY ? "busy (scan_ind)" : "open (ind)");
    return 0;
}

/* One frame to one connection: ctx says which characteristic and which slot. */
static int notify(void *ctx, const uint8_t *frame, size_t len)
{
    tx_t *t = (tx_t *)ctx;
    int i = (t == &tx_relay) ? relay_owner : t->idx;
    if (i < 0 || !conns[i].used)
        return OZ_E_HW;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
    if (!om || ble_gatts_notify_custom(conns[i].h, (*t->chr)->getHandle(), om) != 0)
        return OZ_E_HW;                 /* notify_custom frees om on every path */
    /* A short yield keeps a burst of fragments from running the host's
     * buffers dry (AK1037 retries BUFFER_FULL). */
    vTaskDelay(pdMS_TO_TICKS(3));
    return OZ_OK;
}

static void refresh_info(const oz_lock *L)
{
    uint8_t buf[96];
    size_t n = 0;
    if (oz_lock_info(L, buf, sizeof buf, &n) == OZ_OK)
        chr_info->setValue(buf, n);
}

extern "C" void oz_gatt_c6_pair_prompt(int on) { oz_keypad_pairing(&keypad, on); }
extern "C" void oz_gatt_c6_actuate(int unlock) { oz_motor_command(&motor, unlock, now_ms()); }

/* Motor: log-only on the bench boards (no bolt), so every move ends
 * UNCONFIRMED, exactly as AK1037 does until OZ-7. */
static void motor_step(oz_lock *L)
{
    static uint8_t driven = OZ_MOTOR_OFF;
    static const char *const name[] = { "off", "to unlock", "to lock", "brake" };
    int unlock = 0;
    int r = oz_motor_step(&motor, now_ms(), OZ_BOLT_NONE, 0, &unlock);
    if (motor.out != driven) {
        driven = motor.out;
        oz_port_log("motor: %s", name[driven & 3]);
    }
    if (r != OZ_MOVE_NONE) {
        oz_lock_actuated(L, unlock, r);
        oz_port_log("motor: %s %s", unlock ? "unlock" : "lock",
                    r == OZ_MOVE_OK ? "ok" : r == OZ_MOVE_JAMMED ? "JAMMED" : "unconfirmed");
    }
}

static void card_in(oz_lock *L, const uint8_t *id, size_t len)
{
    if (!oz_card_seen(&card, id, len, now_ms()))
        return;
    int r = oz_lock_card(L, id, len);
    oz_port_log("card: %s", r == OZ_CARD_OPEN ? "open" : r == OZ_CARD_LOCKED_OUT ? "locked out" : "wrong");
}

/* The connection whose session is waiting for the keypad code, or NULL. */
static oz_ble *pairing_session(void)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].link.pairing)
            return &conns[i].ble;
    return NULL;
}

static int find_conn(uint16_t h)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].h == h)
            return i;
    return -1;
}

/* Connections that make the lock busy (ozkey-48 rev 7 §2.3: busy means the
 * app slot is taken). The lock decides: an authenticated bridge never does;
 * the relay owner doesn't only while no bridge is enrolled (rev 6 rule). */
static int app_links(const oz_lock *L)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && oz_lock_takes_app_slot(L, &conns[i].link, i == relay_owner))
            n++;
    return n;
}

static void key_in(oz_lock *L, char key)
{
    uint32_t code = 0;
    switch (oz_keypad_key(&keypad, key, now_ms(), &code)) {
    case OZ_KP_PIN: {
        int r = oz_lock_keypad_pin(L, keypad.entry);
        oz_keypad_done(&keypad);
        oz_port_log("keypad: %s", r == OZ_PIN_OPEN ? "open" : r == OZ_PIN_LOCKED_OUT ? "locked out" : "wrong");
        break;
    }
    case OZ_KP_PAIR_CODE: {
        oz_ble *s = pairing_session();
        if (s)
            (void)oz_ble_pair_code(s, code);
        else
            oz_port_log("keypad: pairing code but no app is pairing");
        refresh_info(L);
        break;
    }
    case OZ_KP_CLEARED:
        oz_port_log("keypad: cleared");
        break;
    default:
        break;
    }
}

static void ozkie_task(void *param)
{
    oz_lock *L = (oz_lock *)param;
    static msg_t m;
    int applied = -1, failed = 0;
    refresh_info(L);
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(motor.phase == OZ_MOTOR_IDLE ? 100 : 10);
        if (xQueueReceive(mq, &m, wait) == pdTRUE) {
            int i = find_conn(m.conn);
            switch (m.ev) {
            case EV_CONNECT: {
                int f = -1;
                for (int j = 0; j < MAX_CONNS && f < 0; j++)
                    if (!conns[j].used)
                        f = j;
                if (f < 0) {
                    oz_port_log("gatt: a third connection, refused");
                    server->disconnect(m.conn);
                    break;
                }
                conns[f].used = 1;
                conns[f].h = m.conn;
                conns[f].at = now_ms();
                conns[f].kicked = 0;
                conns[f].mtu = OZ_FRAME_MIN_MTU;
                oz_ble_connect(&conns[f].ble);
                applied = -1;           /* NimBLE stops advertising on a connect */
                break;
            }
            case EV_MTU:
                if (i >= 0) {
                    conns[i].mtu = m.arg;
                    oz_ble_set_mtu(&conns[i].ble, m.arg);
                    if (i == relay_owner)
                        oz_ble_set_mtu(&relay_ble, m.arg);
                }
                break;
            case EV_FRAME:
                if (i < 0)
                    break;
                if (m.chr == CH_OZKIE) {
                    (void)oz_ble_rx(&conns[i].ble, m.data, m.len);
                } else {
                    if (relay_owner < 0 && !oz_lock_relay_may_own(L, &conns[i].link)) {
                        /* F1-BRIDGE-BOND-1: a bridge is enrolled, and this
                         * connection hasn't authenticated as one. */
                        if (++relay_unauth == 1 || relay_unauth % 100 == 0)
                            oz_port_log("gatt: relay write from an unauthenticated connection dropped (%lu)",
                                        (unsigned long)relay_unauth);
                        break;
                    }
                    if (relay_owner < 0) {
                        relay_owner = i;
                        oz_ble_connect_as(&relay_ble, 1);
                        oz_ble_set_mtu(&relay_ble, conns[i].mtu);
                        oz_port_log("gatt: relay owned by connection %u", (unsigned)m.conn);
                        applied = -1;   /* this connection no longer makes the lock busy */
                    }
                    if (relay_owner == i)
                        (void)oz_ble_rx(&relay_ble, m.data, m.len);
                    else if (++relay_refused == 1 || relay_refused % 100 == 0)
                        oz_port_log("gatt: relay write from a non-owner dropped (%lu)",
                                    (unsigned long)relay_refused);
                }
                refresh_info(L);
                break;
            case EV_PAIR_CODE: {
                oz_ble *s = pairing_session();
                if (s)
                    (void)oz_ble_pair_code(s, m.arg);
                refresh_info(L);
                break;
            }
            case EV_DISCONNECT:
                if (i >= 0) {
                    oz_ble_disconnect(&conns[i].ble);
                    if (i == relay_owner) {
                        oz_ble_disconnect(&relay_ble);  /* half-done EDHOC + Echo go; contexts stay */
                        relay_owner = -1;
                    }
                    conns[i].used = 0;
                    applied = -1;
                }
                break;
            case EV_TOUCH:
                oz_adv_touch(&adv, now_ms());
                break;
            case EV_KEY:
                key_in(L, (char)m.arg);
                break;
            case EV_CARD:
                card_in(L, m.data, m.len);
                break;
            default:
                break;
            }
            int up = 0;
            for (int j = 0; j < MAX_CONNS; j++)
                up += conns[j].used;
            conn_up = up;
            /* An owner that took `relay` under the rev 6 rule loses it as soon
             * as a bridge is enrolled and it isn't that bridge. */
            if (relay_owner >= 0 && !oz_lock_relay_may_own(L, &conns[relay_owner].link)) {
                oz_ble_disconnect(&relay_ble);
                relay_owner = -1;
                oz_port_log("gatt: relay released: its owner is not an authenticated bridge");
            }
            adv.links = app_links(L);
            if (up >= MAX_CONNS && adv.links == 0)
                adv.links = 1;          /* both slots taken: never connectable */
        }
        for (int j = 0; j < MAX_CONNS; j++)
            if (conns[j].used)
                (void)oz_ble_tick(&conns[j].ble);
        /* F1-BRIDGE-BOND-1 (B): free the app slot from a connection that never
         * authenticates (a rogue, or an unenrolled idle bridge). */
        for (int j = 0; j < MAX_CONNS; j++)
            if (conns[j].used && !conns[j].kicked &&
                oz_lock_link_squatting(L, &conns[j].link, j == relay_owner, (uint32_t)(now_ms() - conns[j].at))) {
                conns[j].kicked = 1;
                ++squat_drops;
                oz_port_log("gatt: connection %u dropped: no authenticated session in %u s (%lu so far)",
                            (unsigned)conns[j].h, OZ_LOCK_AUTH_GRACE_MS / 1000u,
                            (unsigned long)squat_drops);
                server->disconnect(conns[j].h);
            }
        if (relay_owner >= 0)
            (void)oz_ble_tick(&relay_ble);
        {
            int bl = relay_owner >= 0 ? 1 : 0;
            for (int j = 0; j < MAX_CONNS; j++)
                if (conns[j].used && oz_lock_link_is_bridge(L, &conns[j].link))
                    bl = 2;
            bridge_link = bl;
        }
        motor_step(L);
        if (oz_keypad_tick(&keypad, now_ms()) == OZ_KP_CLEARED)
            oz_port_log("keypad: entry timed out");
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

/* ── public ──────────────────────────────────────────────────────────────── */

extern "C" void oz_gatt_c6_ble_addr(uint8_t out[6]) { esp_read_mac(out, ESP_MAC_BT); }

extern "C" int oz_gatt_c6_connected(void) { return conn_up; }
extern "C" int oz_gatt_c6_bridge_link(void) { return bridge_link; }

extern "C" int oz_gatt_c6_pair_code(uint32_t code) { post(EV_PAIR_CODE, code, NULL, 0); return OZ_OK; }
extern "C" int oz_gatt_c6_key(char key) { post(EV_KEY, (uint8_t)key, NULL, 0); return OZ_OK; }
extern "C" int oz_gatt_c6_touch(void) { post(EV_TOUCH, 0, NULL, 0); return OZ_OK; }

extern "C" int oz_gatt_c6_card(const uint8_t *id, size_t len)
{
    if (len == 0 || len > OZ_CARD_ID_MAX)
        return OZ_E_ARG;
    post(EV_CARD, 0, id, len);
    return OZ_OK;
}

extern "C" int oz_gatt_c6_start(oz_lock *L)
{
    static int started;
    if (started)
        return OZ_E_HW;
    mq = xQueueCreate(QUEUE_DEPTH, sizeof(msg_t));
    if (!mq)
        return OZ_E_HW;
    for (int i = 0; i < MAX_CONNS; i++) {
        tx_session[i].chr = &chr_ozkie;
        tx_session[i].idx = i;
        oz_ble_init(&conns[i].ble, L, &conns[i].link, notify, &tx_session[i]);
    }
    oz_ble_init(&relay_ble, L, &relay_link_, notify, &tx_relay);
    oz_adv_init(&adv, OZ_GATT_ADV_ALWAYS, now_ms());
    oz_keypad_init(&keypad);
    oz_motor_init(&motor, NULL);
    oz_card_init(&card);

    BLEDevice::init("OZLOCK");
    BLEDevice::setMTU(247);             /* ozkey-48 §2.1 */
    server = BLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    BLEService *svc = server->createService(BLEUUID((uint8_t *)OZ_BLE_UUID_SERVICE, 16, false));
    chr_info = svc->createCharacteristic(BLEUUID((uint8_t *)OZ_BLE_UUID_INFO, 16, false),
                                         BLECharacteristic::PROPERTY_READ);
    chr_ozkie = svc->createCharacteristic(BLEUUID((uint8_t *)OZ_BLE_UUID_OZKIE, 16, false),
                                          BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    chr_relay = svc->createCharacteristic(BLEUUID((uint8_t *)OZ_BLE_UUID_RELAY, 16, false),
                                          BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    chr_ozkie->setCallbacks(new FrameCb(CH_OZKIE));   /* NimBLE adds the CCCDs itself */
    chr_relay->setCallbacks(new FrameCb(CH_RELAY));
    svc->start();
    started = 1;
    if (xTaskCreate(ozkie_task, "ozkie", OZ_GATT_C6_STACK, L, 5, NULL) != pdPASS)
        return OZ_E_HW;
    return OZ_OK;
}
