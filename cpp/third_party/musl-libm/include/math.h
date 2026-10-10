/*
 *  math.h --- the part of musl's <math.h> that this libm subset is built
 *  against (see ../README-r2.md).  Every name is renamed to jsr2m_* by
 *  ../names.h, which the build force-includes, so these functions never
 *  collide with libc++r2's own scalar sin/cos/floor.
 */
#ifndef _MUSL_LIBM_MATH_H
#define _MUSL_LIBM_MATH_H

#include "../names.h"

typedef float float_t;
typedef double double_t;

#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VALF INFINITY
#define HUGE_VAL ((double)INFINITY)
#define HUGE_VALL ((long double)INFINITY)

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
#define isnormal(x) __builtin_isnormal(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

#ifdef __cplusplus
extern "C" {
#endif
double acos(double);
double acosh(double);
double asin(double);
double asinh(double);
double atan(double);
double atan2(double, double);
double atanh(double);
double cbrt(double);
double ceil(double);
double cos(double);
double cosh(double);
double exp(double);
double expm1(double);
double fabs(double);
double floor(double);
double fmax(double, double);
double fmin(double, double);
double fmod(double, double);
double hypot(double, double);
double log(double);
double log10(double);
double log1p(double);
double log2(double);
long lrint(double);
double modf(double, double *);
double pow(double, double);
double rint(double);
double round(double);
double scalbn(double, int);
double sin(double);
double sinh(double);
double sqrt(double);
double tan(double);
double tanh(double);
double trunc(double);
#ifdef __cplusplus
}
#endif

#endif
