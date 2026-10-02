// File-level so resetWallpaperCache() can null them after r2_heap_restore.
static PlatformColor *wp_lit = nullptr;
static PlatformColor *wp_dk = nullptr;
static void resetWallpaperCache()
{
    wp_lit = nullptr;
    wp_dk = nullptr;
}

//
//  A picture as the wallpaper, over the whole screen: chosen in the file
//  manager ("Set as wallpaper" on a .PNG), kept for the rest of the process,
//  logouts included.  Without one the stars are drawn.
//
//  It is decoded once, when it is chosen, into the screen's colours
//  (web::decodePicture: stb_image, then a dither into the palette) and at no
//  more than the size that covers the screen.  Painting it is then a copy of
//  bytes: scaled to cover --- the same factor both ways, the middle of it
//  kept, what sticks out at two opposite sides cut off --- and the column
//  map built once per size, not per pixel.  It lives in web::big_alloc's
//  memory, not in a window's context, so resetWallpaperCache leaves it be.
//
#include "../web/image.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"
#include "ui/platform/impl/r2/R2_Vga.h"

static web::Picture g_wallPic;

//  Loads the picture at `path` (a full path, any mount) as the wallpaper.
//  nullptr when it worked, otherwise why not, in a few words.  On failure
//  the wallpaper there was stays.
static const char *setWallpaper(const char *path)
{
    r2::string_view sv(path, strlen(path));
    auto size = r2::fs::size_of(sv);
    if (!size || !*size)
        return "cannot read the file";
    uint8_t *file = (uint8_t *)web::big_alloc(*size);
    if (!file)
        return "out of memory";
    int64_t got = r2::fs::read_at(sv, r2::byte_span(file, *size), 0);
    if (got <= 0)
    {
        web::big_free(file);
        return "cannot read the file";
    }

    int32 sw = 640, sh = 400; // the screen, in pixels
    MementoR2Impl::R2_Vga640x400::ScreenSize(sw, sh);
    int pw, ph;
    if (!web::pictureSize(file, (size_t)got, pw, ph))
    {
        web::big_free(file);
        return "not a picture";
    }
    //  Made smaller only as far as it still covers the screen both ways.
    int maxW = pw;
    if ((long long)sw * ph >= (long long)sh * pw) // wider than the screen's shape: width decides
        maxW = sw < pw ? sw : pw;
    else if (sh < ph)
        maxW = (int)(((long long)pw * sh + ph - 1) / ph);

    web::Picture pic;
    const char *why = web::decodePicture(file, (size_t)got, maxW, ph, (int)MementoR2Impl::R2_Palette::Count(),
                                         0x000000, pic);
    web::big_free(file);
    if (why)
        return why;
    g_wallPic.release();
    g_wallPic = pic;
    return nullptr;
}

//  The picture over the whole of `target`, scaled to cover it.
static void paintWallpaperPicture(PlatformBitmap *target)
{
    auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
    uint8 *px = bm->GetPixels();
    const web::Picture &p = g_wallPic;
    int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
    if (!px || !p.px || bw <= 0 || bh <= 0 || bw > 4096)
        return;

    //  Cover: the larger of the two factors, centred.  In 1/1024ths.
    long long fx = (long long)bw * 1024 / p.w, fy = (long long)bh * 1024 / p.h;
    long long f = fx > fy ? fx : fy;
    if (f < 1)
        f = 1;
    long long ox = ((long long)p.w * f / 1024 - bw) / 2, oy = ((long long)p.h * f / 1024 - bh) / 2;

    static int cols[4096];
    static int colsFor = -1, colsW = -1;
    if (colsFor != bw || colsW != p.w)
    {
        for (int x = 0; x < bw; x++)
        {
            long long sx = (x + ox) * 1024 / f;
            cols[x] = (int)(sx < 0 ? 0 : sx >= p.w ? p.w - 1 : sx);
        }
        colsFor = bw;
        colsW = p.w;
    }
    const bool straight = f == 1024 && ox == 0;
    for (int y = 0; y < bh; y++)
    {
        long long sy = (y + oy) * 1024 / f;
        const uint8_t *src = p.px + (size_t)(sy < 0 ? 0 : sy >= p.h ? p.h - 1 : sy) * p.w;
        uint8 *dst = px + (size_t)y * bw;
        if (straight)
            memcpy(dst, src, (size_t)bw);
        else
            for (int x = 0; x < bw; x++)
                dst[x] = src[cols[x]];
    }
}

// Wallpaper painted first; every window then draws on top of it.
static void drawWallpaper(PlatformDrawingContext *dc, PlatformBitmap *target)
{
    PlatformColor *&lit = wp_lit;
    PlatformColor *&dk = wp_dk;

    if (!lit)
        lit = dc->CreateColor(0xFFD0D0F8, nullptr, nullptr);
    if (!dk)
        dk = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
    if (g_wallPic.px)
    {
        paintWallpaperPicture(target);
        return;
    }
    if (!lit)
        return;

    const Coord W = target->GetWidth();
    const Coord H = target->GetHeight();

    // ── Stars: 8×5 grid, one star per 40×40 cell, offset for natural look ────
    static const int sx[] = {
        8,
        52,
        88,
        125,
        168,
        215,
        255,
        295, // cells y=0..40
        22,
        68,
        102,
        138,
        178,
        208,
        248,
        308, // cells y=40..80
        12,
        58,
        95,
        145,
        182,
        225,
        268,
        298, // cells y=80..120
        32,
        75,
        118,
        152,
        195,
        235,
        272,
        312, // cells y=120..160
        18,
        62,
        105,
        148,
        188,
        218,
        262,
        302, // cells y=160..200
    };
    static const int sy[] = {
        12,
        6,
        28,
        8,
        22,
        5,
        30,
        14,
        55,
        42,
        68,
        48,
        62,
        44,
        72,
        52,
        92,
        112,
        82,
        108,
        88,
        105,
        85,
        118,
        142,
        128,
        155,
        135,
        148,
        122,
        158,
        130,
        175,
        190,
        168,
        182,
        172,
        195,
        178,
        188,
    };
    //  The 320x200 pattern, repeated over a screen larger than that.
    const int w = (int)COORD_VAL(W), h = (int)COORD_VAL(H);
    for (int ty = 0; ty < h; ty += 200)
        for (int tx = 0; tx < w; tx += 320)
            for (int i = 0; i < 40; i++)
                target->FillRect(tx + sx[i], ty + sy[i], 2, 2, lit, false);
}
