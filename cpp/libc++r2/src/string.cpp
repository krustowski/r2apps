/*
 *  string.cpp — the out-of-line parts of r2::string, and number conversion.
 */

#include "r2/io.hpp"
#include "r2/libc.hpp"
#include "r2/string.hpp"

namespace r2 {

bool string::reserve(size_t new_cap) {
    if (new_cap <= cap_)
        return true;

    char *fresh = (char *)::operator new(new_cap + 1, std::nothrow);
    if (!fresh) {
        failed_ = true;
        return false;
    }

    memcpy(fresh, data_, size_);
    fresh[size_] = '\0';

    if (on_heap())
        ::operator delete(data_);

    data_ = fresh;
    cap_ = new_cap;
    return true;
}

bool string::append(string_view sv) {
    if (sv.empty())
        return true;

    size_t needed = size_ + sv.size();
    if (needed > cap_) {
        /*  Doubling, so that building a string one piece at a time does not
         *  reallocate on every append.  */
        size_t target = cap_ * 2;
        if (target < needed)
            target = needed;
        if (!reserve(target))
            return false;
    }

    memcpy(data_ + size_, sv.data(), sv.size());
    size_ = needed;
    data_[size_] = '\0';
    return true;
}

string to_string(int64_t value, unsigned base, size_t width, char pad) {
    string result;
    StringWriter sink(result);
    format_int(sink, value, base, width, pad);
    return result;
}

string to_string(uint64_t value, unsigned base, size_t width, char pad) {
    string result;
    StringWriter sink(result);
    format_uint(sink, value, base, width, pad);
    return result;
}

string to_string(double value, unsigned decimals) {
    string result;
    StringWriter sink(result);
    format_double(sink, value, decimals);
    return result;
}

namespace {

/*  Digit value of c in the given base, or -1 when it is not one.  */
int digit_value(char c, unsigned base) {
    int value;
    if (c >= '0' && c <= '9')
        value = c - '0';
    else if (c >= 'a' && c <= 'z')
        value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        value = c - 'A' + 10;
    else
        return -1;

    return value < (int)base ? value : -1;
}

} // namespace

bool parse_uint(string_view text, uint64_t &out, unsigned base) {
    text = text.trim();
    if (text.empty() || base < 2 || base > 36)
        return false;

    /*  Accept the 0x prefix when the base says hexadecimal.  */
    if (base == 16 && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.remove_prefix(2);

    uint64_t value = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int digit = digit_value(text[i], base);
        if (digit < 0)
            return false;

        uint64_t next = value * base + (uint64_t)digit;
        if (next < value) /*  overflowed  */
            return false;
        value = next;
    }

    out = value;
    return true;
}

bool parse_int(string_view text, int64_t &out, unsigned base) {
    text = text.trim();
    if (text.empty())
        return false;

    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        text.remove_prefix(1);
    }

    uint64_t magnitude = 0;
    if (!parse_uint(text, magnitude, base))
        return false;

    if (negative) {
        if (magnitude > 0x8000000000000000ULL)
            return false;
        out = magnitude == 0x8000000000000000ULL ? (int64_t)0x8000000000000000ULL
                                                 : -(int64_t)magnitude;
    } else {
        if (magnitude > 0x7FFFFFFFFFFFFFFFULL)
            return false;
        out = (int64_t)magnitude;
    }
    return true;
}

} // namespace r2
