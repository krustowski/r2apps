/*
 *  libct --- a test of the C library tcc uses on r2.
 *
 *  The expected values are glibc's: the same file built and run on Linux
 *  must pass too (`make libct-host`).  Results go to <dir>/LIBCT.TXT, <dir>
 *  being argv[1] or /mnt/fat; the last line is written by an atexit handler
 *  into a stream main leaves open, so it only appears when exit() runs the
 *  handlers and then flushes.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __TINYC__
#define COMPILER "tcc"
#else
#define COMPILER "gcc"
#endif

static char log_buf[16384];
static size_t log_len;
static int passed, total;
static FILE *result;

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_len += (size_t)vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (log_len >= sizeof(log_buf))
        log_len = sizeof(log_buf) - 1;
}

static void check(const char *name, int ok)
{
    total++;
    passed += ok;
    say("%s %s\n", ok ? "ok  " : "FAIL", name);
}

static void check_str(const char *name, const char *got, const char *want)
{
    total++;
    if (!strcmp(got, want)) {
        passed++;
        say("ok   %s\n", name);
    } else {
        say("FAIL %s\n  got  [%s]\n  want [%s]\n", name, got, want);
    }
}

static void test_printf_int(void)
{
    char b[256];

    snprintf(b, sizeof(b), "%d|%5d|%-5d|%05d|%+d|% d|%x|%#x|%X|%o|%#o|%u", 42, 42, 42, 42, 42, 42,
             255, 255, 255, 8, 8, 4000000000u);
    check_str("printf int", b, "42|   42|42   |00042|+42| 42|ff|0xff|FF|10|010|4000000000");

    snprintf(b, sizeof(b), "%lld|%llu|%ld|%hhd|%hd|%zu|%.3d|%8.3d|%-8.3d|%c|%%", LLONG_MIN,
             ULLONG_MAX, -5L, 300, 70000, (size_t)7, 5, 5, 5, 'z');
    check_str("printf long", b,
              "-9223372036854775808|18446744073709551615|-5|44|4464|7|005|     005|005     |z|%");

    snprintf(b, sizeof(b), "%s|%10s|%-10s|%.2s|%*d|%-*d|%.*s|%p|%p", "abc", "abc", "abc", "abc", 4,
             7, 4, 7, 1, "xyz", (void *)0, (void *)0x1234);
    check_str("printf str", b, "abc|       abc|abc       |ab|   7|7   |x|(nil)|0x1234");

    check("snprintf truncates", snprintf(b, 5, "%s", "abcdefgh") == 8 && !strcmp(b, "abcd"));
}

static void test_printf_float(void)
{
    char b[512];

    snprintf(b, sizeof(b), "%f|%.2f|%.0f|%e|%.3e|%.2e", 3.14159265358979, 2.675, 2.7, 123456.789,
             0.000123456, 9.999);
    check_str("printf f/e", b, "3.141593|2.67|3|1.234568e+05|1.235e-04|1.00e+01");

    snprintf(b, sizeof(b), "%g|%g|%g|%g|%G|%g|%.15g|%.17g", 100000.0, 1000000.0, 0.0001,
             0.00001234, 1e-10, 0.0, 0.1, 0.1);
    check_str("printf g", b, "100000|1e+06|0.0001|1.234e-05|1E-10|0|0.1|0.10000000000000001");

    snprintf(b, sizeof(b), "%10.3f|%-10.2f|%+.1f|%.10f|%.3f|%f|%Lf|%08.2f", -3.14159, 1.0, 0.05,
             1.0 / 3, 0.0005, 1e20, 1.5L, -2.5);
    check_str("printf f width", b,
              "    -3.142|1.00      |+0.1|0.3333333333|0.001|100000000000000000000.000000|"
              "1.500000|-0002.50");

    snprintf(b, sizeof(b), "%f|%f|%e|%f|%g", -0.0, HUGE_VAL, -HUGE_VAL, strtod("nan", 0), 1e300);
    check_str("printf special", b, "-0.000000|inf|-inf|nan|1e+300");
}

static void test_strto(void)
{
    char *end;
    const char *s;

    s = "  -0x1fZ";
    check("strtol hex", strtol(s, &end, 0) == -31 && *end == 'Z');
    check("strtol octal", strtol("077", 0, 0) == 63);
    check("strtol base 36", strtol("z", 0, 36) == 35);
    s = "abc";
    check("strtol none", strtol(s, &end, 10) == 0 && end == s);
    errno = 0;
    check("strtol range", strtol("99999999999999999999", 0, 10) == LONG_MAX && errno == ERANGE);
    errno = 0;
    check("strtoll min", strtoll("-9223372036854775808", 0, 10) == LLONG_MIN && errno == 0);
    check("strtoul neg", strtoul("-1", 0, 10) == ULONG_MAX);
    check("strtoull max", strtoull("18446744073709551615", 0, 10) == ULLONG_MAX);
    check("atoi", atoi(" 123abc") == 123);
}

static void test_strtod(void)
{
    static const struct {
        const char *s;
        double v;
    } cases[] = {
        {"3.14159265358979", 3.14159265358979},
        {"0.1", 0.1},
        {"1e-300", 1e-300},
        {"6.02214076e23", 6.02214076e23},
        {"0x1.8p3", 12.0},
        {"-2.5e-3", -2.5e-3},
        {"1.7976931348623157e308", 1.7976931348623157e308},
        {"2.2250738585072014e-308", 2.2250738585072014e-308},
        {"4.9e-324", 4.9e-324},
        {"123456789012345678901234567890", 123456789012345678901234567890.0},
        {"0.000001", 0.000001},
        {"299792458", 299792458.0},
    };
    char name[64], *end;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double v = strtod(cases[i].s, 0);
        snprintf(name, sizeof(name), "strtod %s", cases[i].s);
        if (memcmp(&v, &cases[i].v, sizeof(v)) == 0) {
            check(name, 1);
        } else {
            total++;
            say("FAIL %s\n  got  %.17g\n  want %.17g\n", name, v, cases[i].v);
        }
    }
    check("strtod end", strtod(" .5x", &end) == 0.5 && *end == 'x');
    errno = 0;
    check("strtod overflow", strtod("1e400", 0) == HUGE_VAL && errno == ERANGE);
    check("strtod inf", strtod("-inf", 0) == -HUGE_VAL);
    check("strtold", strtold("1.5", 0) == 1.5L);
    check("strtof", strtof("0.25", 0) == 0.25f);
}

static void test_string(void)
{
    char b[32];
    char *dup, *tok;

    strcpy(b, "abcdefgh");
    memmove(b + 2, b, 6);
    check_str("memmove up", b, "ababcdef");
    strcpy(b, "abcdefgh");
    memmove(b, b + 2, 6);
    check_str("memmove down", b, "cdefghgh");
    check("strstr", !strcmp(strstr("hello world", "o w"), "o world") && !strstr("abc", "abd"));
    strcpy(b, "a/b/c");
    check("strchr/strrchr", strchr(b, '/') == b + 1 && strrchr(b, '/') == b + 3 &&
                                !strchr(b, 'z') && strchr(b, 0) == b + 5);
    check("strpbrk/spn", !strcmp(strpbrk("abc;def", ";,"), ";def") && strspn("aab", "a") == 2 &&
                             strcspn("abc", "c") == 2);
    check("strncmp/memcmp", strncmp("abcx", "abcy", 3) == 0 && strncmp("a", "b", 1) < 0 &&
                                memcmp("\xff", "\x01", 1) > 0);
    memset(b, 'x', sizeof(b));
    strncpy(b, "ab", 5);
    check("strncpy pads", b[0] == 'a' && b[2] == 0 && b[4] == 0 && b[5] == 'x');
    dup = strdup("one,two,,three");
    tok = strtok(dup, ",");
    check("strtok", tok && !strcmp(tok, "one") && !strcmp(strtok(0, ","), "two") &&
                        !strcmp(strtok(0, ","), "three") && !strtok(0, ","));
    free(dup);
    check("strerror", !strcmp(strerror(ENOENT), "No such file or directory"));
    check("ctype", isdigit('7') && !isdigit('a') && isspace('\t') && isxdigit('F') &&
                       toupper('q') == 'Q' && tolower('Q') == 'q' && ispunct('!') && !isalpha('1'));
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static void test_sort(void)
{
    static int v[2000];
    int i, sorted = 1, key;

    srand(12345);
    for (i = 0; i < 2000; i++)
        v[i] = rand() % 100000;
    v[777] = 424242;
    qsort(v, 2000, sizeof(int), cmp_int);
    for (i = 1; i < 2000; i++)
        sorted &= v[i - 1] <= v[i];
    check("qsort", sorted);
    key = 424242;
    check("bsearch", bsearch(&key, v, 2000, sizeof(int), cmp_int) == &v[1999]);
}

static jmp_buf jb;
static int depth;

static void dive(int n)
{
    depth = n;
    if (n == 50)
        longjmp(jb, 0);
    dive(n + 1);
}

static void test_setjmp(void)
{
    volatile int local = 7;
    int r = setjmp(jb);

    if (r == 0) {
        local = 8;
        dive(0);
    }
    check("setjmp/longjmp", r == 1 && depth == 50 && local == 8);
}

static void test_heap(void)
{
    size_t n = 300000, i;
    unsigned char *a = malloc(n), *b = calloc(n, 1);
    int ok = a && b;

    for (i = 0; ok && i < n; i++)
        ok = b[i] == 0;
    if (ok) {
        memset(a, 0x5a, n);
        memcpy(b, a, n);
        ok = b[0] == 0x5a && b[n - 1] == 0x5a && b[70000] == 0x5a;
        a = realloc(a, 2 * n);
        ok = ok && a && a[n - 1] == 0x5a;
    }
    free(a);
    free(b);
    check("heap 300 KB", ok);
}

static void test_files(const char *dir)
{
    char path[128], line[64], buf[16];
    FILE *f;
    long size;
    int i, ok;

    snprintf(path, sizeof(path), "%s/LIBCT.DAT", dir);
    remove(path);
    f = fopen(path, "w");
    if (!f) {
        check("fopen w", 0);
        return;
    }
    fprintf(f, "first %d\n", 1);
    fputs("second\n", f);
    for (i = 0; i < 10000; i++)
        fputc('0' + i % 10, f);
    check("fclose", fclose(f) == 0);

    f = fopen(path, "r");
    if (!f) {
        check("fopen r", 0);
        return;
    }
    check("fgets", fgets(line, sizeof(line), f) && !strcmp(line, "first 1\n"));
    check("fgetc/ungetc", fgetc(f) == 's' && ungetc('s', f) == 's' && fgetc(f) == 's');
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    check("ftell end", size == 8 + 7 + 10000);
    fseek(f, 15 + 1234, SEEK_SET);
    check("fseek/fread", fread(buf, 1, 5, f) == 5 && !memcmp(buf, "45678", 5) &&
                             ftell(f) == 15 + 1239);
    fseek(f, -3, SEEK_END);
    ok = fread(buf, 1, 10, f) == 3 && feof(f);
    check("fread at end", ok);
    fclose(f);

    f = fopen(path, "a");
    fputs("tail\n", f);
    fclose(f);
    f = fopen(path, "r");
    fseek(f, -5, SEEK_END);
    check("append", f && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "tail\n", 5));
    fclose(f);

    f = fopen(path, "w");
    fputs("new\n", f);
    fclose(f);
    f = fopen(path, "r");
    fseek(f, 0, SEEK_END);
    check("w truncates", ftell(f) == 4);
    fclose(f);

    check("remove", remove(path) == 0);
    errno = 0;
    check("fopen missing", fopen(path, "r") == 0 && errno == ENOENT);
}

static void test_time(void)
{
    time_t t = 0;
    struct tm *tm = gmtime(&t);
    struct tm copy;

    check("gmtime 0", tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1 &&
                          tm->tm_wday == 4 && tm->tm_hour == 0);
    t = 1700000000;
    tm = gmtime(&t);
    check("gmtime 1.7e9", tm->tm_year == 123 && tm->tm_mon == 10 && tm->tm_mday == 14 &&
                              tm->tm_hour == 22 && tm->tm_min == 13 && tm->tm_sec == 20 &&
                              tm->tm_wday == 2 && tm->tm_yday == 317);
    copy = *tm;
    check("mktime", mktime(&copy) == 1700000000);
    check("time now", time(0) > 1700000000);
}

static void at_exit(void)
{
    /* result is still open: exit() has to flush it after this */
    fprintf(result, "atexit ok\n");
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/mnt/fat";
    char path[128];

    say("libct built by %s\n", COMPILER);
    test_printf_int();
    test_printf_float();
    test_strto();
    test_strtod();
    test_string();
    test_sort();
    test_setjmp();
    test_heap();
    test_files(dir);
    test_time();
    say("passed %d/%d\n", passed, total);

    fputs(log_buf, stdout);
    snprintf(path, sizeof(path), "%s/LIBCT.TXT", dir);
    remove(path);
    result = fopen(path, "w");
    if (!result) {
        printf("cannot write %s\n", path);
        return 2;
    }
    fputs(log_buf, result);
    atexit(at_exit);
    return passed == total ? 0 : 1;
}
