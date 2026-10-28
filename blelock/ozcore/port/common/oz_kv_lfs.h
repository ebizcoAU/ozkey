/* oz_kv_lfs — oz_port.h key-value storage on a mounted littlefs (ozkey-49 P4).
 *
 * Shared by every port that has littlefs: the AK1037 port mounts its own
 * partition and forwards oz_port_kv_* here; the host test runs the same code
 * on a RAM flash with power cuts injected.
 *
 * Each key is one file, "oz.<key>". A set writes "oz.<key>.t" and renames it
 * over the old file. littlefs commits a rename atomically, so after power loss
 * the key holds the old value or the new one, never a mix and never a
 * half-created empty file. Leftover ".t" files are removed at attach.
 *
 * Not re-entrant: call from one thread (the OZKIE thread). littlefs's own lock
 * hooks protect the filesystem itself.
 */
#ifndef OZ_KV_LFS_H
#define OZ_KV_LFS_H

#include <stddef.h>
#include <stdint.h>

#include "lfs.h"

/* Keys are 1..OZ_KV_LFS_KEY_MAX chars of [A-Za-z0-9_-]. */
#define OZ_KV_LFS_KEY_MAX   15
/* Per-file cache, on the stack; the mount's cache_size must not exceed it. */
#define OZ_KV_LFS_CACHE_MAX 256

/* Use an already-mounted filesystem. Returns OZ_E_ARG if its cache_size is
 * too big, otherwise OZ_OK (after removing leftover temp files). */
int oz_kv_lfs_attach(lfs_t *lfs);

int oz_kv_lfs_get(const char *key, uint8_t *buf, size_t cap, size_t *len);
int oz_kv_lfs_set(const char *key, const uint8_t *buf, size_t len);
int oz_kv_lfs_erase(const char *key);

#endif
