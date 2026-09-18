#ifndef _R2CXX_IO_HPP_
#define _R2CXX_IO_HPP_

/*
 *  io.hpp — writing to the console.
 *
 *  Everything ends at syscall 0x10, which takes a pointer and a length and
 *  prints bytes until it hits a NUL.  On top of that sits a buffered writer
 *  and two ways to format:
 *
 *      print("pid ", pid, " ready\n");          // concatenation
 *      printf("pid {} of {}\n", pid, total);    // placeholders
 *      string s = format("{}:{}", host, port);  // same, into a string
 *
 *  Both are type-safe variadic templates: there is no format string to get out
 *  of step with its arguments, and no va_arg to read a pointer as an int.
 *
 *  The formatting layer is written against a sink --- anything with put(char)
 *  and write(string_view) --- so the console writer and the string builder
 *  share one set of overloads.  A program can add its own types to it by
 *  declaring
 *
 *      template <class W> void format_value(W &w, const MyType &v);
 *
 *  in the namespace of MyType; argument-dependent lookup finds it.
 */

#include "string.hpp"
#include "syscall.hpp"

namespace r2 {

/* ------------------------------------------------------------------------ *
 *  Sinks
 * ------------------------------------------------------------------------ */

/*
 *  A buffered console writer.  Bytes accumulate until the buffer fills or a
 *  newline goes past, so a line of output costs one syscall rather than one
 *  per character.
 */
class Writer {
  public:
    static constexpr size_t BUFFER_SIZE = 256;

    Writer() noexcept : len_(0) {}
    ~Writer() { flush(); }

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    void put(char c);
    void write(string_view text);
    void flush();

  private:
    char buf_[BUFFER_SIZE];
    size_t len_;
};

/*  The process-wide console writer, flushed at exit by the runtime.  */
extern Writer out;

/*  A sink that appends to a string.  */
class StringWriter {
  public:
    explicit StringWriter(string &target) noexcept : target_(target) {}

    void put(char c) { target_.append(c); }
    void write(string_view text) { target_.append(text); }
    void flush() {}

  private:
    string &target_;
};

/* ------------------------------------------------------------------------ *
 *  Manipulators
 * ------------------------------------------------------------------------ */

struct Hex {
    uint64_t value;
    unsigned width;
    bool upper;
};
inline Hex hex(uint64_t value, unsigned width = 0, bool upper = false) {
    return Hex{value, width, upper};
}

struct Bin {
    uint64_t value;
    unsigned width;
};
inline Bin bin(uint64_t value, unsigned width = 0) { return Bin{value, width}; }

struct Pad {
    int64_t value;
    unsigned width;
    char fill;
};
inline Pad pad(int64_t value, unsigned width, char fill = ' ') { return Pad{value, width, fill}; }

struct Fixed {
    double value;
    unsigned decimals;
};
inline Fixed fixed(double value, unsigned decimals = 3) { return Fixed{value, decimals}; }

/* ------------------------------------------------------------------------ *
 *  Number formatting
 * ------------------------------------------------------------------------ */

namespace detail {

/*  64 binary digits is the widest any base can be; no allocation involved.  */
inline constexpr size_t DIGITS_MAX = 64;

template <class Sink>
void emit_digits(Sink &w, uint64_t value, unsigned base, size_t width, char fill, bool upper,
                 bool negative) {
    if (base < 2 || base > 16)
        base = 10;

    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[DIGITS_MAX];
    size_t n = 0;

    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0 && n < DIGITS_MAX);

    size_t len = n + (negative ? 1 : 0);

    /*  Zero padding goes after the sign, spaces before it.  */
    if (fill == '0') {
        if (negative)
            w.put('-');
        for (size_t i = len; i < width; i++)
            w.put('0');
    } else {
        for (size_t i = len; i < width; i++)
            w.put(fill);
        if (negative)
            w.put('-');
    }

    while (n > 0)
        w.put(tmp[--n]);
}

} // namespace detail

template <class Sink>
void format_uint(Sink &w, uint64_t value, unsigned base = 10, size_t width = 0, char fill = ' ',
                 bool upper = false) {
    detail::emit_digits(w, value, base, width, fill, upper, false);
}

template <class Sink>
void format_int(Sink &w, int64_t value, unsigned base = 10, size_t width = 0, char fill = ' ') {
    bool negative = value < 0;
    /*  Negating through uint64_t so that INT64_MIN survives the conversion.  */
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    detail::emit_digits(w, magnitude, base, width, fill, false, negative);
}

