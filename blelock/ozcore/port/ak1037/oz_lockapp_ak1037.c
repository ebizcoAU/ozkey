/* oz_lockapp_ak1037 — the lock image's shell commands (ozkey-49 P4 item 4b):
 * the real OZKIE lock with log-only hardware, for bring-up before the keypad
 * and motor are wired (P5). The motor's state machine runs (P5-C) against a
 * log-only driver, so every move reports "unconfirmed".
 *
 *   oz.start     oz_lock_init + the OZKIE GATT service.
 *   oz.touch     someone is at the door (opens the advertising window).
 *   oz.code N    the owner typed pairing code N on the keypad.
 *   oz.key K…    keys pressed on the keypad, e.g. "oz.key 4826#" ('*' clears).
 *   oz.card HEX  a card at the reader, e.g. "oz.card 04a1b2c3d4e5f6" (its UID).
 *   oz.state     the lock state and how the last motor move ended.
 *
 * MSH runs commands on the shell thread's small stack, so these only start
 * or post to the OZKIE thread. Its priority is compile-time until the kit
 * shows what the SDK's own threads use.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtthread.h"
#include "com_msh.h"
#include "com_ble.h"

#include "oz_port.h"
#include "oz_lock.h"
#include "oz_gatt_ak1037.h"
#include "oz_board_ak1037.h"
#include "oz_port_ak1037.h"
#ifdef OZ_DEBUG_HOOKS
#include "oz_kv_lfs.h"
#endif

#ifndef OZ_START_PRIORITY
#define OZ_START_PRIORITY (RT_THREAD_PRIORITY_MAX / 2)
#endif
/* REL-VER-1: "ozkie-ak1037-<semver>+<git sha7>" in INFO and dev/info, so a
 * test page can assert exactly which build runs (CN-LAB-1 T3). The build host
 * writes port/ak1037/oz_build_sha.h (#define OZ_BUILD_SHA "abc1234", not in
 * git) from the commit it builds; without it the image says "+nosha". */
#if defined(__has_include)
#if __has_include("oz_build_sha.h")
#include "oz_build_sha.h"
#endif
#endif
#ifndef OZ_BUILD_SHA
#define OZ_BUILD_SHA "nosha"
#endif
#define OZ_LOCKAPP_SEMVER "0.1.0"
#define OZ_LOCKAPP_FW "ozkie-ak1037-" OZ_LOCKAPP_SEMVER "+" OZ_BUILD_SHA

static volatile int running;

static oz_lock lock;

static void actuate(void *app, int unlock)
{
    (void)app;
    oz_port_log("actuate: %s", unlock ? "UNLOCK" : "lock");
    oz_gatt_ak1037_actuate(unlock);
}

static void log_prompt(void *app, int on)
{
    (void)app;
    oz_gatt_ak1037_pair_prompt(on);
    oz_port_log("keypad: %s", on ? "enter the pairing code on the keypad (NNNNNN#)" : "pairing prompt off");
}

