/* nano_printf.h — make the host's snprintf behave like the AK1037's (NANO-PRINTF-1).
 *
 * The chip links newlib-nano, whose printf family has no "ll", "z" or "j"
 * length modifiers: "%lld" comes out as the letter "d" and the argument is
 * never read. The host's full printf hid that, so ozcore built a signed
 * invite string differently on the chip (first on-chip self-test, 2026-11-18).
 *
 * CMakeLists.txt force-includes this header into every host build
 * (OZ_NANO_PRINTF, on by default), so a "%lld" in an snprintf fails here too.
 * Only snprintf/vsnprintf are mapped: they produce data. printf is display. */
#ifndef OZ_NANO_PRINTF_H
#define OZ_NANO_PRINTF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int oz_nano_snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int oz_nano_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

#ifdef __cplusplus
}
#endif

#ifndef OZ_NANO_PRINTF_IMPL
#undef snprintf
#undef vsnprintf
#define snprintf oz_nano_snprintf
#define vsnprintf oz_nano_vsnprintf
#endif

#endif
