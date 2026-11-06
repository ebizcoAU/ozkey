/* hal_lpc.h (host mock): boot state (0 = a cold boot, else a wake-up). */
#ifndef HAL_LPC_H_MOCK
#define HAL_LPC_H_MOCK
#include <stdint.h>
int32_t hal_lpc_get_boot_state(void);
#endif
