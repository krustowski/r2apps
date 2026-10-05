//go:build r2
#include <stddef.h>
extern unsigned long r2_syscall(unsigned long,unsigned long,unsigned long);
static void *r2v_raw_alloc(size_t n){return (void *)r2_syscall(0x0a,n,0);}
static void r2v_raw_free(void *p){r2_syscall(0x0f,(unsigned long)p,0);}
#include "alloc.h"
