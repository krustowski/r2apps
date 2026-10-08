/*
 *  Wall-clock time from the RTC, taken as UTC.  The RTC only counts seconds,
 *  so gettimeofday() adds the kernel's millisecond ticks to the second it
 *  read first, which keeps it moving forward smoothly.
 */
#include <sys/time.h>
#include <time.h>

#include "r2sys.h"

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm). */
static long days_from_civil(long y, int m, int d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

time_t time(time_t *t)
{
    RTC_T rtc = {0, 0, 0, 1, 1, 1970};
    time_t now;

    read_rtc(&rtc);
    now = (time_t)days_from_civil(rtc.year, rtc.month, rtc.day) * 86400 + rtc.hours * 3600 +
          rtc.minutes * 60 + rtc.seconds;
    if (t)
        *t = now;
    return now;
}

clock_t clock(void)
{
    return (clock_t)get_ticks();
}

double difftime(time_t a, time_t b)
{
    return (double)(a - b);
}

int gettimeofday(struct timeval *tv, void *tz)
{
    static time_t base;
    static unsigned long base_ticks;
    unsigned long ms;

    (void)tz;
    if (!base) {
        base = time(0);
        base_ticks = get_ticks();
    }
    ms = get_ticks() - base_ticks;
    tv->tv_sec = base + (time_t)(ms / 1000);
    tv->tv_usec = (long)(ms % 1000) * 1000;
    return 0;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    long days = *t / 86400, secs = *t % 86400, z, era, doe, yoe, y, doy, mp;

    if (secs < 0) {
        secs += 86400;
        days--;
    }
    tm.tm_sec = (int)(secs % 60);
    tm.tm_min = (int)(secs / 60 % 60);
    tm.tm_hour = (int)(secs / 3600);
    tm.tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    tm.tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm.tm_mon = (int)(mp < 10 ? mp + 2 : mp - 10);
    y += tm.tm_mon <= 1;
    tm.tm_year = (int)(y - 1900);
    tm.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm.tm_isdst = 0;
    return &tm;
}

struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

time_t mktime(struct tm *tm)
{
    long y = tm->tm_year + 1900L + tm->tm_mon / 12;
    int m = tm->tm_mon % 12;
    time_t t;

    if (m < 0) {
        m += 12;
        y--;
    }
    t = (time_t)days_from_civil(y, m + 1, 1) * 86400 + (tm->tm_mday - 1) * 86400L +
        tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    *tm = *gmtime(&t);
    return t;
}
