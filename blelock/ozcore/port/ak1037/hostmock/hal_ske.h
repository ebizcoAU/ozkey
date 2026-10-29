/* hal_ske.h (host mock): the SKE calls port/ak1037 makes. */
#ifndef HAL_SKE_H_MOCK
#define HAL_SKE_H_MOCK
#include <stdint.h>
typedef enum { SKE_ALG_AES_128 = 1 } SKE_ALG;
typedef enum { SKE_MODE_ECB = 1 } SKE_MODE;
typedef enum { SKE_CRYPTO_ENCRYPT = 0, SKE_CRYPTO_DECRYPT } SKE_CRYPTO;
enum { SKE_SUCCESS = 0 };
uint32_t hal_ske_init(SKE_ALG alg, SKE_MODE mode, SKE_CRYPTO crypto, uint8_t *key, uint8_t sp_key_idx, uint8_t *iv);
uint32_t hal_ske_update_blocks(uint8_t *in, uint8_t *out, uint32_t byteLen);
uint32_t hal_ske_final(void);
#endif
