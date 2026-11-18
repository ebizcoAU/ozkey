/* nano_printf.c — see nano_printf.h (NANO-PRINTF-1). */
#define OZ_NANO_PRINTF_IMPL 1
#include "nano_printf.h"

#include <stdlib.h>
#include <string.h>

/* CMake force-includes nano_printf.h before the line above defines the guard. */
#undef snprintf
#undef vsnprintf

/* Copy fmt, turning each conversion that uses "ll", "z" or "j" into just its
 * conversion letter, as newlib-nano prints it. Such a conversion never reads
 * its argument there, so any conversion after it would read the wrong one:
 * the host can't copy that faithfully, so it stops instead of guessing. */
static void nano_fmt(char *out, size_t cap, const char *fmt)
{
    size_t o = 0;
    int dropped = 0;
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { if (o + 1 < cap) out[o++] = *p; p++; continue; }
        const char *q = p + 1;
        if (*q == '%') { if (o + 2 < cap) { out[o++] = '%'; out[o++] = '%'; } p = q + 1; continue; }
        while (*q && strchr("-+ #0", *q)) q++;
        while (*q && (*q == '*' || (*q >= '0' && *q <= '9') || *q == '.')) q++;
        int unsupported = (q[0] == 'l' && q[1] == 'l') || *q == 'z' || *q == 'j';
        if (unsupported) {
            q += (q[0] == 'l') ? 2 : 1;
            if (*q && o + 1 < cap) out[o++] = *q;
            dropped = 1;
        } else {
            if (dropped) {
                fprintf(stderr, "nano_printf: a conversion follows an unsupported one in \"%s\"\n", fmt);
                abort();
            }
            for (const char *c = p; c <= q && *c; c++) if (o + 1 < cap) out[o++] = *c;
        }
        p = *q ? q + 1 : q;
    }
    out[o] = 0;
}

int oz_nano_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    char f[512];
    nano_fmt(f, sizeof f, fmt);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    return vsnprintf(buf, cap, f, ap);
#pragma GCC diagnostic pop
}

int oz_nano_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = oz_nano_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
