/*
 *  time.h --- what QuickJS's Date needs.  r2's clock is UTC, so localtime_r
 *  is gmtime: every page sees time zone offset 0 (src/libc.c).
 */
#ifndef JSR2_PORT_TIME_H
#define JSR2_PORT_TIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define localtime_r jsr2_localtime_r
#define gmtime_r jsr2_gmtime_r
#define clock_gettime jsr2_clock_gettime
typedef long time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
struct tm *localtime_r(const time_t *, struct tm *);
struct tm *gmtime_r(const time_t *, struct tm *);
int clock_gettime(int, struct timespec *);
#ifdef __cplusplus
}
#endif
#endif
