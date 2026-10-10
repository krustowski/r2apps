/*
 *  libc.c --- the C library QuickJS needs on r2, past what libc++r2 has.
 *
 *  Formatting is stb_sprintf under jsr2_ names; output and the clocks go to
 *  the platform hooks the program installed with jsr2::setPlatform()
 *  (engine.cpp keeps them; these are the C entry points to them).  r2 keeps
 *  UTC, so local time is UTC and every page sees a time zone offset of 0.
 *
 *  Only built for r2: on the host QuickJS uses the system's C library.
 */
#define STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_NOUNALIGNED
#define STB_SPRINTF_DECORATE(name) jsr2_stbsp_##name
#include "../../third_party/stb/stb_sprintf.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/time.h>
#include <time.h>

/* engine.cpp: the platform's log and clocks. */
void jsr2_platform_log(int level, const char *text, size_t len);
double jsr2_platform_epoch_ms(void);
unsigned long long jsr2_platform_monotonic_ms(void);

static FILE out_file = {1}, err_file = {2};
FILE *jsr2_stdout = &out_file, *jsr2_stderr = &err_file;

int jsr2_vsnprintf(char *dst, size_t size, const char *fmt, va_list ap)
{
    return jsr2_stbsp_vsnprintf(dst, size > INT_MAX ? INT_MAX : (int)size, fmt, ap);
}

int jsr2_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = jsr2_vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return n;
}

int jsr2_sprintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = jsr2_stbsp_vsprintf(dst, fmt, ap);
    va_end(ap);
    return n;
}

/*  What QuickJS prints itself (a dump, an assertion) goes to the log a line
 *  at a time; a line longer than the buffer is cut. */
int jsr2_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[256];
    int n = jsr2_vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = n < 0 ? 0 : (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    jsr2_platform_log(f == jsr2_stderr ? 3 : 0, buf, len);
    return n;
}

int jsr2_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = jsr2_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int jsr2_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = jsr2_vfprintf(jsr2_stdout, fmt, ap);
    va_end(ap);
    return n;
}

int jsr2_fputs(const char *s, FILE *f)
{
    jsr2_platform_log(f == jsr2_stderr ? 3 : 0, s, strlen(s));
    return 0;
}

int jsr2_fputc(int c, FILE *f)
{
    char ch = (char)c;
    jsr2_platform_log(f == jsr2_stderr ? 3 : 0, &ch, 1);
    return c;
}

int jsr2_putchar(int c) { return jsr2_fputc(c, jsr2_stdout); }
int jsr2_puts(const char *s) { return jsr2_fputs(s, jsr2_stdout); }

size_t jsr2_fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    jsr2_platform_log(f == jsr2_stderr ? 3 : 0, (const char *)p, size * n);
    return n;
}

int jsr2_fflush(FILE *f)
{
    (void)f;
    return 0;
}

void jsr2_assert_fail(const char *expr, const char *file, int line)
{
    char buf[200];
    int n = jsr2_snprintf(buf, sizeof(buf), "assertion failed: %s (%s:%d)", expr, file, line);
    jsr2_platform_log(3, buf, n < 0 ? 0 : (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    abort();
}

size_t jsr2_malloc_usable_size(void *p)
{
    (void)p;
    return 0;
}

int jsr2_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    double ms = jsr2_platform_epoch_ms();
    long long whole = (long long)ms;
    tv->tv_sec = (time_t)(whole / 1000);
    tv->tv_usec = (long)((whole % 1000) * 1000);
    return 0;
}

int jsr2_clock_gettime(int clock, struct timespec *ts)
{
    long long ms = clock == CLOCK_MONOTONIC ? (long long)jsr2_platform_monotonic_ms()
                                            : (long long)jsr2_platform_epoch_ms();
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)((ms % 1000) * 1000000);
    return 0;
}

/*  Days from 1970-01-01 to a civil date and back (Howard Hinnant's
 *  algorithms), for gmtime. */
static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

struct tm *jsr2_gmtime_r(const time_t *t, struct tm *tm)
{
    long secs = (long)*t;
    long days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}

struct tm *jsr2_localtime_r(const time_t *t, struct tm *tm) { return jsr2_gmtime_r(t, tm); }
