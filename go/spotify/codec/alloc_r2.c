//go:build r2
#include <stddef.h>
#include <string.h>
extern unsigned long r2_syscall(unsigned long,unsigned long,unsigned long);
void *r2v_malloc(size_t n){if(n>16*1024*1024)return 0;unsigned long p=r2_syscall(0x0a,n+16,0);if(!p)return 0;*(size_t *)p=n;return (void *)(p+16);}
void r2v_free(void *p){if(p)r2_syscall(0x0f,(unsigned long)p-16,0);}
void *r2v_calloc(size_t n,size_t size){if(size&&n>(16*1024*1024)/size)return 0;size_t total=n*size;void *p=r2v_malloc(total);if(p)memset(p,0,total);return p;}
void *r2v_realloc(void *p,size_t n){if(!p)return r2v_malloc(n);if(!n){r2v_free(p);return 0;}void *q=r2v_malloc(n);if(!q)return 0;size_t old=*(size_t *)((char *)p-16);memcpy(q,p,old<n?old:n);r2v_free(p);return q;}
