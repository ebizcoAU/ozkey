/* oz_adv — see oz_adv.h. */
#include <string.h>

#include "oz_adv.h"
#include "oz_ble.h"
#include "oz_crypto.h"
#include "oz_invite.h"

static int before(uint32_t now, uint32_t deadline) { return (int32_t)(deadline - now) > 0; }

void oz_adv_init(oz_adv *a, int always, uint32_t now_ms)
{
    memset(a, 0, sizeof *a);
    a->always = always;
    a->boot_until = now_ms + OZ_ADV_BOOT_MS;
    a->in_boot = 1;
}

/* Every touch re-arms the full window, during a link too: a long enrolment
 * must not have its window expire underneath it. */
void oz_adv_touch(oz_adv *a, uint32_t now_ms)
{
    a->window_until = now_ms + OZ_ADV_WINDOW_MS;
    a->window_set = 1;
}

void oz_adv_link(oz_adv *a, int up)
{
    if (up)
        a->links++;
    else if (a->links > 0)
        a->links--;
}

oz_adv_mode oz_adv_mode_at(oz_adv *a, int claimed, uint32_t now_ms)
{
    /* A link exists only because we advertised; it stays visible as busy for
     * as long as it lasts, whatever the window did meanwhile. */
    if (a->links > 0)
        return OZ_ADV_BUSY;
    if (a->window_set && !before(now_ms, a->window_until))
        a->window_set = 0;              /* expired, or ended with its link */
    if (a->always || a->window_set)
        return OZ_ADV_OPEN;
    /* Once the grace has passed, stop comparing, so a 49-day millisecond
     * wrap cannot reopen it. */
    if (a->in_boot && !before(now_ms, a->boot_until))
        a->in_boot = 0;
    return !claimed && a->in_boot ? OZ_ADV_OPEN : OZ_ADV_OFF;
}

static const char service_uuid[] = OZ_BLE_UUID_SERVICE;   /* 16 B + NUL */

size_t oz_adv_data(uint8_t out[OZ_ADV_MAX])
{
    uint8_t *p = out;
    *p++ = 2;  *p++ = 0x01; *p++ = 0x06;                /* LE general, no BR/EDR */
    *p++ = 17; *p++ = 0x07;                             /* complete 128-bit UUIDs */
    memcpy(p, service_uuid, 16);
    p += 16;
    *p++ = (uint8_t)(sizeof OZ_ADV_NAME);  *p++ = 0x09; /* complete local name */
    memcpy(p, OZ_ADV_NAME, sizeof OZ_ADV_NAME - 1);
    p += sizeof OZ_ADV_NAME - 1;
    return (size_t)(p - out);
}

/* Short id = SHA-256(device_id)[0..3] (ozkey-48 §2.2, PM ruling 2026-11-05):
 * the device_id's bytes exactly as `info` key 1 carries them, no case folding.
 * A hash spreads evenly whatever the vendor's OUI, and doesn't put it on air. */
size_t oz_adv_scan_rsp(uint8_t out[OZ_ADV_MAX], int busy, const char *device_id)
{
    size_t n = 0;                                       /* strnlen isn't C11 */
    while (device_id && n <= OZ_INVITE_MAX_DEVICE_ID && device_id[n])
        n++;
    uint8_t h[32];
    if (n == 0 || n > OZ_INVITE_MAX_DEVICE_ID || oz_sha256((const uint8_t *)device_id, n, h) != 0)
        return 0;
    uint8_t *p = out;
    *p++ = 1 + 16 + 1 + 4;  *p++ = 0x21;                /* service data, 128-bit */
    memcpy(p, service_uuid, 16);
    p += 16;
    *p++ = busy ? 0x01 : 0x00;
    memcpy(p, h, 4);
    p += 4;
    return (size_t)(p - out);
}
