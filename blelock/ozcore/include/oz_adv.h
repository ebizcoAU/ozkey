/* oz_adv — what the lock advertises, and when (CONTRACT.md "Advertising",
 * M3 note 5, [XF-47] busy flag; ozkey-48 §2 keeps all of it for v2).
 *
 * Portable: the policy is a pure function of time and state, and the packet
 * bytes are built here, so both are host-tested. The port applies the result
 * (on AK1037: oz_gatt_ak1037.c, raw bytes through the SDK's custom_data).
 *
 *   ADV (29 of 31 B): flags 06 · complete 128-bit UUID list (service) · name "OZLOCK"
 *   scan response (23 of 31 B): 128-bit service data = busy byte ‖ 4-byte short id
 *
 * The v2 service UUID (…4b32) is how a bridge tells a v2 lock from a v1 one
 * (ozkey-48 §7). The UUID list stays in ADV because apps filter on it
 * (Android `withServices`), so the busy byte goes in the scan response and the
 * app must scan actively, exactly as on v1.
 *
 * Modes:
 *   OFF   not advertising
 *   OPEN  connectable (ADV_IND), busy = 0
 *   BUSY  a link is up: scannable but NOT connectable (ADV_SCAN_IND), busy = 1.
 *         The link layer refuses a second central; that IS the single-connection
 *         rule, and the busy byte says why the lock refuses.
 *
 * When it advertises at all:
 *   - `always` (a lock its bridge must reach at any moment: the CH584M ruling,
 *     ozdoorlock_core.h bleAdvertisingAllowed(), OZ_NO_MCU branch): always;
 *   - otherwise: inside a touch window (60 s, re-armed by every touch), or
 *     unclaimed and inside the 120 s post-boot grace. A claimed lock outside
 *     its window is dark. A window that expires during a link ends at the
 *     disconnect, not before. */
#ifndef OZ_ADV_H
#define OZ_ADV_H

#include <stddef.h>
#include <stdint.h>

#define OZ_ADV_WINDOW_MS 60000u
#define OZ_ADV_BOOT_MS   120000u
#define OZ_ADV_MAX       31
#define OZ_ADV_NAME      "OZLOCK"

typedef enum { OZ_ADV_OFF = 0, OZ_ADV_OPEN, OZ_ADV_BUSY } oz_adv_mode;

typedef struct {
    int      always;
    uint32_t boot_until;                /* ms; valid while in_boot */
    int      in_boot;
    uint32_t window_until;              /* ms; valid while window_set */
    int      window_set;
    int      links;
} oz_adv;

/* now_ms is any free-running millisecond clock; wrap-around is handled. */
void        oz_adv_init(oz_adv *a, int always, uint32_t now_ms);
void        oz_adv_touch(oz_adv *a, uint32_t now_ms);
void        oz_adv_link(oz_adv *a, int up);
oz_adv_mode oz_adv_mode_at(oz_adv *a, int claimed, uint32_t now_ms);

/* Packet bytes. device_id "ozk-acebe63acab8" → short id ac eb e6 3a, the same
 * 8 hex characters the panel prints. Returns the length, 0 on a bad id. */
size_t oz_adv_data(uint8_t out[OZ_ADV_MAX]);
size_t oz_adv_scan_rsp(uint8_t out[OZ_ADV_MAX], int busy, const char *device_id);

#endif
