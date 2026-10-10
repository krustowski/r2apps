#ifndef JSR2_PORT_SYS_TIME_H
#define JSR2_PORT_SYS_TIME_H
#include "../time.h"
#ifdef __cplusplus
extern "C" {
#endif
#define gettimeofday jsr2_gettimeofday
struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *, void *);
#ifdef __cplusplus
}
#endif
#endif
