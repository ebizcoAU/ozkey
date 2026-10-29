/* test_kv_lfs — oz_kv_lfs on a RAM NOR flash with power cuts (ozkey-49 P4).
 *
 * The flash has the AK1037 geometry: 256-byte pages, 4 KiB sectors, 16
 * sectors (64 KiB). It behaves like NOR: erase sets 0xFF, program can only
 * clear bits. A "power cut" after N flash operations tears the operation in
 * flight (half a page programmed, half a sector erased) and then the CPU
 * stops: a longjmp straight back to the test, so no code after the cut ever
 * runs, until the test "reboots" by mounting the same image again. (Letting
 * littlefs carry on against a dead flash is not a state silicon can reach,
 * and littlefs 2.4.1 rightly asserts on it.)
 *
 * The sweep cuts each kv call at every possible operation and checks the
 * oz_port.h rule: after a reboot the key holds the old value or the new one,
 * never a mix, never an empty half-created file, and other keys are intact.
 */
#include <setjmp.h>
#include <stdlib.h>

#include "oz_test.h"
#include "oz_port.h"
#include "oz_kv_lfs.h"

#define PAGE    256
#define SECTOR  4096
#define SECTORS 16

static uint8_t flash[SECTORS * SECTOR];
static long ops_left = -1;              /* -1: no cut armed */
static int dead;                        /* power is off until reboot */
static long ops_done, nor_violations;
static long prog_fail_at;               /* fail the Nth prog, power stays on */
static long prog_failures;
static jmp_buf power_off;               /* where the CPU "stops" at a cut */

/* Run one kv call with power that may die during it: rc is OZ_E_HW if it did. */
#define POWERED(rc, call) do {                     \
        if (setjmp(power_off) == 0) (rc) = (call); \
        else (rc) = OZ_E_HW;                       \
    } while (0)

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n)
{
    (void)c;
    memcpy(buf, flash + b * SECTOR + off, n);
    return 0;
}

/* Counts one flash operation; returns 1 if the power dies during it. */
static int cut_now(void)
{
    if (dead)
        return 1;
    ops_done++;
    if (ops_left > 0 && --ops_left == 0) {
        dead = 1;
        return 1;
    }
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n)
{
    (void)c;
    if (dead)
        return LFS_ERR_IO;
    if (prog_fail_at > 0 && --prog_fail_at == 0) {
        prog_failures++;                /* a flash error: nothing written */
        return LFS_ERR_IO;
    }
    int torn = cut_now();
    uint8_t *dst = flash + b * SECTOR + off;
    const uint8_t *src = buf;
    lfs_size_t todo = torn ? n / 2 : n;
    for (lfs_size_t i = 0; i < todo; i++) {
        if ((dst[i] & src[i]) != src[i])
            nor_violations++;           /* littlefs asked for a 0 -> 1 */
        dst[i] &= src[i];
    }
    if (torn)
        longjmp(power_off, 1);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;
    if (dead)
        return LFS_ERR_IO;
    int torn = cut_now();
    memset(flash + b * SECTOR, 0xff, torn ? SECTOR / 2 : SECTOR);
    if (torn)
        longjmp(power_off, 1);
    return 0;
}

static int bd_sync(const struct lfs_config *c) { (void)c; return dead ? LFS_ERR_IO : 0; }

static const struct lfs_config cfg = {
    .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
    .read_size = PAGE, .prog_size = PAGE, .block_size = SECTOR, .block_count = SECTORS,
    .cache_size = PAGE, .lookahead_size = 16, .block_cycles = 500,
};

static lfs_t lfs;
static int mounted;

/* Power on: mount whatever is in flash, as the device does at boot. */
static int boot(void)
{
    dead = 0;
    ops_left = -1;
    memset(&lfs, 0, sizeof lfs);
    mounted = lfs_mount(&lfs, &cfg) == 0;
    if (!mounted)
        return -1;
    return oz_kv_lfs_attach(&lfs);
}

static void format_and_boot(void)
{
    memset(flash, 0xff, sizeof flash);
    memset(&lfs, 0, sizeof lfs);
    lfs_format(&lfs, &cfg);
    expect_int("boot after format", boot(), OZ_OK);
}

static void fill(uint8_t *b, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)(seed + i * 7);
}

/* 0 = absent, 1 = equals a, 2 = equals b, -1 = anything else (a failure). */
static int which(const char *key, const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    uint8_t got[1024];
    size_t n = 0;
    int rc = oz_kv_lfs_get(key, got, sizeof got, &n);
    if (rc == OZ_E_NOT_FOUND)
        return 0;
    if (rc != OZ_OK)
        return -1;
    if (a && n == al && !memcmp(got, a, n))
        return 1;
    if (b && n == bl && !memcmp(got, b, n))
        return 2;
    return -1;
}

