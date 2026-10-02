/*
 *  h264_r2.h --- put in front of every h264bsd file on r2 (-include): its
 *  malloc and free become the picture allocator's, the big pool (web/image.cpp).
 *  The host tests leave it out and h264bsd has the C library's.
 */
void *web_img_alloc(unsigned long n);
void web_img_free(void *p);
#define malloc(n) web_img_alloc(n)
#define free(p) web_img_free(p)
