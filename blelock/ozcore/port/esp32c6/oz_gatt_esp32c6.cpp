/* oz_gatt_esp32c6 — see oz_gatt_esp32c6.h.
 *
 * Arduino-ESP32 3.3.11 BLE library. On the C6 it builds the NimBLE backend
 * (CONFIG_NIMBLE_ENABLED, confirmed for the v1 sketches in
 * ozdoorlock_core.h); the Bluedroid callbacks are kept for other targets.
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

#if defined(CONFIG_NIMBLE_ENABLED)
#define OZ_ADV_TYPE_IND      BLE_GAP_CONN_MODE_UND /* connectable */
#define OZ_ADV_TYPE_SCAN_IND BLE_GAP_CONN_MODE_NON /* scannable, not connectable */
#else
#define OZ_ADV_TYPE_IND      0x00
#define OZ_ADV_TYPE_SCAN_IND 0x02
#endif

/* ── BLE callbacks → OZKIE task ──────────────────────────────────────────── */

enum { EV_CONNECT = 1, EV_DISCONNECT, EV_MTU, EV_FRAME, EV_PAIR_CODE, EV_TOUCH, EV_KEY, EV_CARD };

#define FRAME_MAX 244                   /* MTU 247 − 3 (ozkey-48 §2.1) */
typedef struct {
    uint8_t  ev;
    uint16_t len;
    uint32_t arg;
    uint8_t  data[FRAME_MAX];
} msg_t;

#define QUEUE_DEPTH 8
static QueueHandle_t mq;
static volatile int conn_up;

static void post(uint8_t ev, uint32_t arg, const uint8_t *data, size_t len)
{
    static msg_t m;                     /* callers are serialised per source; see below */
    msg_t local;
    msg_t *p = (ev == EV_FRAME) ? &m : &local; /* frames come from the BLE host task only */
    p->ev = ev;
    p->arg = arg;
    p->len = (uint16_t)len;
    if (len)
        memcpy(p->data, data, len);
    if (xQueueSend(mq, p, 0) != pdTRUE)
        oz_port_log("gatt: queue full, event %u dropped", (unsigned)ev);
}

static BLEServer *server;
static BLECharacteristic *chr_info, *chr_ozkie;

class ServerCb : public BLEServerCallbacks {
#if defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer *, ble_gap_conn_desc *) override { conn_up = 1; post(EV_CONNECT, 0, NULL, 0); }
    void onDisconnect(BLEServer *, ble_gap_conn_desc *) override { conn_up = 0; post(EV_DISCONNECT, 0, NULL, 0); }
    void onMtuChanged(BLEServer *, ble_gap_conn_desc *, uint16_t mtu) override { post(EV_MTU, mtu, NULL, 0); }
#else
    void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *) override { conn_up = 1; post(EV_CONNECT, 0, NULL, 0); }
    void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *) override { conn_up = 0; post(EV_DISCONNECT, 0, NULL, 0); }
    void onMtuChanged(BLEServer *, esp_ble_gatts_cb_param_t *p) override { post(EV_MTU, p->mtu.mtu, NULL, 0); }
#endif
};

class OzkieCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override
    {
        size_t n = c->getLength();
        if (n == 0 || n > FRAME_MAX)
            return;                     /* the reassembler would refuse it anyway */
        post(EV_FRAME, 0, c->getData(), n);
    }
};

/* ── the OZKIE task ──────────────────────────────────────────────────────── */

static oz_link link_;
static oz_ble session;
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

static int notify(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (!conn_up)
        return OZ_E_HW;
    chr_ozkie->setValue((uint8_t *)frame, len);
    chr_ozkie->notify();
    /* The library gives no status; a short yield keeps a burst of fragments
     * from running the host's buffers dry (AK1037 retries BUFFER_FULL). */
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

static void key_in(oz_lock *L, char key, int connected)
{
    uint32_t code = 0;
    switch (oz_keypad_key(&keypad, key, now_ms(), &code)) {
    case OZ_KP_PIN: {
        int r = oz_lock_keypad_pin(L, keypad.entry);
        oz_keypad_done(&keypad);
        oz_port_log("keypad: %s", r == OZ_PIN_OPEN ? "open" : r == OZ_PIN_LOCKED_OUT ? "locked out" : "wrong");
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

static void ozkie_task(void *param)
{
    oz_lock *L = (oz_lock *)param;
    static msg_t m;
    int connected = 0, applied = -1, failed = 0;
    refresh_info(L);
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(motor.phase == OZ_MOTOR_IDLE ? 100 : 10);
        if (xQueueReceive(mq, &m, wait) == pdTRUE) {
            switch (m.ev) {
            case EV_CONNECT:
                oz_ble_connect(&session);
                if (!connected)
                    oz_adv_link(&adv, 1);
                connected = 1;
                break;
            case EV_MTU:
                oz_ble_set_mtu(&session, m.arg);
                break;
            case EV_FRAME:
                if (connected)
                    (void)oz_ble_rx(&session, m.data, m.len);
                refresh_info(L);
                break;
            case EV_PAIR_CODE:
                if (connected)
                    (void)oz_ble_pair_code(&session, m.arg);
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
                key_in(L, (char)m.arg, connected);
                break;
            case EV_CARD:
                card_in(L, m.data, m.len);
                break;
            default:
                break;
            }
        }
        if (connected)
            (void)oz_ble_tick(&session);
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
    oz_ble_init(&session, L, &link_, notify, NULL);
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
#if !defined(CONFIG_NIMBLE_ENABLED)
    chr_ozkie->addDescriptor(new BLE2902());   /* NimBLE adds the CCCD itself */
#endif
    chr_ozkie->setCallbacks(new OzkieCb());
    svc->start();
    started = 1;
    if (xTaskCreate(ozkie_task, "ozkie", OZ_GATT_C6_STACK, L, 5, NULL) != pdPASS)
        return OZ_E_HW;
    return OZ_OK;
}
