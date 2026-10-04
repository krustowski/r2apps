//go:build !r2
#include <stdlib.h>
void *r2v_malloc(size_t n){return malloc(n);}
void *r2v_calloc(size_t n,size_t size){return calloc(n,size);}
void *r2v_realloc(void *p,size_t n){return realloc(p,n);}
void r2v_free(void *p){free(p);}
