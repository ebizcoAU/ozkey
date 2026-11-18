/* hal_gpio.h (host mock): the calls oz_port_ak1037.c makes (MOTOR-GPIO-1). */
#ifndef HAL_GPIO_H_MOCK
#define HAL_GPIO_H_MOCK
#include <stdint.h>
typedef enum { GPIO17 = 17, GPIO18 = 18, GPIO30 = 30, GPIO_MOCK_N = 64 } T_eGPIO;
typedef enum { GPIO_DIR_OUTPUT, GPIO_DIR_INPUT, GPIO_DIR_HEIGHT_Z } T_eGPIO_DIR;
typedef enum { GPIO_PULL_UP = 0, GPIO_PULL_DOWN } T_eGPIO_PULL;
int32_t hal_gpio_init(T_eGPIO gpio);
int32_t hal_gpio_set_dir(T_eGPIO gpio, T_eGPIO_DIR dir);
int32_t hal_gpio_output_value(T_eGPIO gpio, uint8_t value);
int32_t hal_gpio_set_pull(T_eGPIO gpio, T_eGPIO_PULL pull, uint8_t able);
#endif
