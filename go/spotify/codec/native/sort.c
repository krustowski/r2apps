#include <stddef.h>
/* Integer-only in-place heapsort: decoding must not depend on the host libc. */
static void swap(unsigned char *a,unsigned char *b,size_t width) {
 for(size_t i=0;i<width;i++){unsigned char v=a[i];a[i]=b[i];b[i]=v;}
}
static void sift(unsigned char *data,size_t count,size_t root,size_t width,int (*compare)(const void *,const void *)) {
 while(root<count/2){
  size_t child=root*2+1;
  if(child+1<count&&compare(data+child*width,data+(child+1)*width)<0)child++;
  if(compare(data+root*width,data+child*width)>=0)return;
  swap(data+root*width,data+child*width,width);root=child;
 }
}
void r2v_qsort(void *data,size_t count,size_t width,int (*compare)(const void *,const void *)) {
 if(count<2||!width||count>(size_t)-1/width)return;
 unsigned char *p=data;
 for(size_t i=count/2;i>0;i--)sift(p,count,i-1,width,compare);
 for(size_t n=count-1;n>0;n--){swap(p,p+n*width,width);sift(p,n,0,width,compare);}
}
