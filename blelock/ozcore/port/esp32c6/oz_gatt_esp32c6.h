/* oz_gatt_esp32c6 — the OZKIE GATT service and the OZKIE task on ESP32-C6
 * (ozkey-55 F5). The same design as oz_gatt_ak1037: BLE callbacks only copy
 * into a queue; one task owns the oz_lock and does all the work. Anything
 * else that touches the lock (keypad, card, shell) posts here too. */
#ifndef OZ_GATT_ESP32C6_H
#define OZ_GATT_ESP32C6_H
#include <stddef.h>
#include <stdint.h>
#include "oz_lock.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The RV32 static worst case is 6,384 B from the OZKIE thread's entry
 * (ozkey-49 P5-D); NVS is shallower than littlefs. 12 KB, as on AK1037. */
#define OZ_GATT_C6_STACK (12 * 1024)

/* 1 = always advertising: a lock its bridge must reach at any moment (as on
 * AK1037). 0 = the touch window + unclaimed boot grace (oz_adv.h). */
#ifndef OZ_GATT_ADV_ALWAYS
#define OZ_GATT_ADV_ALWAYS 1
#endif
/* Advertising interval in 0.625 ms units: 800 = 500 ms. */
#ifndef OZ_GATT_ADV_INTERVAL
#define OZ_GATT_ADV_INTERVAL 800
#endif

/* BLE must not be initialised yet: this calls BLEDevice::init(), registers the
 * service, then starts the task. L must already be oz_lock_init()ed.
 * OZ_OK or OZ_E_HW. */
int oz_gatt_c6_start(oz_lock *L);

/* The BLE address the lock advertises, most significant byte first. Valid
 * before oz_gatt_c6_start() (read from eFuse). */
void oz_gatt_c6_ble_addr(uint8_t out[6]);

int oz_gatt_c6_touch(void);
int oz_gatt_c6_pair_code(uint32_t code);
int oz_gatt_c6_key(char key);
int oz_gatt_c6_card(const uint8_t *id, size_t len);

/* Lock hooks, which run on the OZKIE task. */
void oz_gatt_c6_pair_prompt(int on);
void oz_gatt_c6_actuate(int unlock);

/* 1 while a central is connected (a racy read, for the shell). */
int oz_gatt_c6_connected(void);

/* The bridge link, for the display (a racy read): 0 none, 1 a central owns
 * `relay` without bridge authentication (rev 6 rule, no bridge bond enrolled),
 * 2 an enrolled bridge bond is connected and authenticated (F1-BRIDGE-BOND-1). */
int oz_gatt_c6_bridge_link(void);

#ifdef __cplusplus
}
#endif
#endif