static int temp_files(void)
{
    lfs_dir_t d;
    struct lfs_info info;
    int n = 0;
    if (lfs_dir_open(&lfs, &d, "/") < 0)
        return -1;
    while (lfs_dir_read(&lfs, &d, &info) > 0) {
        size_t l = strlen(info.name);
        if (l > 2 && !strcmp(info.name + l - 2, ".t"))
            n++;
    }
    lfs_dir_close(&lfs, &d);
    return n;
}

static void basics(void)
{
    format_and_boot();
    uint8_t v[64], out[64];
    size_t n = 0;
    fill(v, sizeof v, 1);
    expect_int("get absent", oz_kv_lfs_get("b0", out, sizeof out, &n), OZ_E_NOT_FOUND);
    expect_int("erase absent", oz_kv_lfs_erase("b0"), OZ_E_NOT_FOUND);
    expect_int("set", oz_kv_lfs_set("b0", v, sizeof v), OZ_OK);
    expect_int("get", oz_kv_lfs_get("b0", out, sizeof out, &n), OZ_OK);
    expect_int("get value", n == sizeof v && !memcmp(out, v, n), 1);
    expect_int("small cap", oz_kv_lfs_get("b0", out, 10, &n), OZ_E_NO_SPACE);
    expect_int("empty value", oz_kv_lfs_set("ep", v, 0), OZ_OK);
    expect_int("get empty", oz_kv_lfs_get("ep", out, sizeof out, &n), OZ_OK);
    expect_int("empty len", (long long)n, 0);
    expect_int("bad key /", oz_kv_lfs_set("a/b", v, 1), OZ_E_ARG);
    expect_int("bad key .", oz_kv_lfs_set("a.t", v, 1), OZ_E_ARG);
    expect_int("bad key empty", oz_kv_lfs_set("", v, 1), OZ_E_ARG);
    expect_int("key 15 ok", oz_kv_lfs_set("abcdefghijklmno", v, 1), OZ_OK);
    expect_int("key 16 bad", oz_kv_lfs_set("abcdefghijklmnop", v, 1), OZ_E_ARG);
    expect_int("erase", oz_kv_lfs_erase("b0"), OZ_OK);
    expect_int("gone", oz_kv_lfs_get("b0", out, sizeof out, &n), OZ_E_NOT_FOUND);
    /* Survives a clean reboot. */
    expect_int("set lk", oz_kv_lfs_set("lk", v, 32), OZ_OK);
    lfs_unmount(&lfs);
    expect_int("reboot", boot(), OZ_OK);
    expect_int("lk after reboot", which("lk", v, 32, NULL, 0), 1);
}

/* The self-test's namespace: its keys and wipes never reach the real ones. */
static void namespaces(void)
{
    format_and_boot();
    uint8_t v[16], w[16], out[16];
    size_t n = 0;
    fill(v, sizeof v, 3);
    fill(w, sizeof w, 9);
    expect_int("ns: real key", oz_kv_lfs_set("lk", v, sizeof v), OZ_OK);
    expect_int("ns: bad, 1 char", oz_kv_lfs_namespace("z"), OZ_E_ARG);
    expect_int("ns: bad, 3 chars", oz_kv_lfs_namespace("ztt"), OZ_E_ARG);
    expect_int("ns: bad, upper", oz_kv_lfs_namespace("ZT"), OZ_E_ARG);
    expect_int("ns: bad, null", oz_kv_lfs_namespace(NULL), OZ_E_ARG);
    expect_int("ns: switch", oz_kv_lfs_namespace("zt"), OZ_OK);
    expect_int("ns: real key invisible", oz_kv_lfs_get("lk", out, sizeof out, &n), OZ_E_NOT_FOUND);
    expect_int("ns: own key", oz_kv_lfs_set("lk", w, sizeof w), OZ_OK);
    expect_int("ns: own value", which("lk", w, sizeof w, v, sizeof v), 1);
    expect_int("ns: second key", oz_kv_lfs_set("b0", w, 4), OZ_OK);
    expect_int("ns: wipe", oz_kv_lfs_wipe(), OZ_OK);
    expect_int("ns: wiped", which("lk", w, sizeof w, v, sizeof v), 0);
    expect_int("ns: wiped too", which("b0", w, 4, NULL, 0), 0);
    expect_int("ns: back", oz_kv_lfs_namespace("oz"), OZ_OK);
    expect_int("ns: real key survived", which("lk", v, sizeof v, w, sizeof w), 1);

    /* A temp left by a cut inside the test namespace is swept at boot too. */
    lfs_file_t f;
    expect_int("ns: plant zt temp", lfs_file_open(&lfs, &f, "zt.b0.t", LFS_O_WRONLY | LFS_O_CREAT), 0);
    lfs_file_close(&lfs, &f);
    lfs_unmount(&lfs);
    expect_int("ns: reboot", boot(), OZ_OK);
    expect_int("ns: zt temp swept", temp_files(), 0);
    expect_int("ns: real key after reboot", which("lk", v, sizeof v, NULL, 0), 1);
}

