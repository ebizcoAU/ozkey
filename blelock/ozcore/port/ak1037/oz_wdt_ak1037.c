/* oz_wdt_ak1037 — see oz_wdt_ak1037.h. */
#include "oz_wdt_ak1037.h"

#include "hal_sys.h"
#include "hal_watchdog.h"
#include "hal_sysclock.h"
#include "hal_lpc.h"

#include "oz_port.h"
#include "oz_wdt.h"
#include "oz_port_ak1037.h"

#define BG_KEY "bg"

static oz_wdt sup;
static int main_id = -1;
static uint8_t bg_count;
static int safe;
static int booted;
static int late_logged = -1;
static uint32_t boot_ms;

static uint32_t now32(void) { return (uint32_t)get_ms_tick_func(); }

static int cause_now(void)
{
    if (hal_lpc_get_boot_state() != 0)
        return OZ_RST_OTHER;            /* a wake-up, not a reset */
    switch (hal_clock_get_reset_status()) {
    case HAL_AO_POR_RST:
    case HAL_AO_HW_RST:                 /* power-on, or the reset pin: a person */
        return OZ_RST_POWER;
    case HAL_AO_WDT_RST:
    case HAL_CORE_WDT_RST:
        return OZ_RST_WDT;
    case HAL_SOFT_RESET_RST:
        return OZ_RST_SOFT;
    default:
        return OZ_RST_OTHER;
    }
}

static void bg_save(void)
{
    /* The self-test wipes and switches kv namespaces: stay out of its way. */
    if (oz_ak1037_kv_owner() == OZ_AK1037_KV_SELFTEST)
        return;
    if (oz_port_kv_set(BG_KEY, &bg_count, 1) != OZ_OK)
        oz_port_log("wdt: boot count not saved (kv)");
}

int oz_ak1037_wdt_boot(void)
{
    static const char *const cause_name[] = { "power-on", "WATCHDOG", "soft", "other" };
    if (booted)
        return safe;
    booted = 1;
    boot_ms = now32();
    oz_wdt_init(&sup);

    size_t len = 0;
    uint8_t v = 0;
    if (oz_port_kv_get(BG_KEY, &v, 1, &len) == OZ_OK && len == 1)
        bg_count = v;
    int cause = cause_now();
    int dirty = 0;
    safe = oz_bootguard_boot(&bg_count, cause, &dirty);
    if (dirty)
        bg_save();
    oz_port_log("wdt: reset %s, watchdog resets in a row %u%s", cause_name[cause & 3],
                (unsigned)bg_count, safe ? ": SAFE MODE (motor off, BLE and owner only)" : "");

    main_id = oz_wdt_add(&sup, OZ_AK1037_WDT_MAIN_MS, boot_ms);
    hal_wdt_start(WATCHDOG_12P8S);      /* the SDK's own boot setting, kept on */
    hal_wdt_feed();
    return safe;
}

int oz_ak1037_wdt_add(uint32_t period_ms)
{
    int id = oz_wdt_add(&sup, period_ms, now32());
    if (id < 0)
        oz_port_log("wdt: no room for another task");
    return id;
}

void oz_ak1037_wdt_checkin(int id) { oz_wdt_checkin(&sup, id, now32()); }

void oz_ak1037_wdt_service(void)
{
    if (!booted)
        return;
    uint32_t now = now32();
    oz_wdt_checkin(&sup, main_id, now);
    int late;
    if (oz_wdt_may_feed(&sup, now, &late)) {
        hal_wdt_feed();
        late_logged = -1;
    } else if (late != late_logged) {
        late_logged = late;
        oz_port_log("wdt: task %d is late: no more feeds, reset in under 12.8 s", late);
    }
    if (oz_bootguard_stable(&bg_count, now - boot_ms, safe))
        bg_save();
}

int oz_ak1037_wdt_safe(void) { return safe; }

uint32_t oz_ak1037_wdt_resets(void) { return bg_count; }