int oz_lockapp_ak1037_start(void)
{
    static oz_lock_hooks hooks = { actuate, log_prompt, NULL, NULL };
    hooks.model = oz_ak1037_board()->model;     /* BOARD-PROFILE-1 */
    char id[4 + 12 + 1];

    if (running) {
        printf("oz.start: already running\r\n");
        return OZ_E_ARG;
    }
    /* The self-test wipes kv between cases: no lock after it this boot. */
    if (oz_ak1037_kv_claim(OZ_AK1037_KV_LOCK) != OZ_OK) {
        printf("oz.start: reboot after oz.selftest\r\n");
        return OZ_E_ARG;
    }
    /* ozk- + the BLE address, most significant byte first. UNVERIFIED: the
     * byte order com_ble_get_dev_addr() returns, against what a phone scans.
     * The stack sets the address in its own init, so bring it up first: read
     * before that, it is all zeros, and every unit was ozk-000000000000
     * (LOCK-ID-1, first chip run). A zero address is never an ID. */
    if (oz_gatt_ak1037_radio_up() != OZ_OK) {
        printf("oz.start: BLE init failed; reboot\r\n");
        return OZ_E_HW;
    }
    const uint8_t *a = com_ble_get_dev_addr();
    static const uint8_t zero[6];
    if (!a || !memcmp(a, zero, sizeof zero)) {
        printf("oz.start: no BLE address yet; try again\r\n");
        return OZ_E_HW;
    }
    snprintf(id, sizeof id, "ozk-%02x%02x%02x%02x%02x%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    if (oz_lock_init(&lock, id, OZ_LOCKAPP_FW, &hooks) != OZ_OK) {
        printf("oz.start: oz_lock_init failed\r\n");
        return OZ_E_HW;
    }
    /* KV-LAT-1: a kv write can take ~530 ms on this NOR, so the limit
     * writes follow the beep (the OZKIE thread flushes at once). */
    oz_lock_defer_saves(&lock, 1);
    running = 1;
    if (oz_gatt_ak1037_start(&lock, OZ_START_PRIORITY) != OZ_OK) {
        printf("oz.start: GATT start failed; reboot\r\n");
        return OZ_E_HW;
    }
    oz_ak1037_battery_note();
    printf("oz.start: %s %s\r\n", id, oz_lock_claimed(&lock) ? "claimed" : "UNCLAIMED");
    return OZ_OK;
}

static void oz_start_cmd(char argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)oz_lockapp_ak1037_start();
}

static void oz_touch_cmd(char argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!running || oz_gatt_ak1037_touch() != OZ_OK)
        printf("oz.touch: the lock isn't running (oz.start)\r\n");
}

#ifdef OZ_DEBUG_HOOKS
/* REL-SHELL-1 (PM 2026-11-20, security blocker): these three inject keypad
 * digits, a card UID or the pairing code from the UART, without the keypad or
 * the card. Bench (_dbg) images only; the release-binary scan (CMake +
 * link_app_mcu.ps1) fails any image that holds their names. */
static void oz_code_cmd(char argc, char **argv)
{
    if (argc != 2) {
        printf("usage: oz.code 123456\r\n");
        return;
    }
    if (!running || oz_gatt_ak1037_pair_code((uint32_t)strtoul(argv[1], NULL, 10)) != OZ_OK)
        printf("oz.code: the lock isn't running (oz.start)\r\n");
}

static void oz_key_cmd(char argc, char **argv)
{
    if (argc != 2) {
        printf("usage: oz.key 1234#   (digits, * clears, # enters)\r\n");
        return;
    }
    if (!running) {
        printf("oz.key: the lock isn't running (oz.start)\r\n");
        return;
    }
    for (const char *k = argv[1]; *k; k++)
        (void)oz_gatt_ak1037_key(*k);
}

static void oz_card_cmd(char argc, char **argv)
{
    uint8_t id[16];
    size_t n = 0;
    const char *h = argc == 2 ? argv[1] : "";
    for (; h[0] && h[1] && n < sizeof id; h += 2) {
        char b[3] = { h[0], h[1], 0 };
        char *end;
        unsigned long v = strtoul(b, &end, 16);
        if (*end)
            break;
        id[n++] = (uint8_t)v;
    }
    if (argc != 2 || *h || n == 0) {
        printf("usage: oz.card 04a1b2c3   (the card's UID in hex, 1..16 bytes)\r\n");
        return;
    }
    if (!running || oz_gatt_ak1037_card(id, n) != OZ_OK)
        printf("oz.card: the lock isn't running (oz.start)\r\n");
}
#endif /* OZ_DEBUG_HOOKS: REL-SHELL-1 */

/* Two bytes the OZKIE thread writes: a torn read can't happen, a stale one
 * only shows the state a moment late. */
