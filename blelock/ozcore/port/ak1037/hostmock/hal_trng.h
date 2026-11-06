/* hal_trng.h (host mock). */
#ifndef HAL_TRNG_H_MOCK
#define HAL_TRNG_H_MOCK
#include <stdint.h>
void    hal_trng_init(void);
int32_t hal_trng_get_randbuff_bytes(uint8_t *da, int32_t len);
#endif
