#pragma once
#define NAN (__builtin_nan(""))
#define INFINITY (__builtin_inf())
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double floor(double), ceil(double), trunc(double), fabs(double), sqrt(double);
double sin(double), cos(double), tan(double), atan(double), atan2(double, double);
#define acos r2js_acos
#define asin r2js_asin
#define exp r2js_exp
#define log r2js_log
#define pow r2js_pow
#define fmod r2js_fmod
double r2js_acos(double), r2js_asin(double), r2js_exp(double), r2js_log(double);
double r2js_pow(double, double), r2js_fmod(double, double);
