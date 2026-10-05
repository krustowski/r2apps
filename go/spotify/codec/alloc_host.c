//go:build !r2
#include <stdlib.h>
static void *r2v_raw_alloc(size_t n){return malloc(n);}
static void r2v_raw_free(void *p){free(p);}
#include "alloc.h"
