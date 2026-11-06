/* hal_saradc.h (host mock): the calls oz_port_ak1037.c makes. */
#ifndef HAL_SARADC_H_MOCK
#define HAL_SARADC_H_MOCK
#include <stdint.h>
typedef enum {
    SARADC_SEL_AIN0 = (1 << 0), SARADC_SEL_AIN1 = (1 << 1), SARADC_SEL_AIN2 = (1 << 2),
    SARADC_SEL_AIN3 = (1 << 3), SARADC_SEL_AIN4 = (1 << 4), SARADC_SEL_AIN5 = (1 << 5),
    SARADC_SEL_AIN6 = (1 << 6), SARADC_SEL_AIN7 = (1 << 7),
} T_eSARADC_SEL;
int32_t hal_saradc_init(int mark);
int     hal_saradc_get_data(T_eSARADC_SEL sel);
#endif
