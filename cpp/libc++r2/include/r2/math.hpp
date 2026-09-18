#ifndef _R2CXX_MATH_HPP_
#define _R2CXX_MATH_HPP_

/*
 *  math.hpp — the floating-point helpers a graphical program needs.
 *
 *  The kernel enables SSE and FXSR in CR4 before it schedules anything, so
 *  double arithmetic works in userland; there is just no libm to call.  sqrt
 *  is the hardware instruction, sin and cos are Taylor series over a reduced
 *  argument, and the rest are exact.
 *
 *  These keep the names and the C linkage that libm gives them, and they are
 *  deliberately NOT mirrored into namespace r2: a program that says
 *  `using namespace r2;` would then have two candidates for every call.  Write
 *  sqrt(x) and sin(x) unqualified, as you would anywhere else.
 *
 *  What is in namespace r2 is what libm has no claim on: the constants, the
 *  integer square root, and abs() as a template so it works for any numeric
 *  type rather than just int.
 */

#include "types.hpp"

extern "C" {

double sqrt(double v);
float sqrtf(float v);
double fabs(double v);

double floor(double v);
double ceil(double v);
double round(double v);
double trunc(double v);
double fmod(double a, double b);

double sin(double radians);
double cos(double radians);
double tan(double radians);
double atan(double v);
double atan2(double y, double x);

/*  Integer exponent only; there is no general pow() here.  */
double pow_int(double base, int exponent);

} // extern "C"

namespace r2 {

inline constexpr double PI = 3.14159265358979323846;
inline constexpr double TAU = 6.28318530717958647692;
inline constexpr double DEG_TO_RAD = PI / 180.0;
inline constexpr double RAD_TO_DEG = 180.0 / PI;

template <class T> constexpr T abs(T v) { return v < T(0) ? -v : v; }

/*  Exact, and no FPU involved.  */
uint32_t isqrt(uint64_t value);

/*  Linear interpolation, the one piece of arithmetic every animation needs.  */
inline constexpr double lerp(double a, double b, double t) { return a + (b - a) * t; }

} // namespace r2

#endif
