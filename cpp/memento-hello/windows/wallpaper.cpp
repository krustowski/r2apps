// File-level so resetWallpaperCache() can null them after r2_heap_restore.
static PlatformColor *wp_lit = nullptr;
static PlatformColor *wp_dk = nullptr;
static void resetWallpaperCache()
{
    wp_lit = nullptr;
    wp_dk = nullptr;
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
    for (int i = 0; i < 40; i++)
        target->FillRect(sx[i], sy[i], 2, 2, lit, false);
}
