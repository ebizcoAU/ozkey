/* oz_gatt_ak1037 — the OZKIE GATT service and the OZKIE thread on AK1037
 * (ozkey-48 §2, ozkey-49 P4).
 *
 * The BLE stack's callbacks only copy data into a message queue. The OZKIE
 * thread owns the oz_lock and does all the work (EDHOC, OSCORE, kv), so its
 * stack, not the BLE stack's, takes the engine's deep frames. Anything else
 * that touches the lock (keypad, RFID) must post here too, never call oz_lock
 * directly. */
#ifndef OZ_GATT_AK1037_H
#define OZ_GATT_AK1037_H

#include <stdint.h>

#include "oz_lock.h"

/* 12 KB until measured on silicon: worst static chain 6,160 B with littlefs
 * (ozkey-49 P5-D), plus indirect calls and littlefs recursion. */
#define OZ_GATT_THREAD_STACK (12 * 1024)

/* 1 = always advertising, for a lock its bridge must reach at any moment (the
 * CH584M ruling, ozdoorlock_core.h bleAdvertisingAllowed()); 0 = CONTRACT.md's
 * touch window plus the unclaimed post-boot grace. oz_adv.h has both rules. */
#ifndef OZ_GATT_ADV_ALWAYS
#define OZ_GATT_ADV_ALWAYS 1
#endif

/* Advertising interval in 0.625 ms units: 800 = 500 ms. Until measured (P6). */
#ifndef OZ_GATT_ADV_INTERVAL
#define OZ_GATT_ADV_INTERVAL 800
#endif

/* Bring up BLE (server role), register the service, start the OZKIE thread.
 * L must already be initialised with oz_lock_init(). Returns OZ_OK or OZ_E_HW. */
int oz_gatt_ak1037_start(oz_lock *L, uint8_t thread_priority);

/* From the keypad, RFID or fingerprint thread: someone is at the door. Opens
 * (or re-arms) the 60 s advertising window. */
int oz_gatt_ak1037_touch(void);

/* From the keypad thread: the owner typed the 6-digit pairing code. */
int oz_gatt_ak1037_pair_code(uint32_t code);

/* From the keypad driver: one key, '0'..'9', '*' or '#'. The OZKIE thread
 * runs the entry state machine (oz_keypad.h) and checks the PIN there. */
int oz_gatt_ak1037_key(char key);

/* The lock's pair_prompt hook, which runs on the OZKIE thread: while on, an
 * entry on the keypad is the pairing code. */
void oz_gatt_ak1037_pair_prompt(int on);

/* The lock's actuate hook, also on the OZKIE thread: start (or queue) a bolt
 * move (oz_motor.h). The thread steps the motor and reports each finished
 * move to oz_lock_actuated(). */
void oz_gatt_ak1037_actuate(int unlock);

#endif
