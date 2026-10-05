#ifndef R2V_ALLOC_H
#define R2V_ALLOC_H
#include <stddef.h>
#include <string.h>
static size_t r2v_live_bytes, r2v_peak_bytes;
void *r2v_malloc(size_t n) {
 if(n>16*1024*1024)return 0;
 unsigned char *raw=r2v_raw_alloc(n+16);if(!raw)return 0;
 *(size_t *)raw=n;
 r2v_live_bytes+=n;
 if(r2v_live_bytes>r2v_peak_bytes)r2v_peak_bytes=r2v_live_bytes;
 return raw+16;
}
void r2v_free(void *p) {
 if(p){unsigned char *raw=(unsigned char *)p-16;r2v_live_bytes-=*(size_t *)raw;r2v_raw_free(raw);}
}
void *r2v_calloc(size_t n,size_t size) {
 if(size&&n>(16*1024*1024)/size)return 0;
 size_t total=n*size;void *p=r2v_malloc(total);if(p)memset(p,0,total);return p;
}
void *r2v_realloc(void *p,size_t n) {
 if(!p)return r2v_malloc(n);if(!n){r2v_free(p);return 0;}
 void *q=r2v_malloc(n);if(!q)return 0;
 size_t old=*(size_t *)((unsigned char *)p-16);memcpy(q,p,old<n?old:n);r2v_free(p);return q;
}
void r2v_memory(size_t *used,size_t *peak){*used=r2v_live_bytes;*peak=r2v_peak_bytes;}
#endif
