#include "image.h"

extern "C" {
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req);
int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp);
void stbi_image_free(void *p);
const char *stbi_failure_reason(void);

//  stb_image's allocator (see stb_image.c).
void *web_img_alloc(unsigned long n) { return web::big_alloc(n); }
void *web_img_realloc(void *p, unsigned long n) { return web::big_realloc(p, n); }
void web_img_free(void *p)
{
    if (p)
        web::big_free(p);
}
}

namespace web {

void Picture::release()
{
    if (px)
        big_free(px);
    px = nullptr;
    w = h = 0;
}

namespace {

//  The 16 colours as the palette has them, and for every RGB at four bits a
//  channel the nearest of them; the Bayer offsets for 16 colours (steps of
//  85) and for the cube (steps of 51).
const uint8_t kEga[16][3] = {
    {0, 0, 0},     {0, 0, 170},    {0, 170, 0},    {0, 170, 170},  {170, 0, 0},    {170, 0, 170},
    {170, 85, 0},  {170, 170, 170}, {85, 85, 85},  {85, 85, 255},  {85, 255, 85},  {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};
uint8_t nearest[16 * 16 * 16];
int bayer[4][4], bayer6[4][4];
uint8_t q6[256];
bool tablesBuilt = false;

void buildTables()
{
    if (tablesBuilt)
        return;
    for (int r = 0; r < 16; r++)
        for (int g = 0; g < 16; g++)
            for (int b = 0; b < 16; b++)
            {
                int R = r * 17, G = g * 17, B = b * 17, best = 0, bestD = 1 << 30;
                for (int k = 0; k < 16; k++)
                {
                    int dr = R - kEga[k][0], dg = G - kEga[k][1], db = B - kEga[k][2];
                    int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db; // green counts most
                    if (d < bestD)
                        bestD = d, best = k;
                }
                nearest[(r << 8) | (g << 4) | b] = (uint8_t)best;
            }
    static const int m[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            bayer[y][x] = m[y][x] * 85 / 16 - 40;
            bayer6[y][x] = m[y][x] * 51 / 16 - 24;
        }
    for (int v = 0; v < 256; v++)
        q6[v] = (uint8_t)((v * 5 + 127) / 255);
    tablesBuilt = true;
}

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

} // namespace

bool pictureSize(const uint8_t *data, size_t len, int &w, int &h)
{
    int comp = 0;
    w = h = 0;
    if (!data || !len || len > 0x7FFFFFFF)
        return false;
    return stbi_info_from_memory(data, (int)len, &w, &h, &comp) && w > 0 && h > 0;
}

namespace {

//  The size a w x h picture is shown at in maxW x maxH: the same factor both
//  ways, never enlarged.
void fitSize(int w, int h, int maxW, int maxH, int &tw, int &th)
{
    if (maxW < 1)
        maxW = 1;
    if (maxH < 1)
        maxH = 1;
    tw = w;
    th = h;
    if (tw > maxW)
    {
        th = (int)((long long)th * maxW / tw);
        tw = maxW;
    }
    if (th > maxH)
    {
        tw = (int)((long long)tw * maxH / th);
        th = maxH;
    }
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
}

//  w x h of RGBA made tw x th palette indices in px.  The dither is by
//  position alone, so a pixel that stays the same colour from one frame of
//  an animation to the next stays the same index: nothing crawls.
void ditherInto(const unsigned char *rgba, int w, int h, int tw, int th, int colours, uint32_t bg, uint8_t *px)
{
    buildTables();
    const int bgR = (int)(bg >> 16) & 255, bgG = (int)(bg >> 8) & 255, bgB = (int)bg & 255;
    const bool cube = colours >= 256;
    for (int ty = 0; ty < th; ty++)
    {
        int sy0 = (int)((long long)ty * h / th), sy1 = (int)((long long)(ty + 1) * h / th);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        const int *dither = cube ? bayer6[ty & 3] : bayer[ty & 3];
        uint8_t *o = px + (size_t)ty * tw;
        for (int tx = 0; tx < tw; tx++)
        {
            int sx0 = (int)((long long)tx * w / tw), sx1 = (int)((long long)(tx + 1) * w / tw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            //  The average of the pixels this one stands for, each laid over
            //  the background by its alpha first.
            long long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
            {
                const unsigned char *p = rgba + ((size_t)sy * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, p += 4)
                {
                    int a = p[3];
                    sr += (p[0] * a + bgR * (255 - a)) / 255;
                    sg += (p[1] * a + bgG * (255 - a)) / 255;
                    sb += (p[2] * a + bgB * (255 - a)) / 255;
                    n++;
                }
            }
            int d = dither[tx & 3];
            int r = clamp255((int)(sr / n) + d), g = clamp255((int)(sg / n) + d), b = clamp255((int)(sb / n) + d);
            o[tx] = cube ? (uint8_t)(16 + 36 * q6[r] + 6 * q6[g] + q6[b])
                         : nearest[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
        }
    }
}

} // namespace

const char *decodePicture(const uint8_t *data, size_t len, int maxW, int maxH, int colours, uint32_t bg,
                          Picture &out)
{
    out.release();
    int w, h;
    if (!pictureSize(data, len, w, h))
        return "not a picture this browser reads (PNG, JPEG, GIF, BMP)";
    //  Six million pixels is 24 MiB decoded, before it is made smaller.
    if ((long long)w * h > 6LL * 1000 * 1000)
        return "too big to decode";
    int tw, th;
    fitSize(w, h, maxW, maxH, tw, th);

    uint8_t *px = (uint8_t *)big_alloc((size_t)tw * th);
    if (!px)
        return "out of memory";
    int comp = 0;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba)
    {
        big_free(px);
        const char *why = stbi_failure_reason();
        return why && !strcmp(why, "outofmem") ? "out of memory" : "the picture could not be read";
    }
    ditherInto(rgba, w, h, tw, th, colours, bg, px);
    stbi_image_free(rgba);
    out.w = tw;
    out.h = th;
    out.px = px;
    return nullptr;
}

// ── Animations ───────────────────────────────────────────────────────────────

void Animation::release()
{
    if (px)
        big_free(px);
    if (delay)
        big_free(delay);
    px = nullptr;
    delay = nullptr;
    w = h = frames = 0;
    length = 0;
}

int Animation::frameAt(uint64_t ms) const
{
    if (frames < 2 || !length)
        return 0;
    uint32_t t = (uint32_t)(ms % length);
    for (int i = 0; i < frames; i++)
    {
        if (t < delay[i])
            return i;
        t -= delay[i];
    }
    return frames - 1;
}

bool isGif(const uint8_t *data, size_t len)
{
    return data && len >= 6 && !memcmp(data, "GIF8", 4) && (data[4] == '7' || data[4] == '9') && data[5] == 'a';
}

AnimationBuilder::AnimationBuilder(Animation &out, int maxW, int maxH, int colours, uint32_t bg, size_t budget)
    : a(out), maxW(maxW), maxH(maxH), colours(colours), bg(bg), budget(budget)
{
    a.release();
}

namespace {

void addDelay(uint16_t &d, int ms)
{
    int v = d + ms;
    d = (uint16_t)(v > 65535 ? 65535 : v);
}

} // namespace

bool AnimationBuilder::add(const uint8_t *rgba, int w, int h, int ms)
{
    if (why_)
        return false;
    if (ms < 1)
        ms = 1;
    if (!a.px)
    {
        //  The first: how big the others will be, and room for them.
        fitSize(w, h, maxW, maxH, a.w, a.h);
        size_t fb = (size_t)a.w * a.h;
        size_t n = budget / fb;
        if (n > 1024)
            n = 1024;
        cap = (int)(n & ~(size_t)1);
        if (cap < 2)
        {
            why_ = "too big to animate";
            return false;
        }
        a.px = (uint8_t *)big_alloc(fb * cap);
        a.delay = (uint16_t *)big_alloc(sizeof(uint16_t) * cap);
        if (!a.px || !a.delay)
        {
            why_ = "out of memory";
            return false;
        }
    }
    if (seen++ % stride)
    {
        addDelay(a.delay[a.frames - 1], ms);
        return true;
    }
    if (a.frames == cap)
    {
        //  Full: every other frame goes, and the rest are kept at twice the
        //  distance.  cap is even, so this frame is one of those.
        size_t fb = (size_t)a.w * a.h;
        int n = 0;
        for (int i = 0; i < a.frames; i += 2, n++)
        {
            if (n != i)
                memcpy(a.px + fb * n, a.px + fb * i, fb);
            uint16_t d = a.delay[i];
            if (i + 1 < a.frames)
                addDelay(d, a.delay[i + 1]);
            a.delay[n] = d;
        }
        a.frames = n;
        stride *= 2;
    }
    ditherInto(rgba, w, h, a.w, a.h, colours, bg, a.px + (size_t)a.frames * a.w * a.h);
    a.delay[a.frames++] = (uint16_t)(ms > 65535 ? 65535 : ms);
    return true;
}

const char *AnimationBuilder::finish()
{
    if (!a.frames)
    {
        a.release();
        return why_ ? why_ : "no pictures in it";
    }
    //  Give back the room that was not needed.
    if (a.frames < cap)
    {
        if (void *p = big_realloc(a.px, (size_t)a.w * a.h * a.frames))
            a.px = (uint8_t *)p;
    }
    a.length = 0;
    for (int i = 0; i < a.frames; i++)
        a.length += a.delay[i];
    return nullptr;
}

namespace {

//  Frames in the file stood for by the ones kept: what browsers do with a
//  delay of 0 or 10 ms, which many GIFs have and none means, is 100.
int shownFor(int ms) { return ms <= 10 ? 100 : ms; }

int onGifFrame(void *ctx, const unsigned char *rgba, int w, int h, int delayMs)
{
    return !((AnimationBuilder *)ctx)->add(rgba, w, h, shownFor(delayMs));
}

} // namespace

extern "C" int web_gif_frames(const unsigned char *data, int len,
                              int (*fn)(void *, const unsigned char *, int, int, int), void *ctx);

const char *decodeAnimation(const uint8_t *data, size_t len, int maxW, int maxH, int colours, uint32_t bg,
                            size_t budget, Animation &out)
{
    out.release();
    int w, h;
    if (!isGif(data, len) || !pictureSize(data, len, w, h))
        return "not a GIF";
    //  While it is read, stb_image keeps the picture four times over at four
    //  bytes a pixel and once at one: a million pixels is 17 MiB.
    if ((long long)w * h > 1000 * 1000)
        return "too big to animate";
    AnimationBuilder b(out, maxW, maxH, colours, bg, budget);
    int n = web_gif_frames(data, (int)len, onGifFrame, &b);
    if (b.why() || n < 0)
    {
        out.release();
        const char *why = stbi_failure_reason();
        return b.why()          ? b.why()
               : why && !strcmp(why, "outofmem") ? "out of memory"
                                                 : "the GIF could not be read";
    }
    return b.finish();
}

} // namespace web
