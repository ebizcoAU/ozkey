/* oz_kv_lfs — see oz_kv_lfs.h. */
#include <string.h>

#include "oz_kv_lfs.h"
#include "oz_port.h"
#include "oz_crypto.h"

static lfs_t *fs;
static char ns[2] = {'o', 'z'};         /* namespace, see oz_kv_lfs_namespace() */

/* "oz." + key + ".t" + NUL */
#define PATH_MAX_LEN (3 + OZ_KV_LFS_KEY_MAX + 2 + 1)

static int map_err(int e)
{
    switch (e) {
    case LFS_ERR_NOENT: return OZ_E_NOT_FOUND;
    case LFS_ERR_NOSPC: return OZ_E_NO_SPACE;
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG: return OZ_E_ARG;
    default: return OZ_E_HW;
    }
}

static int path_of(char *out, const char *key, int temp)
{
    size_t n = 0;
    if (!key)
        return OZ_E_ARG;
    while (key[n]) {
        char c = key[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return OZ_E_ARG;
        if (++n > OZ_KV_LFS_KEY_MAX)
            return OZ_E_ARG;
    }
    if (n == 0)
        return OZ_E_ARG;
    out[0] = ns[0];
    out[1] = ns[1];
    out[2] = '.';
    memcpy(out + 3, key, n);
    if (temp) {
        memcpy(out + 3 + n, ".t", 3);
    } else {
        out[3 + n] = '\0';
    }
    return OZ_OK;
}

static int in_ns(const char *name)
{
    return name[0] == ns[0] && name[1] == ns[1] && name[2] == '.';
}

/* Any namespace's temp file is garbage after a power cut. */
static int is_temp(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && name[2] == '.' && !strcmp(name + n - 2, ".t");
}

/* Remove, one at a time and outside the walk, every file `doomed_by` picks:
 * littlefs doesn't promise a stable iteration while the directory changes. */
static int remove_all(int (*doomed_by)(const char *))
{
    lfs_dir_t dir;
    struct lfs_info info;
    char doomed[PATH_MAX_LEN];
    for (;;) {
        int found = 0;
        if (lfs_dir_open(fs, &dir, "/") < 0)
            return OZ_E_HW;
        while (lfs_dir_read(fs, &dir, &info) > 0) {
            if (info.type == LFS_TYPE_REG && doomed_by(info.name) &&
                strlen(info.name) < sizeof doomed) {
                strcpy(doomed, info.name);
                found = 1;
                break;
            }
        }
        lfs_dir_close(fs, &dir);
        if (!found)
            return OZ_OK;
        int e = lfs_remove(fs, doomed);
        if (e < 0)
            return map_err(e);
    }
}

int oz_kv_lfs_namespace(const char *name)
{
    if (!name || strlen(name) != 2)
        return OZ_E_ARG;
    for (int i = 0; i < 2; i++)
        if (name[i] < 'a' || name[i] > 'z')
            return OZ_E_ARG;
    ns[0] = name[0];
    ns[1] = name[1];
    return OZ_OK;
}

int oz_kv_lfs_wipe(void)
{
    return fs ? remove_all(in_ns) : OZ_E_HW;
}

int oz_kv_lfs_attach(lfs_t *lfs)
{
    if (!lfs || lfs->cfg->cache_size > OZ_KV_LFS_CACHE_MAX)
        return OZ_E_ARG;
    fs = lfs;
    /* A set that lost power before its rename leaves "oz.<key>.t" behind:
     * the old value is still intact, so the temp file is just garbage. */
    return remove_all(is_temp);
}

/* An open file and everything littlefs points into while it's open: it keeps
 * the config pointer (file->cfg) and reads its attrs on every sync/close, so
 * the config can't be a local of the open call. */
typedef struct {
    lfs_file_t f;
    struct lfs_file_config fc;
    uint8_t cache[OZ_KV_LFS_CACHE_MAX];
} kv_file;

static int open_file(kv_file *k, const char *path, int flags)
{
    memset(&k->fc, 0, sizeof k->fc);
    k->fc.buffer = k->cache;
    return lfs_file_opencfg(fs, &k->f, path, flags, &k->fc);
}

int oz_kv_lfs_get(const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    char path[PATH_MAX_LEN];
    kv_file k;
    int rc = path_of(path, key, 0);
    if (rc)
        return rc;
    if (!fs)
        return OZ_E_HW;
    int e = open_file(&k, path, LFS_O_RDONLY);
    if (e < 0)
        return map_err(e);
    lfs_soff_t size = lfs_file_size(fs, &k.f);
    if (size < 0) {
        rc = map_err((int)size);
    } else if ((size_t)size > cap) {
        rc = OZ_E_NO_SPACE;
    } else {
        lfs_ssize_t got = lfs_file_read(fs, &k.f, buf, (lfs_size_t)size);
        if (got < 0)
            rc = map_err((int)got);
        else if (got != size)
            rc = OZ_E_HW;
        else
            *len = (size_t)size;
    }
    lfs_file_close(fs, &k.f);
    oz_wipe(k.cache, sizeof k.cache);
    return rc;
}

int oz_kv_lfs_set(const char *key, const uint8_t *buf, size_t len)
{
    char path[PATH_MAX_LEN], temp[PATH_MAX_LEN];
    kv_file k;
    int rc = path_of(path, key, 0);
    if (!rc)
        rc = path_of(temp, key, 1);
    if (rc)
        return rc;
    if (!fs)
        return OZ_E_HW;
    if (len > 0x7fffffff)
        return OZ_E_ARG;

    int e = open_file(&k, temp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (e < 0) {
        oz_wipe(k.cache, sizeof k.cache);
        return map_err(e);
    }
    lfs_ssize_t put = len ? lfs_file_write(fs, &k.f, buf, (lfs_size_t)len) : 0;
    e = lfs_file_close(fs, &k.f);         /* close commits the temp file */
    oz_wipe(k.cache, sizeof k.cache);
    if (put < 0 || e < 0 || (size_t)put != len) {
        (void)lfs_remove(fs, temp);
        return map_err(put < 0 ? (int)put : e < 0 ? e : LFS_ERR_IO);
    }
    e = lfs_rename(fs, temp, path);       /* the atomic switch-over */
    if (e < 0) {
        (void)lfs_remove(fs, temp);
        return map_err(e);
    }
    return OZ_OK;
}

int oz_kv_lfs_erase(const char *key)
{
    char path[PATH_MAX_LEN];
    int rc = path_of(path, key, 0);
    if (rc)
        return rc;
    if (!fs)
        return OZ_E_HW;
    int e = lfs_remove(fs, path);
    return e < 0 ? map_err(e) : OZ_OK;
}
