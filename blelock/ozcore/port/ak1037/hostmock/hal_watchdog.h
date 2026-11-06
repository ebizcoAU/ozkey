/* hal_watchdog.h (host mock): the calls oz_wdt_ak1037.c makes. */
#ifndef HAL_WATCHDOG_H_MOCK
#define HAL_WATCHDOG_H_MOCK
#include <stdint.h>
typedef enum {
    WATCHDOG_P2S = 0, WATCHDOG_P4S, WATCHDOG_P8S, WATCHDOG_1P6S,
    WATCHDOG_3P2S, WATCHDOG_6P4S, WATCHDOG_12P8S, WATCHDOG_25P6S,
} T_eWDT_TIME;
void hal_wdt_start(T_eWDT_TIME feed_time);
void hal_wdt_feed(void);
void hal_wdt_stop(void);
#endif
