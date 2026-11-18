/* hal_rfid.h (host mock): the SDK RFID reader calls the lock image makes (RFID-READER-1). */
#ifndef HAL_RFID_H_MOCK
#define HAL_RFID_H_MOCK
#include <stdint.h>
typedef enum { CARD_TYPE_A, CARD_TYPE_B } EN_RFID_CARD_TYPE;
int32_t hal_rfid_init(void);
int32_t hal_rfid_read_card(EN_RFID_CARD_TYPE card_type, uint8_t *data);
void    hal_rfid_destroy(void);
#endif