/* The core's real working set: 16 bonds, 16 relay contexts, creds, keys. */
static void capacity(void)
{
    format_and_boot();
    uint8_t v[600];
    char k[8];
    int ok = 1;
    for (int i = 0; i < 16 && ok; i++) {
        fill(v, sizeof v, (uint8_t)i);
        snprintf(k, sizeof k, "b%d", i);
        ok &= oz_kv_lfs_set(k, v, 200) == OZ_OK;
        snprintf(k, sizeof k, "r%d", i);
        ok &= oz_kv_lfs_set(k, v, 120) == OZ_OK;
    }
    ok &= oz_kv_lfs_set("cr", v, 600) == OZ_OK;
    ok &= oz_kv_lfs_set("lk", v, 32) == OZ_OK;
    ok &= oz_kv_lfs_set("nc", v, 64) == OZ_OK;
    expect_int("working set fits in 64 KiB", ok, 1);
    /* Rewrite it many times: compaction and wear levelling run, data stays. */
    for (int round = 0; round < 200 && ok; round++)
        for (int i = 0; i < 16 && ok; i++) {
            fill(v, sizeof v, (uint8_t)(round + i));
            snprintf(k, sizeof k, "r%d", i);
            ok &= oz_kv_lfs_set(k, v, 120) == OZ_OK;
        }
    expect_int("3,200 rewrites", ok, 1);
    fill(v, sizeof v, (uint8_t)(199 + 15));
    expect_int("last rewrite reads back", which("r15", v, 120, NULL, 0), 1);
}

/* One kv call, cut at every flash operation it makes, until it runs clean. */
typedef enum { OP_REPLACE, OP_CREATE, OP_ERASE } op_kind;

static void sweep(op_kind kind, const char *name, long min_cuts)
{
    uint8_t oldv[300], newv[500], other[32];
    fill(oldv, sizeof oldv, 0x10);
    fill(newv, sizeof newv, 0x80);
    fill(other, sizeof other, 0x55);
    long cuts = 0, bad = 0, lost_ok = 0, leftovers = 0, unmountable = 0;

    for (long at = 1;; at++) {
        format_and_boot();
        oz_kv_lfs_set("lk", other, sizeof other);
        if (kind != OP_CREATE)
            oz_kv_lfs_set("b3", oldv, sizeof oldv);

        ops_left = at;
        int rc;
        POWERED(rc, kind == OP_ERASE ? oz_kv_lfs_erase("b3")
                                     : oz_kv_lfs_set("b3", newv, sizeof newv));
        int was_cut = dead;
        if (boot() != OZ_OK) {
            unmountable++;
            continue;
        }
        int w = kind == OP_CREATE ? which("b3", NULL, 0, newv, sizeof newv)
                                  : which("b3", oldv, sizeof oldv, newv, sizeof newv);
        int allowed = kind == OP_REPLACE ? (w == 1 || w == 2)
                    : kind == OP_CREATE  ? (w == 0 || w == 2)
                    :                      (w == 0 || w == 1);
        if (!allowed) bad++;
        /* A call that returned OK must have taken effect. */
        if (rc == OZ_OK && w != (kind == OP_ERASE ? 0 : 2)) lost_ok++;
        if (which("lk", other, sizeof other, NULL, 0) != 1) bad++;
        if (temp_files() != 0) leftovers++;
        if (!was_cut)
            break;                      /* the call finished before the cut */
        cuts++;
    }
    char label[64];
    snprintf(label, sizeof label, "%s: cut points tried", name);
    checks++;
    if (cuts < min_cuts) { failures++; printf("FAIL %s: only %ld\n", label, cuts); }
    printf("%s: %ld cut points\n", name, cuts);
    snprintf(label, sizeof label, "%s: old-or-new", name);
    expect_int(label, bad, 0);
    snprintf(label, sizeof label, "%s: OK means done", name);
    expect_int(label, lost_ok, 0);
    snprintf(label, sizeof label, "%s: no temp left", name);
    expect_int(label, leftovers, 0);
    snprintf(label, sizeof label, "%s: mounts after cut", name);
    expect_int(label, unmountable, 0);
}

