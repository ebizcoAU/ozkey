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

#include "rtthread.h"
#include "com_msh.h"
#include "com_ble.h"

#include "oz_port.h"
#include "oz_lock.h"
#include "oz_gatt_ak1037.h"
#include "oz_port_ak1037.h"

#ifndef OZ_START_PRIORITY
#define OZ_START_PRIORITY (RT_THREAD_PRIORITY_MAX / 2)
#endif
#define OZ_LOCKAPP_FW "ozkie-ak1037-bringup"

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
    oz_port_log("keypad: %s", on ? "enter the pairing code (oz.key NNNNNN#)" : "pairing prompt off");
}

int oz_lockapp_ak1037_start(void)
{
    static const oz_lock_hooks hooks = { actuate, log_prompt, NULL };
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
     * byte order com_ble_get_dev_addr() returns, against what a phone scans. */
    const uint8_t *a = com_ble_get_dev_addr();
    if (!a) {
        printf("oz.start: no BLE address\r\n");
        return OZ_E_HW;
    }
    snprintf(id, sizeof id, "ozk-%02x%02x%02x%02x%02x%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
    if (oz_lock_init(&lock, id, OZ_LOCKAPP_FW, &hooks) != OZ_OK) {
        printf("oz.start: oz_lock_init failed\r\n");
        return OZ_E_HW;
    }
    running = 1;
    if (oz_gatt_ak1037_start(&lock, OZ_START_PRIORITY) != OZ_OK) {
        printf("oz.start: GATT start failed; reboot\r\n");
        return OZ_E_HW;
    }
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
#endif

AK_MSH_CMD_EXPORT(oz_start_cmd, oz.start, start the OZKIE lock with log-only hardware);
AK_MSH_CMD_EXPORT(oz_touch_cmd, oz.touch, someone is at the door: open the advertising window);
AK_MSH_CMD_EXPORT(oz_code_cmd, oz.code, the owner typed this pairing code on the keypad);
AK_MSH_CMD_EXPORT(oz_key_cmd, oz.key, keys pressed on the keypad);
AK_MSH_CMD_EXPORT(oz_card_cmd, oz.card, a card at the reader: its UID in hex);
AK_MSH_CMD_EXPORT(oz_state_cmd, oz.state, lock state and the last motor move);
