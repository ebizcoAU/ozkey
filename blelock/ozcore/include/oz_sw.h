/* oz_sw.h — portable software crypto primitives.
 *
 * Used by port/host directly, and by any port whose chip lacks the block.
 * Not constant-time-hardened against cache attacks (table AES); on the
 * AK1037 the hardware AES block is used instead.
 */
#ifndef OZ_SW_H
#define OZ_SW_H

#include "oz_port.h"

#ifdef __cplusplus
extern "C" {
#endif

void oz_sw_sha256_init(oz_sha256_ctx *ctx);
void oz_sw_sha256_update(oz_sha256_ctx *ctx, const uint8_t *data, size_t len);
void oz_sw_sha256_final(oz_sha256_ctx *ctx, uint8_t out[32]);

void oz_sw_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16],
                                uint8_t out[16]);

#ifdef __cplusplus
}
#endif
#endif /* OZ_SW_H */
