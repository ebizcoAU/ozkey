/* hal_sysclock.h (host mock): the reset status, as the SDK's enum. */
#ifndef HAL_SYSCLOCK_H_MOCK
#define HAL_SYSCLOCK_H_MOCK
typedef enum {
    HAL_AO_POR_RST = 0, HAL_AO_WDT_RST, HAL_AO_HW_RST, HAL_RST_RSV,
    HAL_CORE_WDT_RST, HAL_SOFT_RESET_RST, HAL_USB_PLUGIN_RST, HAL_LPC_FSM_ERR_RST,
    HAL_RESET_NONE = 0xff
} T_eRST_STATUS;
T_eRST_STATUS hal_clock_get_reset_status(void);
#endif
