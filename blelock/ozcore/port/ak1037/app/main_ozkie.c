/* main_ozkie — the OZKIE app's entry point on AK1037 (ozkey-49 P4 item 5b).
 *
 * The SDK's app_mcu/src/main.c without its demo hooks: no low-power test task
 * at boot (ble_lp_test_start, which would skew every P6 current reading) and
 * no littlefs power-down test. The main thread still polls the shell and the
 * BLE stack; the OZKIE thread (oz_gatt_ak1037.c) relies on com_ble_process()
 * running here.
 */
#include "platform_cfg.h"
#include "cfg_mcu_anyka.h"
#include "ak_debug.h"
#include "hal_pmu.h"
#include "sbi_global.h"
#include "cmsis_os2.h"
#include "com_ble.h"
#include "hal_lpc.h"
#include "encoding.h"

extern void hal_set_total_callback(void (*fun)(void));
extern void msh_process();

#ifdef SUPP_RTOS
#define MAIN_THREAD_STACK_SIZE (10 * 1024)

#if (SUPP_BLE_FREQ_ADJ == 1)
static uint8_t ble_freq_adj_mk = 0;
static void ble_freq_intr(void) { ble_freq_adj_mk = 1; }
#endif

static void app_main_thread_entry(void *parameter)
{
    (void)parameter;
    printf("ozkie: main thread\r\n");
#if (SUPP_BLE_FREQ_ADJ == 1)
    /* Factory crystal trim, as the SDK app does it. */
    uint32_t osc_init_adjust_cap(void);
    hal_ao_gpio_set_dir(6, 1);
    hal_ao_gpio_set_pull(6, 1, 1);
    hal_ao_gpio_set_interrupt(6, 4, 1);
    hal_ao_gpio_set_interrupt_callback(6, ble_freq_intr);
    osc_init_adjust_cap();
#endif
#ifndef SUPP_WATCHDOG
    extern void hal_wdt_stop(void);
    hal_wdt_stop();
#endif
    for (;;) {
#ifdef SUPP_MSH
        msh_process();
#endif
#ifdef SUPPORT_BLE
        com_ble_process();
#endif
#if (SUPP_BLE_FREQ_ADJ == 1)
        if (ble_freq_adj_mk) {
            extern uint32_t osc_freq_adjust_start(void);
            osc_freq_adjust_start();
            ble_freq_adj_mk = 0;
        }
#endif
#ifdef SUPP_WATCHDOG
        hal_wdt_feed();
#endif
        osDelay(1);
    }
}

static void app_main_thread_init(void *argument)
{
    osThreadAttr_t attr = {
        "main_thread", 0, NULL, 0, NULL, MAIN_THREAD_STACK_SIZE, osPriorityNormal, 0, 0
    };
    (void)argument;
    osThreadNew(app_main_thread_entry, NULL, &attr);
}
#endif /* SUPP_RTOS */

void main_loop(void)
{
#ifndef SUPP_WATCHDOG
    extern void hal_wdt_stop(void);
    hal_wdt_stop();
#endif
    for (;;) {
#ifdef SUPP_MSH
        msh_process();
#endif
#ifdef SUPP_WATCHDOG
        hal_wdt_feed();
#endif
    }
}

void AK_Main(void)
{
    printf("ozkie: %s\r\n", MCU_SOFTWARE_VER);
#ifdef SUPP_RTOS
    osKernelInitialize(app_main_thread_init, NULL);
#endif
#ifdef SUPP_MSH
    extern int msh_init();
    msh_init(NULL);
#endif
#ifdef SUPP_RTOS
    osKernelStart();
#else
    main_loop();
#endif
}

/* The BLE stack calls this when it brings its clock back after low power. */
void app_ble_clk_recovery_cb(void)
{
    if (com_ble_recovery_get_lp_mode() == AK_FALSE)
        ak_print_error(MODULE_ID_APP, "ble clock recovery failed\r\n");
}

uint32_t start_time = 0;

/* Entry from the SDK's startup code: hal, traps, heap, then AK_Main. */
void app_proc(void)
{
    uint32_t cycle = get_cycle();
    set_enter_cycle(cycle);
    start_time = get_ms_tick_func();
#ifdef SUPPORT_BLE
    hal_set_total_callback(app_ble_clk_recovery_cb);
#endif
    hal_init_total(AO_USED_32K_OSC);
    extern void hal_core_trap_init(void);
    hal_core_trap_init();
#ifndef SUPP_RTOS
    uint32_t csr_tmp = read_csr(mstatus);
    write_csr(mstatus, (csr_tmp | (1 << 3)));   /* enable interrupts */
#endif
    extern void ak_mem_init(void);
    ak_mem_init();
    extern uint32_t hal_chip_get_chipid(void);
    printf("ozkie: boot %d chipid %x, cpu %d Hz\r\n", (int)hal_lpc_get_boot_state(),
           (unsigned)hal_chip_get_chipid(), (int)hal_clock_get_cpu_freq());
    AK_Main();
}
