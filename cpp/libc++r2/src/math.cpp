/*
 *  math.cpp — the floating-point functions, since there is no libm.
 *
 *  sin and cos reduce the argument to a quadrant and then evaluate a Taylor
 *  series over [0, pi/2], which is accurate to about 1e-9 there.  atan uses a
 *  polynomial good to about 1e-5 --- fine for rotating a cube or pointing a
 *  vector at the mouse, not for numerical work.
 */

#include "r2/math.hpp"

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 6.28318530717958647692;
constexpr double HALF_PI = 1.57079632679489661923;

/*  sin over [0, pi/2] --- Taylor to x^13.  */
double sin_series(double x) {
    double x2 = x * x;
    double term = x;
    double sum = x;

    term *= -x2 / (2 * 3);
    sum += term;
    term *= -x2 / (4 * 5);
    sum += term;
    term *= -x2 / (6 * 7);
    sum += term;
    term *= -x2 / (8 * 9);
    sum += term;
    term *= -x2 / (10 * 11);
    sum += term;
    term *= -x2 / (12 * 13);
    sum += term;

    return sum;
}

/*  cos over [0, pi/2] --- Taylor to x^12.  */
double cos_series(double x) {
    double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;

    term *= -x2 / (1 * 2);
    sum += term;
    term *= -x2 / (3 * 4);
    sum += term;
    term *= -x2 / (5 * 6);
    sum += term;
    term *= -x2 / (7 * 8);
    sum += term;
    term *= -x2 / (9 * 10);
    sum += term;
    term *= -x2 / (11 * 12);
    sum += term;

    return sum;
}

} // namespace

extern "C" {

double sqrt(double v) {
    double out;
    asm("sqrtsd %1, %0" : "=x"(out) : "x"(v));
    return out;
}

float sqrtf(float v) {
    float out;
    asm("sqrtss %1, %0" : "=x"(out) : "x"(v));
    return out;
}

double fabs(double v) { return __builtin_fabs(v); }

double trunc(double v) {
    /*  Anything this large is already integral, and would overflow int64.  */
    if (v > 9.2e18 || v < -9.2e18 || v != v)
        return v;
    return (double)(int64_t)v;
}

double floor(double v) {
    double t = trunc(v);
    return (v < 0.0 && t != v) ? t - 1.0 : t;
}

double ceil(double v) {
    double t = trunc(v);
    return (v > 0.0 && t != v) ? t + 1.0 : t;
}

double round(double v) { return v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5); }

double fmod(double a, double b) {
    if (b == 0.0 || a != a || b != b)
        return 0.0;
    double quotient = trunc(a / b);
    return a - quotient * b;
}

double sin(double radians) {
    if (radians != radians)
        return radians;

    double x = fmod(radians, TAU);
    if (x < 0.0)
        x += TAU;

    int quadrant = (int)(x / HALF_PI);
    if (quadrant > 3)
        quadrant = 3;
    double r = x - quadrant * HALF_PI;

    switch (quadrant) {
    case 0:
        return sin_series(r);
    case 1:
        return cos_series(r);
    case 2:
        return -sin_series(r);
    default:
        return -cos_series(r);
    }
}

double cos(double radians) { return sin(radians + HALF_PI); }

double tan(double radians) {
    double c = cos(radians);
    if (c == 0.0)
        return 1.0e308; /*  no infinity to hand back without <limits>  */
    return sin(radians) / c;
}

double atan(double v) {
    bool negative = v < 0.0;
    if (negative)
        v = -v;

    /*  atan(v) = pi/2 - atan(1/v) keeps the polynomial inside [0, 1].  */
    bool inverted = v > 1.0;
    if (inverted)
        v = 1.0 / v;

    double v2 = v * v;
    double result =
        v * (0.99986600 +
             v2 * (-0.33029505 + v2 * (0.18014100 + v2 * (-0.08513300 + v2 * 0.02083510))));

    if (inverted)
        result = HALF_PI - result;
    return negative ? -result : result;
}

double atan2(double y, double x) {
    if (x > 0.0)
        return atan(y / x);
    if (x < 0.0)
        return y >= 0.0 ? atan(y / x) + PI : atan(y / x) - PI;
    if (y > 0.0)
        return HALF_PI;
    if (y < 0.0)
        return -HALF_PI;
    return 0.0;
}

double pow_int(double base, int exponent) {
    bool negative_exponent = exponent < 0;
    unsigned n = negative_exponent ? (unsigned)(-exponent) : (unsigned)exponent;

    double result = 1.0;
    while (n) {
        if (n & 1)
            result *= base;
        base *= base;
        n >>= 1;
    }

    return negative_exponent ? 1.0 / result : result;
}

} // extern "C"

namespace r2 {

uint32_t isqrt(uint64_t value) {
    if (value == 0)
        return 0;

    /*  Bit-by-bit: exact, and no rounding to argue with.  */
    uint64_t remainder = value;
    uint64_t result = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > remainder)
        bit >>= 2;

    while (bit != 0) {
        if (remainder >= result + bit) {
            remainder -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

} // namespace r2
