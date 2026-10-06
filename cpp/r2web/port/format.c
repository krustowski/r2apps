/* MuJS needs real formatting, including Number.toFixed/toPrecision. The
 * compatibility archive's printf names are deliberately empty stubs. */
#define STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_NOUNALIGNED
#include "stb_sprintf.h"
#include <stdio.h>
#include <limits.h>

int r2js_vsnprintf(char *dst, size_t size, const char *fmt, va_list ap)
{
    return stbsp_vsnprintf(dst, size > INT_MAX ? INT_MAX : (int)size, fmt, ap);
}
int r2js_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = r2js_vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return n;
}
int r2js_sprintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = stbsp_vsprintf(dst, fmt, ap);
    va_end(ap);
    return n;
}