static void oz_state_cmd(char argc, char **argv)
{
    static const char *const lock_name[] = { "not fully locked", "locked", "unlocked" };
    static const char *const move_name[] = { "none", "ok", "unconfirmed", "JAMMED" };
    (void)argc;
    (void)argv;
    if (!running) {
        printf("oz.state: the lock isn't running (oz.start)\r\n");
        return;
    }
    uint8_t s = lock.lock_state, m = lock.last_move;
    printf("oz.state: %s, last move %s\r\n", s < 3 ? lock_name[s] : "?", m < 4 ? move_name[m] : "?");
}

#ifdef OZ_DEBUG_HOOKS
/* Debug builds only (OZ_DEBUG_HOOKS, never in a release config): force a
 * watchdog reset. Three in a row with no power-on reset boot SAFE mode
 * (ozkey-48 §6.1 bench recipe). */
static void oz_wdthang_cmd(char argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!running) {
        printf("oz.wdthang: oz.start first\r\n");
        return;
    }
    printf("oz.wdthang: OZKIE thread stops checking in; watchdog reset in about 15 s\r\n");
    oz_gatt_ak1037_debug_hang();
}
AK_MSH_CMD_EXPORT(oz_wdthang_cmd, oz.wdthang, DEBUG BUILD ONLY: force a watchdog reset);

/* LED-MAP-1 (debug builds only): drive one J7 backlight pin high, low or
 * release it, so the bench learns which key each pin and level lights (each
 * pin drives a pair of key LEDs). For lighting only the pressed key. */
static void oz_led_cmd(char argc, char **argv)
{
    int st = -1;
    int i = argc >= 3 ? atoi(argv[1]) : -1;
    if (argc >= 3)
        st = !strcmp(argv[2], "h") ? OZ_BL_HIGH : !strcmp(argv[2], "l") ? OZ_BL_LOW
           : !strcmp(argv[2], "off") ? OZ_BL_OFF : -1;
    int g = (st < 0) ? -1 : oz_ak1037_backlight_pin(i, st);
    if (g < 0) {
        printf("usage: oz.led <0-7> <h|l|off>   (DEBUG BUILD ONLY: one J7 backlight pin)\r\n");
        return;
    }
    printf("oz.led: pin %d = GPIO%d %s\r\n", i, g, argv[2]);
}
AK_MSH_CMD_EXPORT(oz_led_cmd, oz.led, DEBUG BUILD ONLY: drive one J7 backlight pin);

/* BATT-SENSE-1 (debug builds only): the raw ADC on the battery channel and
 * the pin voltage it means, to measure the divider with a multimeter. */
static void oz_batt_cmd(char argc, char **argv)
{
    (void)argc;
    (void)argv;
    int raw = oz_ak1037_battery_raw();
    printf("oz.batt: raw %d, pin %u mV, pack %u mV (AIN5; 1-point cal 6340 mV; sense %s)\r\n", raw,
           (unsigned)oz_ak1037_battery_pin_mv(raw), (unsigned)oz_ak1037_battery_pack_mv(raw),
           oz_ak1037_battery_sensing() ? "on" : "OFF");
}
AK_MSH_CMD_EXPORT(oz_batt_cmd, oz.batt, DEBUG BUILD ONLY: raw battery ADC);

/* RFID-TIME-1 (debug builds only): the card-poll time histogram, for Anyka Q12. */
static void oz_rfidstat_cmd(char argc, char **argv)
{
    oz_gatt_ak1037_debug_rfidstat(argc >= 2 && !strcmp(argv[1], "reset"));
}
AK_MSH_CMD_EXPORT(oz_rfidstat_cmd, oz.rfidstat, DEBUG BUILD ONLY: card poll times [reset]);

