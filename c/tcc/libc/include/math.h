#ifndef _MATH_H
#define _MATH_H

/* Only what a compiler's constant folding needs so far. */

#ifdef __TINYC__
#define HUGE_VAL 1e500
#define HUGE_VALF 1e50f
#define HUGE_VALL 1e5000L
/* x86 makes 0/0 the negative "indefinite" NaN; C programs expect +nan */
#define NAN (-(0.0f / 0.0f))
#else
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#define NAN __builtin_nanf("")
#endif
#define INFINITY HUGE_VALF

double ldexp(double x, int exp);
float ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);
double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);

#endif
