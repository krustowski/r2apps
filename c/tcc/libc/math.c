#include <math.h>

/* x × 2^exp, in steps a long double can hold. */
long double ldexpl(long double x, int exp)
{
    while (exp > 16000) {
        x *= 0x1p16000L;
        exp -= 16000;
    }
    while (exp < -16000) {
        x *= 0x1p-16000L;
        exp += 16000;
    }
    while (exp > 60) {
        x *= 0x1p60L;
        exp -= 60;
    }
    while (exp < -60) {
        x *= 0x1p-60L;
        exp += 60;
    }
    return exp >= 0 ? x * (long double)(1UL << exp) : x / (long double)(1UL << -exp);
}

double ldexp(double x, int exp)
{
    return (double)ldexpl(x, exp);
}

float ldexpf(float x, int exp)
{
    return (float)ldexpl(x, exp);
}

long double fabsl(long double x)
{
    return x < 0 ? -x : x;
}

double fabs(double x)
{
    return x < 0 ? -x : x;
}

float fabsf(float x)
{
    return x < 0 ? -x : x;
}
