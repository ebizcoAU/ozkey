/* mock_sdk — the AK1037 SDK as host mocks, so port/ak1037 builds and runs in
 * ctest under the gate, ASan included (ozkey-49 P5-A). The fake headers next
 * to this file stand in for the SDK's; this is how a test drives them.
 *
 * Threads are pthreads. A thread that blocks in rt_mq_recv (the OZKIE thread)
 * runs in lockstep with the test: it parks in rt_mq_recv until the test calls
 * mock_pass(), which lets it take one message (or time out) and run until it
 * parks again. The test only touches mock state while that thread is parked,
 * so nothing races and every run is the same. Time is virtual: mock_ms, moved
 * by the test, by rt_thread_mdelay and by a recv timeout. A thread that never
 * waits (the self-test) just runs; mock_join() waits for it to end.
 */
#ifndef MOCK_SDK_H
#define MOCK_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "com_ble.h"

/* ── time, interrupts ──────────────────────────────────────────────────── */
extern uint32_t mock_ms;                /* get_ms_tick_func(); ticks are mock_ms / 10 */
extern long mock_irq_violations;        /* SKE driver called with interrupts on */

/* ── SKE ───────────────────────────────────────────────────────────────── */
enum { MOCK_SKE_GOOD, MOCK_SKE_WRONG, MOCK_SKE_ERROR };
extern int mock_ske_mode;               /* WRONG flips a bit of every block */
extern long mock_ske_blocks;            /* blocks the engine encrypted */
extern uint8_t mock_ske_key_reg[16];    /* what the key registers hold now */

/* ── TRNG ──────────────────────────────────────────────────────────────── */
enum { MOCK_TRNG_GOOD, MOCK_TRNG_STUCK, MOCK_TRNG_TIMEOUT };
/* SAR ADC: what hal_saradc_get_data returns on the initialised channel
 * (12-bit, 0xffff = the SDK's error). */
extern int mock_adc_raw;
extern int mock_adc_channel;
extern long mock_adc_reads;

extern int mock_trng_mode;
extern int mock_trng_inits;
extern long mock_trng_words;

/* ── NOR flash and the partition table ─────────────────────────────────── */
#define MOCK_FLASH_SIZE (2u * 1024u * 1024u)
extern uint8_t mock_flash[MOCK_FLASH_SIZE];
extern int mock_part_present;           /* partition_get_bin_info finds "OZKV" */
extern uint32_t mock_part_start_page, mock_part_size;
extern long mock_flash_progs, mock_flash_erases, mock_nor_violations, mock_flash_out_of_part;
extern long mock_flash_fail_prog_at;    /* >0: the Nth write from now fails, no bits change */
extern int mock_flash_dead;             /* every read, write and erase fails */
extern long mock_part_lookups;          /* partition_get_bin_info calls */
void mock_flash_blank(void);            /* the whole chip erased (0xFF) */

/* ── threads ───────────────────────────────────────────────────────────── */
void mock_wait_parked(void);            /* until the lockstep thread waits in rt_mq_recv */
void mock_pass(void);                   /* let it take one message or one timeout */
void mock_join(const char *name);       /* until a free-running thread returns */
/* Deepest stack the thread has used so far, in bytes from its entry point,
 * measured on the host stack it really runs on (painted at start). 0 when it
 * can't be measured (ASan builds: instrumented frames and poisoned memory).
 * Host frames are not RV32 frames: a tripwire, not the target's number. */
size_t mock_stack_peak(const char *name);

/* ── BLE ───────────────────────────────────────────────────────────────── */
extern T_BLE_INIT_PARAM mock_ble;       /* what com_ble_init was given */
extern int mock_ble_inits, mock_ble_init_fail, mock_gatt_fail;
extern const T_BT_ATTREC *mock_gatt_table;
extern uint32_t mock_gatt_len;
extern uint16_t mock_mtu_requested;
extern uint8_t mock_dev_addr[6];
extern int mock_no_dev_addr;

extern int mock_adv_enabled;            /* last com_ble_set_adv_enable */
extern uint8_t mock_adv_type;
extern uint16_t mock_adv_min, mock_adv_max;
extern uint8_t mock_adv_data[31], mock_scan_rsp[31];
extern size_t mock_adv_data_len, mock_scan_rsp_len;
extern int mock_adv_data_fail;          /* com_ble_set_adv_data fails while set */
extern long mock_adv_applies;           /* successful enable(ENABLE) calls */

/* Every notification goes here; the default accepts it. */
extern T_eBLE_RESULT (*mock_on_notify)(void *link, const uint8_t *buf, uint16_t len, uint16_t idx);

/* The stack's side, from the test thread (the lockstep thread parked). */
void mock_ble_event(void *link, T_eBT_MSG msg, int32_t param1);
void mock_ble_write(void *link, const void *att_rec, const uint8_t *data, uint16_t len);
uint16_t mock_ble_read(void *link, const void *att_rec, uint8_t *buf, uint16_t cap);

/* ── the shell ─────────────────────────────────────────────────────────── */
/* Run "cmd arg ..." as MSH would. Returns -1 if no such command. */
int mock_msh_run(const char *line);

#endif
