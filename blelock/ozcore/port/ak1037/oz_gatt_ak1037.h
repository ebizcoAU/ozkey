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

/* 12 KB until measured on silicon: worst static chain 6,128 B with littlefs
 * (ozkey-49 P4-prep item 2), plus indirect calls and littlefs recursion. */
#define OZ_GATT_THREAD_STACK (12 * 1024)

/* Bring up BLE (server role), register the service, start the OZKIE thread.
 * L must already be initialised with oz_lock_init(). Returns OZ_OK or OZ_E_HW. */
int oz_gatt_ak1037_start(oz_lock *L, uint8_t thread_priority);

/* From the keypad thread: the owner typed the 6-digit pairing code. */
int oz_gatt_ak1037_pair_code(uint32_t code);

#endif
