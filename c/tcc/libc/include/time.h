#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

/* clock() counts the kernel's milliseconds since boot (10 ms steps). */
#define CLOCKS_PER_SEC 1000L

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

/* r2 keeps no time zone: the RTC is taken as UTC, and localtime is gmtime. */
time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
time_t mktime(struct tm *tm);

#endif
