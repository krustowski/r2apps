/*
 *  h264.c --- web/h264.h over h264bsd.
 *
 *  Built like the rest of the C side (see the Makefile), with h264_r2.h
 *  first on r2 so that the decoder's malloc and free are the picture
 *  allocator's: its reference pictures are a few megabytes, which belong on
 *  the user heap and not in the arena every window shares.
 */
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "h264.h"

void *web_h264_open(void)
{
    storage_t *s = h264bsdAlloc();
    if (!s)
        return 0;
    /*  No reordering: Baseline has no B slices, so a picture is shown in the
     *  order it is decoded and can come out as soon as it is. */
    if (h264bsdInit(s, 1) != HANTRO_OK)
    {
        h264bsdShutdown(s);
        h264bsdFree(s);
        return 0;
    }
    return s;
}

void web_h264_close(void *d)
{
    if (!d)
        return;
    h264bsdShutdown((storage_t *)d);
    h264bsdFree((storage_t *)d);
}

static void output(storage_t *s, web_h264_pic_fn fn, void *ctx)
{
    u32 picId, isIdr, errMbs, crop, left, top, w, h;
    u8 *data;
    while ((data = h264bsdNextOutputPicture(s, &picId, &isIdr, &errMbs)) != 0)
    {
        u32 fw = h264bsdPicWidth(s) * 16, fh = h264bsdPicHeight(s) * 16;
        struct web_h264_pic p;
        h264bsdCroppingParams(s, &crop, &left, &w, &top, &h);
        if (!crop)
        {
            left = top = 0;
            w = fw;
            h = fh;
        }
        p.stride = (int)fw;
        p.cstride = (int)fw / 2;
        p.y = data + top * fw + left;
        p.cb = data + fw * fh + (top / 2) * (fw / 2) + left / 2;
        p.cr = data + fw * fh + (fw / 2) * (fh / 2) + (top / 2) * (fw / 2) + left / 2;
        p.w = (int)w;
        p.h = (int)h;
        p.fullRange = (int)h264bsdVideoRange(s);
        fn(ctx, &p);
    }
}

int web_h264_nal(void *d, unsigned char *nal, unsigned len, web_h264_pic_fn fn, void *ctx)
{
    storage_t *s = (storage_t *)d;
    /*  A NAL unit that ends a picture is not read in the call that says so:
     *  it is given again (the same pointer, which h264bsd knows not to
     *  extract a second time) once the picture is out. */
    while (len)
    {
        u32 read = 0;
        u32 r = h264bsdDecode(s, nal, len, 0, &read);
        if (r == H264BSD_MEMALLOC_ERROR)
            return -1;
        if (r == H264BSD_PIC_RDY)
            output(s, fn, ctx);
        if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR)
            return 0;
        if (read > len)
            read = len;
        /*  Nothing taken is also how h264bsd asks for the NAL unit again:
         *  after a picture (PIC_RDY), and after the first slice has put the
         *  parameter sets into effect (HDRS_RDY).  Anything else is stuck. */
        if (!read && r != H264BSD_PIC_RDY && r != H264BSD_HDRS_RDY)
            return 0;
        nal += read;
        len -= read;
    }
    return 0;
}

void web_h264_flush(void *d, web_h264_pic_fn fn, void *ctx)
{
    storage_t *s = (storage_t *)d;
    h264bsdFlushBuffer(s);
    output(s, fn, ctx);
}
