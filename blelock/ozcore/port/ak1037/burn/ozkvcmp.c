/* ozkvcmp: THE identity check after a burn (PM nexus-1b 2026-11-28; ozkey-49 burn checklist).
 *
 * Mounts two OZKV images read-only (littlefs, AK1037 geometry: 4096 B x 16 blocks, 256 B pages) and
 * compares every file. Exit 0 only if every oz.* file is in both and identical. The self-test's "zt"
 * namespace shares this littlefs, so OZKV is never byte-identical after a self-test run; compare files.
 * Each argument is a 64 KiB OZKV image or a 4 MiB full-flash read-back (OZKV cut at 0x155000).
 * Prints names, sizes and same/DIFF only, never contents: the files hold the lock's keys.
 *
 *   cc -std=c99 -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -I../../../third_party/littlefs \
 *      ozkvcmp.c ../../../third_party/littlefs/lfs.c ../../../third_party/littlefs/lfs_util.c -o ozkvcmp
 *   ./ozkvcmp pre.bin post.bin
 */
#include "lfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BS 4096
#define BC 16
#define OZKV_OFF 0x155000L   /* config_ozkie*.txt: OTA..UDATA = 16+212+780+44+212+100 KiB */
typedef struct { uint8_t img[BS * BC]; lfs_t fs; struct lfs_config c; } vol;
static int rd(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *buf, lfs_size_t n) { memcpy(buf, ((vol *)c->context)->img + b * BS + o, n); return 0; }
static int no_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *buf, lfs_size_t n) { (void)c;(void)b;(void)o;(void)buf;(void)n; return LFS_ERR_IO; }
static int no_erase(const struct lfs_config *c, lfs_block_t b) { (void)c;(void)b; return LFS_ERR_IO; }
static int nosync(const struct lfs_config *c) { (void)c; return 0; }
static int mount(vol *v, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f);
    long off = sz == 4L * 1024 * 1024 ? OZKV_OFF : 0;      /* full flash read-back */
    if (sz != (long)sizeof v->img && !off) { fprintf(stderr, "%s: %ld B, want 64 KiB or 4 MiB\n", path, sz); fclose(f); return -1; }
    fseek(f, off, SEEK_SET);
    if (fread(v->img, 1, sizeof v->img, f) != sizeof v->img) { fprintf(stderr, "read %s\n", path); fclose(f); return -1; }
    fclose(f);
    memset(&v->c, 0, sizeof v->c); v->c.context = v; v->c.read = rd; v->c.prog = no_prog; v->c.erase = no_erase; v->c.sync = nosync;
    v->c.read_size = 256; v->c.prog_size = 256; v->c.block_size = BS; v->c.block_count = BC; v->c.cache_size = 256; v->c.lookahead_size = 16; v->c.block_cycles = -1;
    int rc = lfs_mount(&v->fs, &v->c); if (rc) fprintf(stderr, "mount %s: %d\n", path, rc); return rc;
}
static long readall(vol *v, const char *name, uint8_t *buf, size_t cap) {
    lfs_file_t f; if (lfs_file_open(&v->fs, &f, name, LFS_O_RDONLY) < 0) return -1;
    long n = lfs_file_read(&v->fs, &f, buf, cap); lfs_file_close(&v->fs, &f); return n;
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "lfscmp pre.bin post.bin\n"); return 2; }
    static vol a, b; if (mount(&a, argv[1]) || mount(&b, argv[2])) return 1;
    int diff = 0;
    for (int pass = 0; pass < 2; pass++) {
        vol *x = pass ? &b : &a, *y = pass ? &a : &b;
        lfs_dir_t d; struct lfs_info in; lfs_dir_open(&x->fs, &d, "/");
        while (lfs_dir_read(&x->fs, &d, &in) > 0) {
            if (in.type != LFS_TYPE_REG) continue;
            static uint8_t p[BS * BC], q[BS * BC];
            long n = readall(x, in.name, p, sizeof p), m = readall(y, in.name, q, sizeof q);
            if (pass && m >= 0) continue;               /* both present: reported in pass 0 */
            const char *st = m < 0 ? (pass ? "ONLY IN POST" : "ONLY IN PRE") : (n == m && !memcmp(p, q, n) ? "same" : "DIFF");
            if (strcmp(st, "same")) diff += !strncmp(in.name, "oz.", 3);
            printf("%-14s %5ld %s\n", in.name, n, st);
        }
        lfs_dir_close(&x->fs, &d);
    }
    printf("oz.* differences: %d\n", diff);
    return diff != 0;
}
