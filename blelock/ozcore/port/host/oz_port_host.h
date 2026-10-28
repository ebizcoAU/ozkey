/* Host-port extras for tests. */
#ifndef OZ_PORT_HOST_H
#define OZ_PORT_HOST_H

void oz_host_kv_reset(void);
/* Back kv with a file (loaded now, rewritten atomically on every change). */
int  oz_host_kv_file(const char *path);

#endif
