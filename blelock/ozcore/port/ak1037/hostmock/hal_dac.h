/* hal_dac.h (host mock): the audio DAC calls the lock image makes for the key beep (BEEP-1). */
#ifndef HAL_DAC_H_MOCK
#define HAL_DAC_H_MOCK
#include <stdint.h>
#include "ak_mock_types.h"
T_BOOL  hal_dac_init(uint32_t sample_rate, uint8_t data_bit, uint8_t gain);
T_BOOL  hal_dac_deinit(void);
int32_t hal_dac_open(void);
int32_t hal_dac_close(void);
int32_t hal_dac_trans_single(void *buf, uint32_t len);
#endif