/* AK-FLASH-LAT-1 (debug builds only): time N kv writes on the lock thread. */
static void oz_flashlat_cmd(char argc, char **argv)
{
    int n = argc >= 2 ? atoi(argv[1]) : 10;
    if (!running) {
        printf("oz.flashlat: the lock isn't running\r\n");
        return;
    }
    if (n < 1)
        n = 1;
    if (n > 50)
        n = 50;
    printf("oz.flashlat: %d writes queued on the lock thread\r\n", n);
    oz_gatt_ak1037_debug_flashlat(n);
}
AK_MSH_CMD_EXPORT(oz_flashlat_cmd, oz.flashlat, DEBUG BUILD ONLY: time N kv writes [N<=50]);

/* OZ-WIPE-1 (debug builds only, PM GO 2026-11-18): erase the lock's kv, its
 * identity and bonds, so the next oz.start is a fresh UNCLAIMED lock (bench:
 * a claimed lock back to the start of blerun without a reburn). Serial shell
 * only: no BLE path reaches the shell. Only before oz.start, so it never races
 * the running lock's own kv writes: reset (SW13), oz.wipe CONFIRM, oz.start. */
static void oz_wipe_cmd(char argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "CONFIRM")) {
        printf("usage: oz.wipe CONFIRM   (DEBUG BUILD ONLY: erases identity and bonds; before oz.start)\r\n");
        return;
    }
    if (running) {                      /* AUTOSTART-1: the lock runs from power-on */
        printf("oz.wipe: the lock runs: erasing from its thread, then restarting\r\n");
        oz_gatt_ak1037_debug_wipe_reset();
        return;
    }
    if (oz_ak1037_kv_claim(OZ_AK1037_KV_LOCK) != OZ_OK) {
        printf("oz.wipe: refused: reboot after oz.selftest\r\n");
        return;
    }
    if (oz_ak1037_kv_mount() != OZ_OK || oz_kv_lfs_namespace("oz") != OZ_OK || oz_kv_lfs_wipe() != OZ_OK) {
        printf("oz.wipe: FAILED (kv)\r\n");
        return;
    }
    printf("oz.wipe: identity and bonds erased; oz.start gives a fresh UNCLAIMED lock\r\n");
}
AK_MSH_CMD_EXPORT(oz_wipe_cmd, oz.wipe, DEBUG BUILD ONLY: erase identity and bonds before oz.start);
#endif

AK_MSH_CMD_EXPORT(oz_start_cmd, oz.start, start the OZKIE lock with log-only hardware);
AK_MSH_CMD_EXPORT(oz_touch_cmd, oz.touch, someone is at the door: open the advertising window);
#ifdef OZ_DEBUG_HOOKS   /* REL-SHELL-1: injection, bench images only */
AK_MSH_CMD_EXPORT(oz_code_cmd, oz.code, DEBUG BUILD ONLY: the owner typed this pairing code on the keypad);
AK_MSH_CMD_EXPORT(oz_key_cmd, oz.key, DEBUG BUILD ONLY: keys pressed on the keypad);
AK_MSH_CMD_EXPORT(oz_card_cmd, oz.card, DEBUG BUILD ONLY: a card at the reader: its UID in hex);
#endif
AK_MSH_CMD_EXPORT(oz_state_cmd, oz.state, lock state and the last motor move);

/* AUTOSTART-1: the lock starts itself OZ_AUTOSTART_MS after boot (a product
 * can't wait for a serial command; the operator took a waiting board for a
 * dead one). A failed start (the BLE address not ready) retries each second,
 * a few times. The main loop calls this with the ms tick; a typed oz.start
 * before or after is harmless. */
#define OZ_AUTOSTART_MS    3000u
#define OZ_AUTOSTART_TRIES 5
void oz_ak1037_app_poll(uint32_t now_ms)
{
    static uint32_t next = OZ_AUTOSTART_MS;
    static int tries;
    if (running || tries >= OZ_AUTOSTART_TRIES || now_ms < next)
        return;
    tries++;
    printf("oz.start: automatic start\r\n");
    if (oz_lockapp_ak1037_start() != OZ_OK)
        next = now_ms + 1000u;
}
