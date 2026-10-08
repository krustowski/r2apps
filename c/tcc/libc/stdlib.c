#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "r2sys.h"

int errno;
char **environ;

/* ---- process ---- */

static void (*exit_fns[32])(void);
static int n_exit_fns;

int atexit(void (*fn)(void))
{
    if (n_exit_fns == 32)
        return -1;
    exit_fns[n_exit_fns++] = fn;
    return 0;
}

void _Exit(int code)
{
    r2_exit(0, code);
    for (;;)
        ;
}

void exit(int code)
{
    while (n_exit_fns)
        exit_fns[--n_exit_fns]();
    __stdio_flush_all();
    _Exit(code);
}

void abort(void)
{
    __stdio_flush_all();
    _Exit(134);
}

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
{
    fprintf(stderr, "%s:%u: %s: assertion `%s' failed\n", file, line, func, expr);
    abort();
}

/* r2 has no environment. */
char *getenv(const char *name)
{
    (void)name;
    return 0;
}

int system(const char *cmd)
{
    (void)cmd;
    errno = ENOSYS;
    return -1;
}

void *calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        errno = ENOMEM;
        return 0;
    }
    /* the kernel hands out zeroed memory */
    return malloc(n * size);
}

/*
 *  There are no links and no "." or ".." to resolve past what the kernel does
 *  itself: a path is real once it is absolute.
 */
char *realpath(const char *path, char *resolved)
{
    char cwd[PATH_MAX];
    char *out = resolved ? resolved : malloc(PATH_MAX);

    if (!out)
        return 0;
    if (path[0] == '/') {
        snprintf(out, PATH_MAX, "%s", path);
    } else if (getcwd(cwd, sizeof(cwd))) {
        size_t n = strlen(cwd);
        snprintf(out, PATH_MAX, "%s%s%s", cwd, n && cwd[n - 1] == '/' ? "" : "/", path);
    } else {
        if (!resolved)
            free(out);
        return 0;
    }
    return out;
}

/* ---- integers ---- */

/*
 *  The magnitude of the number at s, saturated at limit; *neg tells the sign.
 *  Sets *end past the digits, or to s when there are none.
 */
static unsigned long long parse_uint(const char *s, char **end, int base, int *neg,
                                     unsigned long long limit, int *range)
{
    const char *p = s;
    unsigned long long v = 0;
    int any = 0;

    *neg = 0;
    *range = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'z')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z')
            d = *p - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        any = 1;
        if (v > (limit - (unsigned)d) / (unsigned)base) {
            *range = 1;
            v = limit;
        } else if (!*range) {
            v = v * (unsigned)base + (unsigned)d;
        }
    }
    if (end)
        *end = (char *)(any ? p : s);
    return any ? v : 0;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, range;
    unsigned long long v = parse_uint(s, end, base, &neg, (unsigned long long)LLONG_MAX + 1, &range);

    if (!neg && v > (unsigned long long)LLONG_MAX) {
        range = 1;
        v = LLONG_MAX;
    }
    if (range) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)(0 - v) : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, range;
    unsigned long long v = parse_uint(s, end, base, &neg, ULLONG_MAX, &range);

    if (range) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? 0 - v : v;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoll(s, end, base);
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtoull(s, end, base);
}

intmax_t strtoimax(const char *s, char **end, int base)
{
    return strtoll(s, end, base);
}

uintmax_t strtoumax(const char *s, char **end, int base)
{
    return strtoull(s, end, base);
}

int atoi(const char *s)
{
    return (int)strtol(s, 0, 10);
}

long atol(const char *s)
{
    return strtol(s, 0, 10);
}

long long atoll(const char *s)
{
    return strtoll(s, 0, 10);
}

/* ---- floating point ---- */

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

/* neither infinite nor NaN */
static int finite_l(long double x)
{
    return x - x == 0;
}

static int match(const char *p, const char *word)
{
    while (*word)
        if (tolower((unsigned char)*p++) != *word++)
            return 0;
    return 1;
}

/*
 *  Decimal and hexadecimal floating constants, inf and nan.  Up to 19
 *  significant digits are kept in an integer and scaled by a power of ten in
 *  long double: a double comes out right but for the odd last bit in rare
 *  cases, a long double to within a few of its last bits.
 */
