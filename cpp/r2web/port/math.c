/* Small scalar helpers for the libm functions absent from libc++r2.
 * log/exp use range reduction; no host library or x87 state is required. */
#include <stdint.h>
#include <math.h>
typedef union { double d; uint64_t u; } Bits;

double r2js_log(double x)
{
    if (x != x || x < 0) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;
    int adjust = 0;
    if (x < 0x1p-1022) { x *= 0x1p52; adjust = -52; }
    Bits b = {x};
    int e = (int)(b.u >> 52) - 1023 + adjust;
    b.u = (b.u & UINT64_C(0xfffffffffffff)) | UINT64_C(0x3ff0000000000000);
    double m = b.d;
    if (m > 1.4142135623730951) { m *= 0.5; ++e; }
    double z = (m - 1) / (m + 1), z2 = z*z, term = z, sum = z;
    for (int i = 3; i <= 39; i += 2) { term *= z2; sum += term / i; }
    return 2*sum + e*0.69314718055994530942;
}
double r2js_exp(double x)
{
    if (x != x) return x;
    if (x > 709.782712893384) return INFINITY;
    if (x < -745.133219101941) return 0;
    int k = (int)floor(x*1.4426950408889634 + 0.5);
    double r = (x - k*0.6931471803691238) - k*1.9082149292705877e-10;
    double term = 1, sum = 1;
    for (int i = 1; i <= 18; ++i) { term *= r/i; sum += term; }
    if (k > 1023) { sum *= 2; --k; }
    if (k < -1022) {
        Bits scale = {.u = (uint64_t)(k + 1023 + 52) << 52};
        return (sum*scale.d)*0x1p-52;
    }
    Bits scale = {.u = (uint64_t)(k + 1023) << 52};
    return sum*scale.d;
}
double r2js_fmod(double x, double y)
{
    if (isnan(x) || isnan(y) || isinf(x) || y == 0) return NAN;
    if (isinf(y)) return x;
    double a = fabs(x), b = fabs(y);
    if (a < b) return x;
    double step = b;
    while (step <= a*0.5) step *= 2;
    while (step >= b) { if (a >= step) a -= step; step *= 0.5; }
    return signbit(x) ? -a : a;
}
double r2js_pow(double x, double y)
{
    if (y == 0) return 1;
    if (isnan(x) || isnan(y)) return NAN;
    double ax = fabs(x);
    if (isinf(y)) {
        if (ax == 1) return NAN;
        return (ax > 1) == (y > 0) ? INFINITY : 0;
    }
    int odd = isfinite(y) && r2js_fmod(fabs(y), 2) == 1;
    if (x == 0 || isinf(x)) {
        double r = (x == 0) == (y > 0) ? 0 : INFINITY;
        return signbit(x) && odd ? -r : r;
    }
    if (x < 0 && trunc(y) != y) return NAN;
    if (trunc(y) == y && fabs(y) <= 2147483647) {
        unsigned n = (unsigned)fabs(y);
        double result = 1, base = y < 0 ? 1/x : x;
        while (n) { if (n & 1) result *= base; base *= base; n >>= 1; }
        return result;
    }
    double r = r2js_exp(y*r2js_log(ax));
    return x < 0 && odd ? -r : r;
}
double r2js_asin(double x) { return atan2(x, sqrt((1-x)*(1+x))); }
double r2js_acos(double x) { return atan2(sqrt((1-x)*(1+x)), x); }
