/* features.h --- musl internal visibility macro, for the libm subset. */
#ifndef _MUSL_LIBM_FEATURES_H
#define _MUSL_LIBM_FEATURES_H
#ifndef hidden
#define hidden __attribute__((__visibility__("hidden")))
#endif
#endif
