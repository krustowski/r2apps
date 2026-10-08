/*
 *  The printf family: one formatter writing to a sink, which is a string for
 *  the sprintf calls and a stream for the rest.
 *
 *  Floating point is converted through long double, exact to 18 significant
 *  digits; digits past those print as 0 (glibc gives the binary value's exact
 *  expansion there).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct sink {
    char *buf;    /* string sink, or 0 */
    size_t size;  /* room in buf, terminator included */
    FILE *file;   /* stream sink, or 0 */
    size_t count; /* bytes produced so far, stored or not */
    char tmp[256];
    size_t ntmp;
};

static void flush_tmp(struct sink *s)
{
    if (s->ntmp) {
        fwrite(s->tmp, 1, s->ntmp, s->file);
        s->ntmp = 0;
    }
}

static void out(struct sink *s, const char *p, size_t n)
{
    if (s->file) {
        while (n) {
            size_t k = sizeof(s->tmp) - s->ntmp;
            if (k > n)
                k = n;
            memcpy(s->tmp + s->ntmp, p, k);
            s->ntmp += k;
            p += k;
            n -= k;
            s->count += k;
            if (s->ntmp == sizeof(s->tmp))
                flush_tmp(s);
        }
        return;
    }
    if (s->count + 1 < s->size) {
        size_t k = s->size - 1 - s->count;
        memcpy(s->buf + s->count, p, k < n ? k : n);
    }
    s->count += n;
}

static void pad(struct sink *s, char c, int n)
{
    char block[32];

    if (n <= 0)
        return;
    memset(block, c, sizeof(block));
    while (n > 0) {
        int k = n < (int)sizeof(block) ? n : (int)sizeof(block);
        out(s, block, (size_t)k);
        n -= k;
    }
}

#define FL_LEFT 1
#define FL_PLUS 2
#define FL_SPACE 4
#define FL_ALT 8
#define FL_ZERO 16

/*
 *  Lay out one field: prefix (sign, 0x), then zeros to reach a minimum number
 *  of body digits, then the body, padded to width with spaces or zeros.
 */
static void field(struct sink *s, const char *prefix, const char *body, int blen, int zeros,
                  int width, int flags)
{
    int plen = (int)strlen(prefix);
    int fill = width - plen - zeros - blen;

    if (!(flags & FL_LEFT) && !(flags & FL_ZERO))
        pad(s, ' ', fill);
    out(s, prefix, (size_t)plen);
    if (!(flags & FL_LEFT) && (flags & FL_ZERO))
        pad(s, '0', fill);
    pad(s, '0', zeros);
    out(s, body, (size_t)blen);
    if (flags & FL_LEFT)
        pad(s, ' ', fill);
}

