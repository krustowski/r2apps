#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../vendor/tremor/codebook.h"

static int live, calls, fail_at=-1;
void *r2v_malloc(size_t n) {
 if(calls++==fail_at)return NULL;
 void *p=malloc(n);if(p)live++;return p;
}
void *r2v_calloc(size_t n,size_t size) {
 void *p=r2v_malloc(n*size);if(p)memset(p,0,n*size);return p;
}
void r2v_free(void *p){if(p){live--;free(p);}}
void *r2v_realloc(void *p,size_t n){if(!p)return r2v_malloc(n);return realloc(p,n);}
static void *decode(void *unused) {
 (void)unused;
 const int count=16384;
 long *lengths=malloc(count*sizeof(*lengths));
 for(int i=0;i<count;i++)lengths[i]=14;
 static_codebook book={0};book.entries=count;book.dim=1;book.lengthlist=lengths;
 codebook decoded;
 int result=vorbis_book_init_decode(&decoded,&book);
 if(fail_at<0&&result){fprintf(stderr,"large codebook failed\n");exit(1);}
 if(fail_at>=0&&!result){fprintf(stderr,"allocation failure ignored\n");exit(1);}
 vorbis_book_clear(&decoded);free(lengths);
 if(live){fprintf(stderr,"leaked %d allocations\n",live);exit(1);}
 return NULL;
}
static void run(void) {
 pthread_attr_t attr;pthread_attr_init(&attr);
 if(pthread_attr_setstacksize(&attr,64*1024)){exit(1);}
 pthread_t thread;if(pthread_create(&thread,&attr,decode,NULL)){exit(1);}
 pthread_join(thread,NULL);pthread_attr_destroy(&attr);
}
int main(void) {
 run();int allocations=calls;
 for(int i=0;i<allocations;i++){calls=0;fail_at=i;run();}
 puts("Large Vorbis codebook passed on 64 KiB stack; allocation failures cleaned up.");
 return 0;
}