template <class Sink> void format_double(Sink &w, double value, unsigned decimals) {
    if (value != value) { /*  NaN is the only value not equal to itself  */
        w.write(string_view("nan"));
        return;
    }

    bool negative = value < 0.0;
    if (negative) {
        value = -value;
        w.put('-');
    }

    if (value > 1.8e19) {
        w.write(string_view("inf"));
        return;
    }

    if (decimals > 9)
        decimals = 9;

    uint64_t scale = 1;
    for (unsigned i = 0; i < decimals; i++)
        scale *= 10;

    /*  Round half away from zero at the last printed digit.  */
    uint64_t scaled = (uint64_t)(value * (double)scale + 0.5);
    uint64_t whole = scaled / scale;
    uint64_t frac = scaled % scale;

    format_uint(w, whole);
    if (decimals > 0) {
        w.put('.');
        format_uint(w, frac, 10, decimals, '0');
    }
}

/* ------------------------------------------------------------------------ *
 *  format_value: one overload per type the library knows how to print
 * ------------------------------------------------------------------------ */

template <class Sink> void format_value(Sink &w, string_view v) { w.write(v); }
template <class Sink> void format_value(Sink &w, const char *v) { w.write(string_view(v)); }
template <class Sink> void format_value(Sink &w, char *v) { w.write(string_view(v)); }
template <class Sink> void format_value(Sink &w, const uint8_t *v) { w.write(string_view(v)); }
template <class Sink> void format_value(Sink &w, const string &v) { w.write(v.view()); }
template <class Sink> void format_value(Sink &w, char v) { w.put(v); }

template <class Sink> void format_value(Sink &w, bool v) {
    w.write(v ? string_view("true") : string_view("false"));
}

template <class Sink> void format_value(Sink &w, signed char v) { format_int(w, v); }
template <class Sink> void format_value(Sink &w, short v) { format_int(w, v); }
template <class Sink> void format_value(Sink &w, int v) { format_int(w, v); }
template <class Sink> void format_value(Sink &w, long v) { format_int(w, v); }
template <class Sink> void format_value(Sink &w, long long v) { format_int(w, (int64_t)v); }

template <class Sink> void format_value(Sink &w, unsigned char v) { format_uint(w, v); }
template <class Sink> void format_value(Sink &w, unsigned short v) { format_uint(w, v); }
template <class Sink> void format_value(Sink &w, unsigned int v) { format_uint(w, v); }
template <class Sink> void format_value(Sink &w, unsigned long v) { format_uint(w, v); }
template <class Sink> void format_value(Sink &w, unsigned long long v) {
    format_uint(w, (uint64_t)v);
}

template <class Sink> void format_value(Sink &w, double v) { format_double(w, v, 3); }
template <class Sink> void format_value(Sink &w, float v) { format_double(w, (double)v, 3); }

template <class Sink> void format_value(Sink &w, const void *v) {
    w.write(string_view("0x"));
    format_uint(w, (uint64_t)v, 16, 0, '0');
}

template <class Sink> void format_value(Sink &w, Hex v) {
    format_uint(w, v.value, 16, v.width, '0', v.upper);
}
template <class Sink> void format_value(Sink &w, Bin v) { format_uint(w, v.value, 2, v.width, '0'); }
template <class Sink> void format_value(Sink &w, Pad v) {
    format_int(w, v.value, 10, v.width, v.fill);
}
template <class Sink> void format_value(Sink &w, Fixed v) { format_double(w, v.value, v.decimals); }

/* ------------------------------------------------------------------------ *
 *  print / println / printf / format
 * ------------------------------------------------------------------------ */

namespace detail {

template <class Sink> void substitute(Sink &w, string_view fmt) { w.write(fmt); }

template <class Sink, class T, class... Rest>
void substitute(Sink &w, string_view fmt, const T &value, const Rest &...rest) {
    size_t at = fmt.find(string_view("{}"));
    if (at == npos) {
        w.write(fmt);
        return;
    }
    w.write(fmt.substr(0, at));
    format_value(w, value);
    substitute(w, fmt.substr(at + 2), rest...);
}

} // namespace detail

/*  Writes each argument in turn, then flushes.  */
template <class... Args> void print(const Args &...args) {
    (format_value(out, args), ...);
    out.flush();
}

template <class... Args> void println(const Args &...args) {
    (format_value(out, args), ...);
    out.put('\n');
    out.flush();
}

/*  Substitutes each {} in fmt with the next argument.  Surplus arguments are
 *  dropped; a {} with no argument left is written through unchanged.  */
template <class... Args> void printf(string_view fmt, const Args &...args) {
    detail::substitute(out, fmt, args...);
    out.flush();
}

/*  The same substitution, returned as a string rather than printed.  */
template <class... Args> string format(string_view fmt, const Args &...args) {
    string result;
    StringWriter sink(result);
    detail::substitute(sink, fmt, args...);
    return result;
}

/*  Concatenates its arguments into a string.  */
template <class... Args> string concat(const Args &...args) {
    string result;
    StringWriter sink(result);
    (format_value(sink, args), ...);
    return result;
}

/*  Unbuffered write, for the last thing a program says before it dies.  */
void write_console(string_view text);

/*  Clears the console (syscall 0x11).  */
inline void clear_screen() { raw_syscall(Sys::ClearScreen, 0, 0); }

} // namespace r2

#endif