static void fmt_int(struct sink *s, uint64_t v, int neg, int base, int upper, int width, int prec,
                    int flags)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24], prefix[4] = "";
    int n = 0, zeros;

    while (v) {
        buf[sizeof(buf) - 1 - n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    if (prec < 0)
        prec = 1;
    else
        flags &= ~FL_ZERO;
    zeros = prec > n ? prec - n : 0;

    if (neg)
        strcpy(prefix, "-");
    else if (flags & FL_PLUS)
        strcpy(prefix, "+");
    else if (flags & FL_SPACE)
        strcpy(prefix, " ");
    if ((flags & FL_ALT) && n) {
        if (base == 16)
            strcat(prefix, upper ? "0X" : "0x");
        else if (base == 8 && !zeros)
            zeros = 1;
    }
    field(s, prefix, buf + sizeof(buf) - n, n, zeros, width, flags);
}

/* ---- floating point ---- */

static uint64_t pow10u(int k)
{
    uint64_t r = 1;
    while (k--)
        r *= 10;
    return r;
}

static long double ten_to(int k)
{
    long double r = 1, b = 10;
    while (k) {
        if (k & 1)
            r *= b;
        b *= b;
        k >>= 1;
    }
    return r;
}

/* v × 10^k, in steps that stay inside long double's range */
static long double scale10(long double v, int k)
{
    while (k > 4000) {
        v *= ten_to(4000);
        k -= 4000;
    }
    while (k < -4000) {
        v /= ten_to(4000);
        k += 4000;
    }
    return k >= 0 ? v * ten_to(k) : v / ten_to(-k);
}

/* The binary exponent of an x87 long double, from its bits. */
static int exp2_of(long double v)
{
    union {
        long double v;
        uint16_t w[5];
    } u;
    u.v = v;
    return (u.w[4] & 0x7fff) - 16383;
}

#define MAX_SIG 18

/*
 *  The first n (1..18) significant digits of v > 0, rounded, into d; returns
 *  the decimal exponent of d[0].  The estimate from the binary exponent is off
 *  by one or two, and the loop settles it; a subnormal long double takes it
 *  further.
 */
static int sig_digits(long double v, int n, char *d)
{
    int e = (int)((exp2_of(v) * 30103L) / 100000L), i;
    uint64_t lo = pow10u(n - 1), hi = lo * 10, N = 0;

    for (i = 0; i < 64; i++) {
        long double x = scale10(v, n - 1 - e) + 0.5L;
        N = x >= (long double)hi ? hi : (uint64_t)x;
        if (N >= hi)
            e++;
        else if (N < lo)
            e--;
        else
            break;
    }
    for (i = n - 1; i >= 0; i--) {
        d[i] = (char)('0' + N % 10);
        N /= 10;
    }
    return e;
}

/* %f: digits d (nd of them, d[0] at 10^e), prec fraction digits */
static int put_fixed(char *o, const char *d, int nd, int e, int prec, int alt)
{
    int n = 0, i;

    if (e < 0) {
        o[n++] = '0';
    } else {
        for (i = 0; i <= e; i++)
            o[n++] = i < nd ? d[i] : '0';
    }
    if (prec || alt)
        o[n++] = '.';
    for (i = 1; i <= prec; i++) {
        int idx = e + i;
        o[n++] = idx >= 0 && idx < nd ? d[idx] : '0';
    }
    return n;
}

/* %e: digits d (nd of them), exponent e */
static int put_exp(char *o, const char *d, int nd, int e, int prec, int alt, int upper)
{
    int n = 0, i;

    o[n++] = d[0];
    if (prec || alt)
        o[n++] = '.';
    for (i = 1; i <= prec; i++)
        o[n++] = i < nd ? d[i] : '0';
    o[n++] = upper ? 'E' : 'e';
    o[n++] = e < 0 ? '-' : '+';
    if (e < 0)
        e = -e;
    if (e >= 1000)
        o[n++] = (char)('0' + e / 1000);
    if (e >= 100)
        o[n++] = (char)('0' + e / 100 % 10);
    o[n++] = (char)('0' + e / 10 % 10);
    o[n++] = (char)('0' + e % 10);
    return n;
}

/* %f of v > 0 (or 0) */
static int fixed(char *o, long double v, int prec, int alt)
{
    char d[MAX_SIG];
    int e, n;

    if (v == 0)
        return put_fixed(o, "0", 1, 0, prec, alt);
    e = sig_digits(v, MAX_SIG, d);
    n = e + 1 + prec; /* significant digits the output shows */
    if (n <= 0) {
        /* below the last place shown: it rounds to 0 or to one unit there */
        if (n == 0 && scale10(v, prec) >= 0.5L)
            return put_fixed(o, "1", 1, -prec, prec, alt);
        return put_fixed(o, "0", 1, 0, prec, alt);
    }
    if (n < MAX_SIG)
        e = sig_digits(v, n, d);
    else
        n = MAX_SIG;
    return put_fixed(o, d, n, e, prec, alt);
}

static int expo(char *o, long double v, int prec, int alt, int upper)
{
    char d[MAX_SIG];
    int n = prec + 1 < MAX_SIG ? prec + 1 : MAX_SIG;
    int e = v == 0 ? (memset(d, '0', (size_t)n), 0) : sig_digits(v, n, d);

    return put_exp(o, d, n, e, prec, alt, upper);
}

/* drop trailing fraction zeros (and a bare point), as %g does without # */
static int strip_zeros(char *o, int n)
{
    int dot = -1, ex = n, i, k;

    for (i = 0; i < n; i++) {
        if (o[i] == '.')
            dot = i;
        if (o[i] == 'e' || o[i] == 'E') {
            ex = i;
            break;
        }
    }
    if (dot < 0)
        return n;
    k = ex;
    while (k > dot + 1 && o[k - 1] == '0')
        k--;
    if (k == dot + 1)
        k = dot;
    memmove(o + k, o + ex, (size_t)(n - ex));
    return n - (ex - k);
}

static int general(char *o, long double v, int prec, int alt, int upper)
{
    char d[MAX_SIG];
    int P = prec == 0 ? 1 : prec, x, n;

    x = v == 0 ? 0 : sig_digits(v, P < MAX_SIG ? P : MAX_SIG, d);
    if (P > x && x >= -4)
        n = fixed(o, v, P - 1 - x, alt);
    else
        n = expo(o, v, P - 1, alt, upper);
    return alt ? n : strip_zeros(o, n);
}

static void fmt_float(struct sink *s, long double v, char conv, int width, int prec, int flags)
{
    union {
        long double v;
        uint16_t w[5];
        uint64_t m;
    } u;
    /* %f of a value below 10^4000 with 4000 decimals, and a little */
    static char o[8064];
    const char *prefix = "";
    int upper = conv == 'F' || conv == 'E' || conv == 'G', n;

    u.v = v;
    if (u.w[4] & 0x8000) {
        prefix = "-";
        v = -v;
    } else if (flags & FL_PLUS) {
        prefix = "+";
    } else if (flags & FL_SPACE) {
        prefix = " ";
    }
    if ((u.w[4] & 0x7fff) == 0x7fff) {
        /* infinity has only the integer bit of the mantissa set */
        int nan = (u.m << 1) != 0;
        field(s, prefix, nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3, 0, width,
              flags & ~FL_ZERO);
        return;
    }
    if (prec < 0)
        prec = 6;
    if (prec > 4000)
        prec = 4000;

    switch (conv) {
    case 'f':
    case 'F':
        /* past 10^4000 there is no room for the integer digits: say it in %e */
        n = v >= ten_to(4000) ? expo(o, v, prec, flags & FL_ALT, upper)
                              : fixed(o, v, prec, flags & FL_ALT);
        break;
    case 'e':
    case 'E':
    case 'a':
    case 'A':
        n = expo(o, v, prec, flags & FL_ALT, upper);
        break;
    default:
        n = general(o, v, prec, flags & FL_ALT, upper);
        break;
    }
    field(s, prefix, o, n, 0, width, flags);
}

static int format(struct sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        int flags = 0, width = 0, prec = -1, lng = 0;
        const char *start = fmt;
        uint64_t uv;
        int64_t sv;

        if (*fmt != '%') {
            const char *p = fmt;
            while (p[1] && p[1] != '%')
                p++;
            out(s, fmt, (size_t)(p - fmt + 1));
            fmt = p;
            continue;
        }

        for (;;) {
            switch (*++fmt) {
            case '-': flags |= FL_LEFT; continue;
            case '+': flags |= FL_PLUS; continue;
            case ' ': flags |= FL_SPACE; continue;
            case '#': flags |= FL_ALT; continue;
            case '0': flags |= FL_ZERO; continue;
            }
            break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FL_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + *fmt++ - '0';
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + *fmt++ - '0';
            }
        }
        if (flags & FL_LEFT)
            flags &= ~FL_ZERO;

        /* lng: -2 hh, -1 h, 0 int, 1 long and the like, 2 long double */
        for (;; fmt++) {
            if (*fmt == 'h')
                lng = lng ? -2 : -1;
            else if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q')
                lng = 1;
            else if (*fmt == 'L')
                lng = 2;
            else
                break;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            sv = lng >= 1 ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (lng == -1)
                sv = (short)sv;
            else if (lng == -2)
                sv = (signed char)sv;
            uv = sv < 0 ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
            fmt_int(s, uv, sv < 0, 10, 0, width, prec, flags);
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            uv = lng >= 1 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            if (lng == -1)
                uv = (unsigned short)uv;
            else if (lng == -2)
                uv = (unsigned char)uv;
            fmt_int(s, uv, 0, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16, *fmt == 'X', width, prec,
                    flags & ~(FL_PLUS | FL_SPACE));
            break;
        case 'p': {
            void *p = va_arg(ap, void *);
            if (p)
                fmt_int(s, (uint64_t)p, 0, 16, 0, width, prec, (flags & FL_LEFT) | FL_ALT);
            else
                field(s, "", "(nil)", 5, 0, width, flags & FL_LEFT);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            field(s, "", &c, 1, 0, width, flags & FL_LEFT);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            int n = 0;
            if (!str)
                str = "(null)";
            while (str[n] && (prec < 0 || n < prec))
                n++;
            field(s, "", str, n, 0, width, flags & FL_LEFT);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            long double v = lng == 2 ? va_arg(ap, long double) : va_arg(ap, double);
            fmt_float(s, v, *fmt, width, prec, flags);
            break;
        }
        case 'n':
            if (lng >= 1)
                *va_arg(ap, int64_t *) = (int64_t)s->count;
            else
                *va_arg(ap, int *) = (int)s->count;
            break;
        case '%':
            out(s, "%", 1);
            break;
        default:
            /* not a conversion: print it as it stands */
            out(s, start, (size_t)(fmt - start + (*fmt != 0)));
            if (!*fmt)
                fmt--;
            break;
        }
    }
    return (int)s->count;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s;
    int n;

    s.buf = buf;
    s.size = size;
    s.file = 0;
    s.count = 0;
    s.ntmp = 0;
    n = format(&s, fmt, ap);
    if (size)
        buf[s.count < size ? s.count : size - 1] = 0;
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct sink s;
    int n;

    s.buf = 0;
    s.size = 0;
    s.file = f;
    s.count = 0;
    s.ntmp = 0;
    n = format(&s, fmt, ap);
    flush_tmp(&s);
    return ferror(f) ? -1 : n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}