long double strtold(const char *s, char **end)
{
    const char *p = s;
    unsigned long long m = 0;
    int neg = 0, exp = 0, digits = 0, any = 0;
    long double v;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';

    if (match(p, "inf")) {
        p += match(p, "infinity") ? 8 : 3;
        v = HUGE_VALL;
        goto done;
    }
    if (match(p, "nan")) {
        p += 3;
        if (*p == '(') {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        v = (long double)NAN;
        goto done;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        int seen_dot = 0;
        p += 2;
        for (;; p++) {
            int d;
            if (*p == '.' && !seen_dot) {
                seen_dot = 1;
                continue;
            }
            if (!isxdigit((unsigned char)*p))
                break;
            d = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
            any = 1;
            if (m >> 60) {
                /* no room: the digit only counts towards the size */
                if (!seen_dot)
                    exp += 4;
            } else {
                m = m * 16 + (unsigned)d;
                if (seen_dot)
                    exp -= 4;
            }
        }
        if (any && (*p == 'p' || *p == 'P')) {
            char *e;
            long be = strtol(p + 1, &e, 10);
            if (e != p + 1) {
                p = e;
                exp += be > 100000 ? 100000 : be < -100000 ? -100000 : (int)be;
            }
        }
        v = ldexpl((long double)m, exp);
        goto done;
    }

    {
        int seen_dot = 0;
        for (;; p++) {
            if (*p == '.' && !seen_dot) {
                seen_dot = 1;
                continue;
            }
            if (*p < '0' || *p > '9')
                break;
            any = 1;
            if (digits < 19) {
                if (m || *p != '0')
                    digits++;
                m = m * 10 + (unsigned)(*p - '0');
                if (seen_dot)
                    exp--;
            } else if (!seen_dot) {
                exp++;
            }
        }
    }
    if (!any) {
        if (end)
            *end = (char *)s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        char *e;
        long de = strtol(p + 1, &e, 10);
        if (e != p + 1) {
            p = e;
            exp += de > 100000 ? 100000 : de < -100000 ? -100000 : (int)de;
        }
    }
    if (m == 0) {
        v = 0;
    } else if (exp > 5000) {
        v = HUGE_VALL;
    } else if (exp < -5000) {
        v = 0;
    } else if (exp >= 0) {
        v = (long double)m * ten_to(exp);
    } else if (exp >= -4900) {
        v = (long double)m / ten_to(-exp);
    } else {
        /* 10^-exp would overflow: come down in two steps */
        v = (long double)m / ten_to(4000) / ten_to(-exp - 4000);
    }
    if (m && (v == 0 || !finite_l(v)))
        errno = ERANGE;

done:
    if (end)
        *end = (char *)p;
    return neg ? -v : v;
}

double strtod(const char *s, char **end)
{
    long double v = strtold(s, end);
    double d = (double)v;

    if ((d == 0 && v != 0) || (!finite_l(d) && finite_l(v)))
        errno = ERANGE;
    return d;
}

float strtof(const char *s, char **end)
{
    long double v = strtold(s, end);
    float f = (float)v;

    if ((f == 0 && v != 0) || (!finite_l(f) && finite_l(v)))
        errno = ERANGE;
    return f;
}

double atof(const char *s)
{
    return strtod(s, 0);
}

/* ---- sorting and searching ---- */

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, size_t root, size_t n, size_t size,
                      int (*cmp)(const void *, const void *))
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (cmp(base + root * size, base + child * size) >= 0)
            return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

/* Heapsort: n log n at worst and no recursion. */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    size_t i;

    if (n < 2)
        return;
    for (i = n / 2; i-- > 0;)
        sift_down(b, i, n, size, cmp);
    for (i = n - 1; i > 0; i--) {
        swap_bytes(b, b + i * size, size);
        sift_down(b, 0, i, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = base;

    while (n) {
        const char *mid = b + (n / 2) * size;
        int c = cmp(key, mid);
        if (c == 0)
            return (void *)mid;
        if (c > 0) {
            b = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return 0;
}

/* ---- arithmetic ---- */

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

div_t div(int num, int den)
{
    div_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

ldiv_t ldiv(long num, long den)
{
    ldiv_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

static unsigned long rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)(rand_state >> 33);
}

void srand(unsigned int seed)
{
    rand_state = seed;
}
