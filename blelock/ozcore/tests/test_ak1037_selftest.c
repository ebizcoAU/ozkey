/* test_ak1037_selftest — the AK1037 self-test image on the host: oz.selftest
 * (oz_selftest_ak1037.c) runs every suite on the real port and the mock SDK,
 * then its silicon checks against the mock engine and TRNG (ozkey-49 P5-A).
 *
 *   test_ak1037_selftest good | wrong
 *
 *   good   everything passes: "OZ SELFTEST PASS"
 *   wrong  the SKE engine flips a bit. The suites still pass (AES falls back
 *          to software), the SKE check must say FAIL. With the oz_ct_equal()
 *          test inverted (6cc74ba) `good` fails and `wrong` passes.
 * Both: oz.start is refused after oz.selftest (one kv owner per boot), and a
 * second oz.selftest is refused.
 */
#include <stdlib.h>
#include <unistd.h>

#include "oz_test.h"
#include "mock_sdk.h"

static char out[1 << 20];

/* Run a shell command (and wait for thread `wait`, if given) with stdout
 * captured into out[], then echoed for the ctest log. */
static void run(const char *cmd, const char *wait)
{
    fflush(stdout);
    int saved = dup(1);
    FILE *tf = tmpfile();
    if (saved < 0 || !tf || dup2(fileno(tf), 1) < 0)
        abort();
    if (mock_msh_run(cmd))
        printf("no command %s\n", cmd);
    if (wait)
        mock_join(wait);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(tf);
    size_t n = fread(out, 1, sizeof out - 1, tf);
    out[n] = 0;
    fclose(tf);
    fputs(out, stdout);
}

static int has(const char *s) { return strstr(out, s) != NULL; }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "good";
    int good = !strcmp(mode, "good");
    if (!good && strcmp(mode, "wrong")) {
        printf("unknown mode %s\n", mode);
        return 2;
    }
    mock_flash_blank();
    mock_ske_mode = good ? MOCK_SKE_GOOD : MOCK_SKE_WRONG;

    run("oz.selftest", "oztest");
    expect_int("every suite passed", has("== lock PASS") && !has("== crypto FAIL") && !has("== lock FAIL") &&
               !has(" FAIL\r"), 1);
    expect_int("ske line", has(good ? "ske: 256 random blocks, 0 differ" : "FAIL ske: engine failed"), 1);
    expect_int("trng monobit", has("raw trng: monobit") && has("conditioned trng: monobit") &&
               !has("monobit FAIL"), 1);
    expect_int("no repeated TRNG blocks", has("trng: 0 repeated"), 1);
    expect_int("verdict", has(good ? "OZ SELFTEST PASS: 0 failing" : "OZ SELFTEST FAIL: 1 failing"), 1);

    run("oz.selftest", NULL);
    expect_int("second run refused", has("already ran this boot"), 1);
    run("oz.start", NULL);
    expect_int("lock refused after the self-test", has("reboot after oz.selftest"), 1);
    expect_int("so BLE never started", mock_ble_inits, 0);

    expect_int("flash stayed in OZKV", mock_flash_out_of_part, 0);
    expect_int("NOR rule kept", mock_nor_violations, 0);
    expect_int("SKE only with interrupts off", mock_irq_violations, 0);
    char suite[48];
    snprintf(suite, sizeof suite, "ak1037_selftest %s", mode);
    return report(suite);
}
