#pragma once
/*
 *  h264.h --- a few calls into h264bsd (third_party/h264bsd), the H.264
 *  decoder under web/mp4.cpp.  C, so that the decoder's own headers stay on
 *  the C side (h264.c) with the freestanding libc they are built against.
 *
 *  h264bsd decodes the Baseline profile: no CABAC, no B slices, no
 *  interlace.  What else comes in is an error, and mp4.cpp does not send it.
 */
#ifdef __cplusplus
extern "C" {
#endif

/*  A picture as the decoder has it, 4:2:0, cut to what the stream says is
 *  shown.  Valid until the next call. */
struct web_h264_pic
{
    const unsigned char *y, *cb, *cr;
    int stride, cstride; /* bytes from a row to the next, luma and chroma */
    int w, h;
    int fullRange;       /* 0..255 rather than 16..235 */
};

typedef void (*web_h264_pic_fn)(void *ctx, const struct web_h264_pic *pic);

/*  nullptr when there is no memory for a decoder. */
void *web_h264_open(void);
void web_h264_close(void *d);

/*  One NAL unit, without a start code or length in front of it; it is
 *  changed in place (the emulation prevention bytes come out).  Each picture
 *  it finishes goes to fn.  -1 when the decoder cannot go on (no memory);
 *  a NAL unit it cannot read is skipped, as a player would. */
int web_h264_nal(void *d, unsigned char *nal, unsigned len, web_h264_pic_fn fn, void *ctx);

/*  After the last NAL unit: the pictures still held back. */
void web_h264_flush(void *d, web_h264_pic_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
