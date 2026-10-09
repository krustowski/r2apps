//go:build !r2
#include <stdlib.h>
static void *tg_raw_alloc(size_t n) { return malloc(n); }
static void tg_raw_free(void *p) { free(p); }
#include "alloc.h"
