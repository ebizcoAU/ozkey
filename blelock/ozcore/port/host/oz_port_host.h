/* Host-port extras for tests. */
#ifndef OZ_PORT_HOST_H
#define OZ_PORT_HOST_H

#include <stdint.h>

void oz_host_kv_reset(void);
/* KV-OVERSIZE-1: the next n sets/erases of key (NULL = any; n < 0 = until
 * cleared with n = 0) fail with OZ_E_HW. Host port only. */
void oz_host_kv_fail(const char *key, int n);
/* Back kv with a file (loaded now, rewritten atomically on every change). */
int  oz_host_kv_file(const char *path);
/* Tests: move the monotonic clock forward (it still never goes back). */
void oz_host_advance_ms(uint64_t ms);

#endif
