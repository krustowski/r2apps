/*
 *  alloc.h --- web_img_alloc and friends, which stb_image and h264bsd call
 *  for their memory (media.c, and web/h264_r2.h for h264bsd), over the raw
 *  allocator the including file defines.  A 16-byte header keeps the size,
 *  for realloc.
 */
#ifndef TG_ALLOC_H
#define TG_ALLOC_H
#include <stddef.h>
#include <string.h>

void *web_img_alloc(unsigned long n)
{
    unsigned char *raw;
    if (n > 32ul * 1024 * 1024)
        return 0;
    raw = (unsigned char *)tg_raw_alloc(n + 16);
    if (!raw)
        return 0;
    *(unsigned long *)raw = n;
    return raw + 16;
}

void web_img_free(void *p)
{
    if (p)
        tg_raw_free((unsigned char *)p - 16);
}

void *web_img_realloc(void *p, unsigned long n)
{
    unsigned long old;
    void *q;
    if (!p)
        return web_img_alloc(n);
    if (!n)
    {
        web_img_free(p);
        return 0;
    }
    q = web_img_alloc(n);
    if (!q)
        return 0;
    old = *(unsigned long *)((unsigned char *)p - 16);
    memcpy(q, p, old < n ? old : n);
    web_img_free(p);
    return q;
}
#endif
