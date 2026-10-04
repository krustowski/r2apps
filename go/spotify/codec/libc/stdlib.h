#ifndef R2_VORBIS_STDLIB_H
#define R2_VORBIS_STDLIB_H
#include <stddef.h>
void *r2v_malloc(size_t);
void *r2v_calloc(size_t,size_t);
void *r2v_realloc(void *,size_t);
void r2v_free(void *);
void r2v_qsort(void *,size_t,size_t,int (*)(const void *,const void *));
#define qsort r2v_qsort
#define malloc r2v_malloc
#define calloc r2v_calloc
#define realloc r2v_realloc
#define free r2v_free
#define alloca __builtin_alloca
static inline int abs(int n){return n<0?-n:n;}
static inline long labs(long n){return n<0?-n:n;}
#endif
