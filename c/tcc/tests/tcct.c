/*
 *  tcct --- a codegen and loader smoke test for compilers targeting r2.
 *
 *  Each check appends "ok <name>" or "FAIL <name>" to a buffer; the buffer is
 *  printed and written to /mnt/fat/TCCT.TXT so the host can read it back with
 *  mtype while the VM runs.
 */
#include "printf.h"
#include "syscall.h"

#ifdef __TINYC__
#define COMPILER "tcc"
#elif defined(__GNUC__)
#define COMPILER "gcc"
#else
#define COMPILER "?"
#endif

static char out[2048];
static uint32_t out_len;
static int passed, total;

static void put(const char *s)
{
    while (*s && out_len < sizeof(out) - 1)
        out[out_len++] = *s++;
    out[out_len] = 0;
}

static void put_num(int64_t v)
{
    char tmp[24];
    int i = 0;
    uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;

    if (v < 0)
        put("-");
    do {
        tmp[i++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (i)
        out[out_len < sizeof(out) - 1 ? out_len++ : out_len] = tmp[--i];
    out[out_len] = 0;
}

static void check(const char *name, int ok)
{
    total++;
    passed += ok;
    put(ok ? "ok   " : "FAIL ");
    put(name);
    put("\n");
}

/* .data, .bss, .rodata */
static int data_word = 0x12345678;
static int64_t bss_words[64];
static const char rodata_str[] = "rodata";

struct pair {
    int64_t a;
    int32_t b;
    char c;
};

static struct pair make_pair(int64_t a, int32_t b, char c)
{
    struct pair p;
    p.a = a;
    p.b = b;
    p.c = c;
    return p;
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }

static int classify(int x)
{
    switch (x) {
    case 0: return 10;
    case 1: return 11;
    case 2: return 12;
    case 3: return 13;
    case 4: return 14;
    case 5: return 15;
    case 6: return 16;
    case 7: return 17;
    default: return -1;
    }
}

/* Calls through printf's own va_arg, then checks our own variadic sum. */
static int64_t vsum(int n, ...)
{
    va_list ap;
    int64_t s = 0;

    va_start(ap, n);
    while (n--)
        s += va_arg(ap, int64_t);
    va_end(ap);
    return s;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int main(int argc, char **argv)
{
    put("tcct built by " COMPILER "\n");

    check("data", data_word == 0x12345678);

    int bss_zero = 1;
    for (int i = 0; i < 64; i++)
        bss_zero &= bss_words[i] == 0;
    check("bss", bss_zero);

    check("rodata", streq(rodata_str, "rodata"));

    struct pair p = make_pair(-5000000000LL, 77, 'z');
    check("struct-return", p.a == -5000000000LL && p.b == 77 && p.c == 'z');

    check("recursion", fib(20) == 6765);

    int (*ops[2])(int, int) = {add, sub};
    check("fnptr", ops[0](7, 5) == 12 && ops[1](7, 5) == 2);

    int sw = 0;
    for (int i = -1; i < 9; i++)
        sw += classify(i);
    check("switch", sw == 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 - 2);

    volatile int64_t big = 0x7fffffffffffLL;
    volatile int64_t neg = -1234567890123LL;
    check("int64", big * 3 / 7 == 0x7fffffffffffLL * 3 / 7 && neg % 1000 == -123 &&
                       (neg >> 4) == -77160493133LL && (uint64_t)neg / 3 == 6148914279713887164ULL);

    volatile double d = 3.5;
    volatile float f = 0.25f;
    check("float", (int)(d * 2.0 + f) == 7 && (int64_t)(d * 1e9) == 3500000000LL);

    check("varargs", vsum(5, (int64_t)1, (int64_t)2, (int64_t)3, (int64_t)4,
                          (int64_t)5000000000LL) == 5000000010LL);

    uint8_t *m = malloc(4096);
    int heap_ok = m != 0;
    if (heap_ok) {
        for (int i = 0; i < 4096; i++)
            m[i] = (uint8_t)i;
        m = realloc(m, 16384);
        heap_ok = m != 0 && m[4095] == 255 && m[100] == 100;
        free(m);
    }
    check("heap", heap_ok);

    check("argv", argc == 3 && streq(argv[1], "alpha") && streq(argv[2], "beta"));

    put("passed ");
    put_num(passed);
    put("/");
    put_num(total);
    put("\n");

    printf((const uint8_t *)"%s", out);
    printf((const uint8_t *)"printf: %d %u %x %c %s\n", -42, 42u, 0xbeefu, 'Q', "str");

    write_file_at((const uint8_t *)"/mnt/fat/TCCT.TXT", (const uint8_t *)out, 0, out_len);
    return passed == total ? 0 : 1;
}
