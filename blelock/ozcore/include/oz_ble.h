/* oz_ble — one BLE GATT connection to the lock engine (ozkey-48 §2).
 *
 * Glues the `ozkie` characteristic to oz_lock: frames written by the central
 * are reassembled (oz_frame), each whole message goes to oz_lock_rx, and the
 * reply is fragmented at the connection's ATT MTU and handed to the port's tx
 * function, one frame per notification. Portable: the port supplies tx and
 * calls these from ONE thread (the OZKIE thread), never from the BLE stack's
 * callback context, so the engine's large stack frames stay on that thread.
 *
 * The bridge writes the lock's `relay` characteristic through a second oz_ble
 * bound to the relay link (ozkey-48 §2.3); over MQTT a message is one payload.
 */
#ifndef OZ_BLE_H
#define OZ_BLE_H

#include <stddef.h>
#include <stdint.h>

#include "oz_frame.h"
#include "oz_lock.h"

/* GATT UUIDs (ozkey-48 §2), as 16 little-endian bytes, the order BLE stacks
 * (the AK1037 SDK included) store them in:
 *   service 4f5a4b32-0001-4c4f-434b-000000000001
 *   info    4f5a4b32-0002-4c4f-434b-000000000001   read
 *   ozkie   4f5a4b32-0003-4c4f-434b-000000000001   write + notify (session links)
 *   relay   4f5a4b32-0004-4c4f-434b-000000000001   write + notify (the relay link, rev 6 §2.3) */
#define OZ_BLE_UUID_LE(n) "\x01\x00\x00\x00\x00\x00\x4b\x43\x4f\x4c" n "\x00\x32\x4b\x5a\x4f"
#define OZ_BLE_UUID_SERVICE OZ_BLE_UUID_LE("\x01")
#define OZ_BLE_UUID_INFO    OZ_BLE_UUID_LE("\x02")
#define OZ_BLE_UUID_OZKIE   OZ_BLE_UUID_LE("\x03")
#define OZ_BLE_UUID_RELAY   OZ_BLE_UUID_LE("\x04")

/* Send one frame as a notification. Returns OZ_OK or an OZ_E_* code. */
typedef int (*oz_ble_tx)(void *ctx, const uint8_t *frame, size_t len);

typedef struct {
    oz_lock   *L;
    oz_link   *link;
    oz_reasm   rx;
    size_t     mtu;                     /* ATT MTU of this connection */
    oz_ble_tx  tx;
    void      *tx_ctx;
    uint8_t    out[OZ_FRAME_MAX_MSG];
} oz_ble;

void oz_ble_init(oz_ble *s, oz_lock *L, oz_link *link, oz_ble_tx tx, void *tx_ctx);

/* A central connected: a fresh session link at the default MTU. */
void oz_ble_connect(oz_ble *s);
/* The same for either kind of link (ozkey-48 §2.3): relay = 1 for the `relay`
 * characteristic, whose link keeps its contexts per bond in L, not here. */
void oz_ble_connect_as(oz_ble *s, int relay);
/* The ATT MTU exchange finished (clamped to 23..517). */
void oz_ble_set_mtu(oz_ble *s, size_t mtu);
void oz_ble_disconnect(oz_ble *s);

/* One frame written to `ozkie`. A malformed frame is dropped, the partial
 * message with it; nothing is sent back, so a stranger learns nothing. */
int  oz_ble_rx(oz_ble *s, const uint8_t *frame, size_t len);
/* The keypad pairing code and the periodic tick may each produce a reply; the
 * tick also sends up to 4 pending `events` notifications. */
int  oz_ble_pair_code(oz_ble *s, uint32_t code);
int  oz_ble_tick(oz_ble *s);

/* Fragment and send one whole message. OZ_E_NO_SPACE if it needs more than 32
 * frames at this MTU (over 608 bytes at MTU 23). */
int  oz_ble_send(oz_ble *s, uint8_t type, const uint8_t *msg, size_t len);

#endif
