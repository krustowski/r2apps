/*
 *  time.cpp — the RTC, and how to render what it says.
 */

#include "r2/io.hpp"
#include "r2/libc.hpp"
#include "r2/time.hpp"

namespace r2 {

optional<RtcTime> clock_now() {
    RtcTime value;
    memset(&value, 0, sizeof(value));

    if (raw_syscall(Sys::Rtc, 0x01, (int64_t)&value) != 0)
        return nullopt;

    return value;
}

string format_time(const RtcTime &t) {
    string result;
    StringWriter sink(result);

    format_uint(sink, t.hours, 10, 2, '0');
    sink.put(':');
    format_uint(sink, t.minutes, 10, 2, '0');
    sink.put(':');
    format_uint(sink, t.seconds, 10, 2, '0');

    return result;
}

string format_date(const RtcTime &t) {
    string result;
    StringWriter sink(result);

    format_uint(sink, t.year, 10, 4, '0');
    sink.put('-');
    format_uint(sink, t.month, 10, 2, '0');
    sink.put('-');
    format_uint(sink, t.day, 10, 2, '0');

    return result;
}

} // namespace r2
