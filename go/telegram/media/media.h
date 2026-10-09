/*
 *  media.h --- the C side of package media: stb_image for pictures and every
 *  frame of a GIF, h264bsd for the MP4s Telegram makes of GIFs.  The same
 *  decoders, built the same way, as Memento's web engine (web/stb_image.c,
 *  web/h264.c).  Nothing here calls back into Go.
 *
 *  Memory comes from web_img_alloc and friends, which the Go package supplies
 *  (alloc_r2.c: the kernel's user heap; alloc_host.c: malloc).
 */
#ifndef TG_MEDIA_H
#define TG_MEDIA_H

/*  A picture's size from its header: 0 when it is not one stb reads. */
int tg_picture_info(const unsigned char *data, int len, int *w, int *h);

/*  The picture as RGBA, freed with tg_free; 0 when it could not be read. */
unsigned char *tg_picture_rgba(const unsigned char *data, int len, int *w, int *h);
void tg_free(void *p);

/*  Why the last decode failed, in stb's words ("outofmem", ...). */
const char *tg_failure(void);

/*  The frames of a GIF, one at a time, each the whole picture as RGBA with
 *  what came before disposed of as the file says.  open copies the file. */
void *tg_gif_open(const unsigned char *data, int len);
const unsigned char *tg_gif_next(void *gif, int *w, int *h, int *delay_ms);
void tg_gif_close(void *gif);

/*  H.264 (Baseline) a NAL unit at a time out of a copy of the file.  feed
 *  gives the NAL unit at [off, off + len) and answers how many pictures it
 *  finished (-1: no memory); the last of them is in rgba until the next call.
 *  flush ends the stream the same way. */
void *tg_h264_open(const unsigned char *file, unsigned long len);
int tg_h264_feed(void *h, unsigned long off, unsigned long len);
int tg_h264_flush(void *h);
const unsigned char *tg_h264_rgba(void *h, int *w, int *h_out);
const unsigned char *tg_h264_file(void *h, unsigned long *len);
void tg_h264_close(void *h);

#endif
