/*
 *  stb_image, for the pictures on web pages and in Telegram (image.cpp).
 *
 *  Built like the rest of the C side (see the Makefile): freestanding and
 *  with -mgeneral-regs-only, so without its SIMD paths and without anything
 *  that needs floating point (HDR, the linear conversions).  PNG, JPEG,
 *  GIF and BMP, which is what pages use short of WebP and SVG; a GIF's other
 *  frames through web_gif_frames at the end of this file.  Memory comes from the engine's big pool, which is the kernel's user
 *  heap: a picture is the largest thing it is asked for.
 *
 *  third_party/stb/stb_image.h is stb_image v2.30 as published
 *  (github.com/nothings/stb), public domain.
 */
void *web_img_alloc(unsigned long n);
void *web_img_realloc(void *p, unsigned long n);
void web_img_free(void *p);

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(n) web_img_alloc(n)
#define STBI_REALLOC(p, n) web_img_realloc(p, n)
#define STBI_FREE(p) web_img_free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*
 *  Every frame of a GIF, one at a time, as stb_image composes them: each the
 *  whole picture in RGBA with what came before it disposed of as the file
 *  says.  stbi_load_gif_from_memory would hand over all of them at once,
 *  four bytes a pixel a frame --- tens of megabytes for a few seconds of a
 *  small animation --- where the caller keeps each one a byte a pixel and
 *  smaller.  So this walks stb's own loop and gives each frame to `fn` as
 *  it comes; `fn` returns nonzero to stop.  `delay_ms` is the file's.
 *
 *  "Restore to previous" needs the frame two back, which stb_image expects
 *  its caller to keep: two copies here, and without them (no memory) such a
 *  frame goes back to the background instead.
 *
 *  The number of frames given to `fn`; -1 when it is not a GIF or the first
 *  frame could not be read.  A file broken further on ends where it breaks.
 */
typedef int (*web_gif_frame_fn)(void *ctx, const unsigned char *rgba, int w, int h, int delay_ms);

int web_gif_frames(const unsigned char *data, int len, web_gif_frame_fn fn, void *ctx)
{
   stbi__context s;
   stbi__gif g;
   stbi_uc *back[2] = {0, 0};
   int n = 0, comp = 0;
   stbi__start_mem(&s, data, len);
   if (!stbi__gif_test(&s))
      return -1;
   memset(&g, 0, sizeof(g));
   for (;;) {
      /* Frame n - 2 is in back[n & 1]. */
      stbi_uc *u = stbi__gif_load_next(&s, &g, &comp, 4, n >= 2 ? back[n & 1] : 0);
      size_t size;
      if (!u || u == (stbi_uc *)&s)
         break; /* broken, or the end */
      if (fn(ctx, u, g.w, g.h, g.delay)) {
         n++;
         break;
      }
      size = (size_t)g.w * g.h * 4;
      if (!back[n & 1])
         back[n & 1] = (stbi_uc *)STBI_MALLOC(size);
      if (back[n & 1])
         memcpy(back[n & 1], u, size);
      n++;
   }
   STBI_FREE(g.out);
   STBI_FREE(g.history);
   STBI_FREE(g.background);
   STBI_FREE(back[0]);
   STBI_FREE(back[1]);
   return n ? n : -1;
}
