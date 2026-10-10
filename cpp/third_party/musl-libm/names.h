/*
 *  names.h --- every symbol of this libm subset, renamed to jsr2m_*.
 *
 *  The build force-includes it into each musl source, and libjsr2's <math.h>
 *  includes it, so QuickJS calls these and libc++r2 keeps its own sin, cos,
 *  floor, ... for the rest of the program.  Generated from `nm` over the
 *  subset; see ../README-r2.md.
 */
#ifndef _MUSL_LIBM_NAMES_H
#define _MUSL_LIBM_NAMES_H
#ifndef hidden
#define hidden __attribute__((__visibility__("hidden")))
#endif
#define  jsr2m_
#define acos jsr2m_acos
#define acosh jsr2m_acosh
#define asin jsr2m_asin
#define asinh jsr2m_asinh
#define atan jsr2m_atan
#define atan2 jsr2m_atan2
#define atanh jsr2m_atanh
#define cbrt jsr2m_cbrt
#define ceil jsr2m_ceil
#define __cos jsr2m___cos
#define cos jsr2m_cos
#define cosh jsr2m_cosh
#define exp jsr2m_exp
#define __exp_data jsr2m___exp_data
#define expm1 jsr2m_expm1
#define __expo2 jsr2m___expo2
#define fabs jsr2m_fabs
#define floor jsr2m_floor
#define fmax jsr2m_fmax
#define fmin jsr2m_fmin
#define fmod jsr2m_fmod
#define hypot jsr2m_hypot
#define log jsr2m_log
#define log10 jsr2m_log10
#define log1p jsr2m_log1p
#define log2 jsr2m_log2
#define __log2_data jsr2m___log2_data
#define __log_data jsr2m___log_data
#define lrint jsr2m_lrint
#define __math_divzero jsr2m___math_divzero
#define __math_invalid jsr2m___math_invalid
#define __math_oflow jsr2m___math_oflow
#define __math_uflow jsr2m___math_uflow
#define __math_xflow jsr2m___math_xflow
#define modf jsr2m_modf
#define pow jsr2m_pow
#define __pow_log_data jsr2m___pow_log_data
#define __rem_pio2 jsr2m___rem_pio2
#define __rem_pio2_large jsr2m___rem_pio2_large
#define rint jsr2m_rint
#define round jsr2m_round
#define scalbn jsr2m_scalbn
#define __sin jsr2m___sin
#define sin jsr2m_sin
#define sinh jsr2m_sinh
#define sqrt jsr2m_sqrt
#define __tan jsr2m___tan
#define tan jsr2m_tan
#define tanh jsr2m_tanh
#define trunc jsr2m_trunc
#endif
