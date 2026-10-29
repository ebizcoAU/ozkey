/* oz_selftest_ak1037 — the self-test image's shell command (ozkey-49 P4
 * item 4b). The lock's own commands are oz_lockapp_ak1037.c, a separate
 * image: both together don't fit PROG on the SDK's default partition table.
 *
 *   oz.selftest  every host suite (tests/selftest_all.c) in its own 16 KB
 *                thread, then what only silicon can check: the SKE engine
 *                against software AES, and the TRNG. Ends with one line,
 *                "OZ SELFTEST PASS|FAIL ...", plus the thread's stack peak.
 *
 * The suites reset kv between cases. They run in kv namespace "zt", wiped
 * before and after, so they never touch the lock's identity in "oz". If the
 * lock app is linked in too and has started, it owns kv this boot
 * (oz_ak1037_kv_claim), so oz.selftest refuses. The suites' check counters
 * are per boot, so it also runs once.
 *
 * MSH runs commands on the shell thread's small stack, so the command only
 * starts a thread. Its priority is compile-time until the kit shows what the
 * SDK's own threads use (rtconfig.h says 8 levels, the shell asks for 20).
 */
#include <stdio.h>
#include <string.h>

#include "rtthread.h"
#include "com_msh.h"

#include "oz_port.h"
#include "oz_sw.h"
#include "oz_crypto.h"
#include "oz_kv_lfs.h"
#include "oz_port_host.h"
#include "oz_selftest.h"
#include "oz_port_ak1037.h"

#ifndef OZ_SELFTEST_STACK
#define OZ_SELFTEST_STACK (16 * 1024)   /* test_lock is the deepest suite */
#endif
#ifndef OZ_SELFTEST_PRIORITY
#define OZ_SELFTEST_PRIORITY (RT_THREAD_PRIORITY_MAX - 2)
#endif

#define SKE_BLOCKS     256
#define MONOBIT_WORDS  625              /* 20,000 bits (FIPS 140-2 §4.9.1) */
#define REPEAT_BLOCKS  64

static volatile enum { IDLE, RUNNING, DONE } state;

/* ── kv for the suites ───────────────────────────────────────────────────── */

/* The host port's reset, on silicon: empty the self-test namespace. */
void oz_host_kv_reset(void)
{
    if (oz_kv_lfs_wipe() != OZ_OK)
        oz_port_log("selftest: kv wipe failed");
}

/* ── checks only silicon can run ─────────────────────────────────────────── */

static int ske_vs_software(void)
{
    uint8_t key[16], in[16], hw[16], sw[16];
    int state_now = oz_ak1037_ske_state(), bad = 0;

    if (state_now < 0) {
        printf("FAIL ske: engine failed FIPS-197 C.1, the port is on software AES\r\n");
        return 1;
    }
    for (int i = 0; i < SKE_BLOCKS; i++) {
        if (oz_port_random(key, sizeof key) != OZ_OK || oz_port_random(in, sizeof in) != OZ_OK) {
            bad++;
            break;
        }
        oz_sw_aes128_encrypt_block(key, in, sw);
        if (oz_ak1037_ske_block(key, in, hw) != OZ_OK || !oz_ct_equal(hw, sw, 16))
            bad++;
    }
    oz_wipe(key, sizeof key);
    printf("ske: %d random blocks, %d differ from software\r\n", SKE_BLOCKS, bad);
    return bad != 0;
}

static unsigned ones32(uint32_t w)
{
    unsigned n = 0;
    for (; w; w &= w - 1)
        n++;
    return n;
}

/* FIPS 140-2 monobit: 9,725 < ones < 10,275 over 20,000 bits. */
static int monobit(const char *what, unsigned ones)
{
    int ok = ones > 9725 && ones < 10275;
    printf("%s trng: monobit %u ones in 20000 bits %s\r\n", what, ones, ok ? "ok" : "FAIL");
    return !ok;
}

static int trng_checks(void)
{
    static uint8_t blocks[REPEAT_BLOCKS][32];
    unsigned raw = 0, cond = 0;
    int fails = 0, repeats = 0;
    uint32_t w;

    for (int i = 0; i < MONOBIT_WORDS; i++) {
        if (oz_ak1037_trng_word(&w) != OZ_OK) {
            printf("FAIL raw trng: read timed out\r\n");
            return 1;
        }
        raw += ones32(w);
    }
    fails += monobit("raw", raw);

    for (int i = 0; i < MONOBIT_WORDS; i++) {
        if (oz_port_random((uint8_t *)&w, sizeof w) != OZ_OK) {
            printf("FAIL oz_port_random failed\r\n");
            return fails + 1;
        }
        cond += ones32(w);
    }
    fails += monobit("conditioned", cond);

    for (int i = 0; i < REPEAT_BLOCKS; i++) {
        if (oz_port_random(blocks[i], 32) != OZ_OK)
            return fails + 1;
        for (int j = 0; j < i; j++)
            repeats += !memcmp(blocks[i], blocks[j], 32);
    }
    printf("trng: %d repeated 32-byte blocks in %d\r\n", repeats, REPEAT_BLOCKS);
    oz_wipe(blocks, sizeof blocks);
    return fails + (repeats != 0);
}

/* ── oz.selftest ─────────────────────────────────────────────────────────── */

static struct rt_thread st_thread;
static uint8_t st_stack[OZ_SELFTEST_STACK] __attribute__((aligned(8)));

/* rt_thread_init fills the stack with '#'; it grows down from the top. */
static unsigned stack_peak(void)
{
    unsigned untouched = 0;
    while (untouched < sizeof st_stack && st_stack[untouched] == '#')
        untouched++;
    return (unsigned)sizeof st_stack - untouched;
}

static void selftest_thread(void *arg)
{
    uint64_t t0 = oz_port_monotonic_ms();
    int fails = 0;
    (void)arg;

    if (oz_ak1037_kv_mount() != OZ_OK || oz_kv_lfs_namespace("zt") != OZ_OK) {
        printf("FAIL kv: OZKV partition not usable, suites not run\r\n");
        fails = 1;
    } else {
        oz_host_kv_reset();
        fails += oz_selftest_suites();
        oz_host_kv_reset();
        (void)oz_kv_lfs_namespace("oz");
    }
    fails += ske_vs_software();
    fails += trng_checks();

    printf("OZ SELFTEST %s: %d failing, %u ms, stack peak %u of %u B\r\n",
           fails ? "FAIL" : "PASS", fails, (unsigned)(oz_port_monotonic_ms() - t0),
           stack_peak(), (unsigned)sizeof st_stack);
    state = DONE;
}

int oz_selftest_ak1037_run(void)
{
    if (oz_ak1037_kv_claim(OZ_AK1037_KV_SELFTEST) != OZ_OK) {
        printf("oz.selftest: the lock is running (oz.start); reboot first\r\n");
        return OZ_E_ARG;
    }
    if (state != IDLE) {
        printf("oz.selftest: %s\r\n", state == RUNNING ? "already running" :
               "already ran this boot; reboot to run again");
        return OZ_E_ARG;
    }
    if (rt_thread_init(&st_thread, "oztest", selftest_thread, NULL, st_stack, sizeof st_stack,
                       OZ_SELFTEST_PRIORITY, 10) != RT_EOK)
        return OZ_E_HW;
    state = RUNNING;
    rt_thread_startup(&st_thread);
    return OZ_OK;
}

static void oz_selftest_cmd(char argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)oz_selftest_ak1037_run();
}

AK_MSH_CMD_EXPORT(oz_selftest_cmd, oz.selftest, run every ozcore suite and the silicon checks);