/* Random cuts across a long run of mixed operations on a well-used flash. */
static void soak(void)
{
    format_and_boot();
    uint8_t cur[8][400], v[400];
    size_t cur_len[8] = {0};
    int present[8] = {0};
    char k[8];
    long bad = 0, reboots = 0;
    srand(1037);
    for (int step = 0; step < 3000; step++) {
        int i = rand() % 8;
        snprintf(k, sizeof k, "k%d", i);
        size_t len = (size_t)(rand() % 400);
        fill(v, len, (uint8_t)step);
        if (rand() % 6 == 0)
            ops_left = 1 + rand() % 12;
        int erase = rand() % 5 == 0;
        int rc;
        POWERED(rc, erase ? oz_kv_lfs_erase(k) : oz_kv_lfs_set(k, v, len));
        if (dead) {
            reboots++;
            if (boot() != OZ_OK) { bad++; format_and_boot(); memset(present, 0, sizeof present); continue; }
            int w = which(k, present[i] ? cur[i] : NULL, cur_len[i], erase ? NULL : v, len);
            if (w < 0 || (w == 0 && present[i] && !erase)) { bad++; continue; }
            if (w == 2) { memcpy(cur[i], v, len); cur_len[i] = len; present[i] = 1; }
            if (w == 0) present[i] = 0;
        } else if (rc == OZ_OK) {
            ops_left = -1;
            if (erase) present[i] = 0;
            else { memcpy(cur[i], v, len); cur_len[i] = len; present[i] = 1; }
        } else if (!(erase && rc == OZ_E_NOT_FOUND && !present[i])) {
            bad++;
        }
        ops_left = -1;
    }
    /* Everything still reads back as last committed. */
    for (int i = 0; i < 8; i++) {
        snprintf(k, sizeof k, "k%d", i);
        int w = which(k, present[i] ? cur[i] : NULL, cur_len[i], NULL, 0);
        if (w != (present[i] ? 1 : 0)) bad++;
    }
    printf("soak: 3000 ops, %ld power cuts\n", reboots);
    expect_int("soak: consistent", bad, 0);
    checks++;
    if (reboots < 100) { failures++; printf("FAIL soak: only %ld cuts\n", reboots); }
}

/* A program that fails with the power still on (a flash error, not a cut).
 * Fail each prog of a replace in turn: the call reports OZ_E_HW or completes,
 * the key is old-or-new, and with no reboot the very next calls work, read
 * from flash and not from the stale cache littlefs 2.4.1 keeps. Runs with
 * littlefs's asserts live, which fire on that stale cache. */
static void io_error(void)
{
    uint8_t a[300], b[300], other[32];
    fill(a, sizeof a, 0x21);
    fill(b, sizeof b, 0x42);
    fill(other, sizeof other, 0x63);
    long tried = 0, bad = 0, unusable = 0;
    format_and_boot();
    expect_int("ioerr: set other", oz_kv_lfs_set("lk", other, sizeof other), OZ_OK);
    for (long at = 1; at <= 12; at++) {
        if (oz_kv_lfs_set("b5", a, sizeof a) != OZ_OK) { unusable++; continue; }
        long before = prog_failures;
        prog_fail_at = at;
        int rc = oz_kv_lfs_set("b5", b, sizeof b);
        prog_fail_at = 0;
        if (prog_failures == before)
            break;                      /* the set finished before prog #at */
        tried++;
        int w = which("b5", a, sizeof a, b, sizeof b);
        if (rc != OZ_OK && rc != OZ_E_HW) bad++;
        if (w != 1 && w != 2) bad++;
        if (rc == OZ_OK && w != 2) bad++;
        if (which("lk", other, sizeof other, NULL, 0) != 1) bad++;
        if (temp_files() != 0) bad++;
        /* Usable straight away, no reboot. */
        if (oz_kv_lfs_set("b5", b, sizeof b) != OZ_OK || which("b5", NULL, 0, b, sizeof b) != 2)
            unusable++;
    }
    checks++;
    if (tried < 3) { failures++; printf("FAIL ioerr: only %ld failing progs tried\n", tried); }
    printf("ioerr: %ld failing progs\n", tried);
    expect_int("ioerr: old-or-new, nothing else", bad, 0);
    expect_int("ioerr: usable without a reboot", unusable, 0);
}

int main(void)
{
    basics();
    namespaces();
    capacity();
    sweep(OP_REPLACE, "replace", 3);
    sweep(OP_CREATE, "create", 3);
    sweep(OP_ERASE, "erase", 1);      /* one metadata commit, so one cut point */
    soak();
    io_error();
    expect_int("littlefs never asked NOR for 0->1", nor_violations, 0);
    return report("kv_lfs");
}
