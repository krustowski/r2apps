/*
 *  media.c --- see media.h.  stb_image is configured as in Memento's
 *  web/stb_image.c: PNG, JPEG, GIF and BMP, no SIMD, no floating point.
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

#include "h264.h"
#include "media.h"

int tg_picture_info(const unsigned char *data, int len, int *w, int *h)
{
   int comp = 0;
   *w = *h = 0;
   return data && len > 0 && stbi_info_from_memory(data, len, w, h, &comp) && *w > 0 && *h > 0;
}

unsigned char *tg_picture_rgba(const unsigned char *data, int len, int *w, int *h)
{
   int comp = 0;
   return stbi_load_from_memory(data, len, w, h, &comp, 4);
}

void tg_free(void *p) { web_img_free(p); }

const char *tg_failure(void)
{
   const char *why = stbi_failure_reason();
   return why ? why : "";
}

/*  Memento's web_gif_frames, turned inside out: the state of stb's GIF loop
 *  between calls.  "Restore to previous" needs the frame two back, which
 *  stb expects its caller to keep: back[n & 1] holds frame n - 2. */
struct tg_gif
{
   stbi__context s;
   stbi__gif g;
   stbi_uc *data, *back[2];
   int n, done;
};

void *tg_gif_open(const unsigned char *data, int len)
{
   struct tg_gif *it;
   if (!data || len <= 0)
      return 0;
   it = (struct tg_gif *)web_img_alloc(sizeof(*it));
   if (!it)
      return 0;
   memset(it, 0, sizeof(*it));
   it->data = (stbi_uc *)web_img_alloc((unsigned long)len);
   if (!it->data)
   {
      web_img_free(it);
      return 0;
   }
   memcpy(it->data, data, (size_t)len);
   stbi__start_mem(&it->s, it->data, len);
   if (!stbi__gif_test(&it->s))
   {
      tg_gif_close(it);
      return 0;
   }
   return it;
}

const unsigned char *tg_gif_next(void *p, int *w, int *h, int *delay_ms)
{
   struct tg_gif *it = (struct tg_gif *)p;
   int comp = 0;
   stbi_uc *u;
   if (!it || it->done)
      return 0;
   /*  The frame before this one is kept before stb overwrites it. */
   if (it->n >= 1)
   {
      size_t size = (size_t)it->g.w * it->g.h * 4;
      int slot = (it->n - 1) & 1;
      if (!it->back[slot])
         it->back[slot] = (stbi_uc *)STBI_MALLOC(size);
      if (it->back[slot])
         memcpy(it->back[slot], it->g.out, size);
   }
   u = stbi__gif_load_next(&it->s, &it->g, &comp, 4, it->n >= 2 ? it->back[it->n & 1] : 0);
   if (!u || u == (stbi_uc *)&it->s)
   {
      it->done = 1; /* broken, or the end */
      return 0;
   }
   it->n++;
   *w = it->g.w;
   *h = it->g.h;
   *delay_ms = it->g.delay;
   return u;
}

void tg_gif_close(void *p)
{
   struct tg_gif *it = (struct tg_gif *)p;
   if (!it)
      return;
   STBI_FREE(it->g.out);
   STBI_FREE(it->g.history);
   STBI_FREE(it->g.background);
   STBI_FREE(it->back[0]);
   STBI_FREE(it->back[1]);
   web_img_free(it->data);
   web_img_free(it);
}

/*  ── H.264 ──────────────────────────────────────────────────────────── */

struct tg_h264
{
   void *decoder;
   unsigned char *file;
   unsigned long len;
   unsigned char *rgba;
   int w, h, pictures, failed;
};

void *tg_h264_open(const unsigned char *file, unsigned long len)
{
   struct tg_h264 *t;
   if (!file || !len)
      return 0;
   t = (struct tg_h264 *)web_img_alloc(sizeof(*t));
   if (!t)
      return 0;
   memset(t, 0, sizeof(*t));
   t->file = (unsigned char *)web_img_alloc(len);
   t->decoder = web_h264_open();
   if (!t->file || !t->decoder)
   {
      tg_h264_close(t);
      return 0;
   }
   memcpy(t->file, file, len);
   t->len = len;
   return t;
}

static unsigned char clamp(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/*  A decoded picture made RGB (BT.601, as H.264 is unless it says
 *  otherwise), as Memento's Mp4Animation::picture. */
static void onPicture(void *ctx, const struct web_h264_pic *pic)
{
   struct tg_h264 *t = (struct tg_h264 *)ctx;
   int x, y;
   if (t->failed || pic->w <= 0 || pic->h <= 0)
      return;
   if (!t->rgba || t->w != pic->w || t->h != pic->h)
   {
      web_img_free(t->rgba);
      t->rgba = (unsigned char *)web_img_alloc((unsigned long)pic->w * pic->h * 4);
      t->w = pic->w;
      t->h = pic->h;
      if (!t->rgba)
      {
         t->failed = 1;
         return;
      }
   }
   for (y = 0; y < pic->h; y++)
   {
      const unsigned char *py = pic->y + (size_t)y * pic->stride;
      const unsigned char *pu = pic->cb + (size_t)(y / 2) * pic->cstride;
      const unsigned char *pv = pic->cr + (size_t)(y / 2) * pic->cstride;
      unsigned char *o = t->rgba + (size_t)y * pic->w * 4;
      for (x = 0; x < pic->w; x++, o += 4)
      {
         int Y = py[x], D = pu[x / 2] - 128, E = pv[x / 2] - 128;
         if (pic->fullRange)
         {
            o[0] = clamp(Y + ((359 * E + 128) >> 8));
            o[1] = clamp(Y - ((88 * D + 183 * E + 128) >> 8));
            o[2] = clamp(Y + ((454 * D + 128) >> 8));
         }
         else
         {
            int C = 298 * (Y - 16) + 128;
            o[0] = clamp((C + 409 * E) >> 8);
            o[1] = clamp((C - 100 * D - 208 * E) >> 8);
            o[2] = clamp((C + 516 * D) >> 8);
         }
         o[3] = 255;
      }
   }
   t->pictures++;
}

int tg_h264_feed(void *p, unsigned long off, unsigned long len)
{
   struct tg_h264 *t = (struct tg_h264 *)p;
   int before;
   if (!t || t->failed || off > t->len || len > t->len - off || !len)
      return t && t->failed ? -1 : 0;
   before = t->pictures;
   if (web_h264_nal(t->decoder, t->file + off, (unsigned)len, onPicture, t) < 0)
      t->failed = 1;
   return t->failed ? -1 : t->pictures - before;
}

int tg_h264_flush(void *p)
{
   struct tg_h264 *t = (struct tg_h264 *)p;
   int before;
   if (!t || t->failed)
      return -1;
   before = t->pictures;
   web_h264_flush(t->decoder, onPicture, t);
   return t->failed ? -1 : t->pictures - before;
}

const unsigned char *tg_h264_rgba(void *p, int *w, int *h)
{
   struct tg_h264 *t = (struct tg_h264 *)p;
   *w = t ? t->w : 0;
   *h = t ? t->h : 0;
   return t ? t->rgba : 0;
}

const unsigned char *tg_h264_file(void *p, unsigned long *len)
{
   struct tg_h264 *t = (struct tg_h264 *)p;
   *len = t ? t->len : 0;
   return t ? t->file : 0;
}

void tg_h264_close(void *p)
{
   struct tg_h264 *t = (struct tg_h264 *)p;
   if (!t)
      return;
   web_h264_close(t->decoder);
   web_img_free(t->rgba);
   web_img_free(t->file);
   web_img_free(t);
}
