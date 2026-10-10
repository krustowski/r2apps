// Exercise the real Files image viewer and codecs with a small host window
// and filesystem. Drawing primitives record text; the preview writes pixels.
#include "../image.h"
#include "../png.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0, allocations = 0, allocationBudget = -1;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

namespace web {
void *alloc(size_t n)
{
    if (allocationBudget == 0)
        return nullptr;
    if (allocationBudget > 0)
        allocationBudget--;
    void *p = std::malloc(n);
    if (p)
        allocations++;
    return p;
}
void *realloc(void *p, size_t n)
{
    if (!p)
        return alloc(n);
    return std::realloc(p, n);
}
void free(void *p) { if (p) { allocations--; std::free(p); } }
void *big_alloc(size_t n) { return alloc(n); }
void *big_realloc(void *p, size_t n) { return web::realloc(p, n); }
void big_free(void *p) { web::free(p); }
} // namespace web

static std::vector<uint8_t> file;
static std::optional<uint32_t> fileSize;
static size_t readChunk = 7, failReadAt = SIZE_MAX;
static int reads = 0;
static std::string requestedPath;

namespace r2 {
using string_view = std::string_view;
struct byte_span { uint8_t *data; size_t size; byte_span(uint8_t *p, size_t n) : data(p), size(n) {} };
namespace fs {
std::optional<uint32_t> size_of(string_view path) { requestedPath = path; return fileSize; }
int64_t read_at(string_view path, byte_span out, uint64_t offset)
{
    CHECK(path == requestedPath);
    reads++;
    if (offset >= failReadAt)
        return -1;
    if (offset >= file.size())
        return 0;
    size_t n = std::min({out.size, readChunk, file.size() - (size_t)offset});
    std::memcpy(out.data, file.data() + offset, n);
    return (int64_t)n;
}
} // namespace fs
} // namespace r2

using Coord = double;
using int32 = int32_t;
using uint8 = uint8_t;
using mchar = char;
#define COORD_VAL(c) (c)
struct PlatformColor {};
struct PlatformFont {};
enum class PlatformAlign { Begin, Middle, End };
struct PlatformDrawTextOptions {
    PlatformFont *font = nullptr;
    PlatformColor *foreground = nullptr;
    PlatformAlign horizontalAlign{}, verticalAlign{};
};
struct PlatformDrawingContext {
    PlatformColor colour;
    PlatformFont font;
    PlatformColor *CreateColor(uint32_t, void *, void *) { return &colour; }
    PlatformFont *CreateFont(int, void *, bool, bool, bool, void *, void *) { return &font; }
};
struct PlatformBitmap {
    int w, h;
    std::vector<std::string> text;
    Coord GetWidth() { return w; }
    Coord GetHeight() { return h; }
    void FillRect(Coord, Coord, Coord, Coord, PlatformColor *, bool) {}
    void DrawText(Coord, Coord, Coord, Coord, const char *s, PlatformDrawTextOptions *, bool) { text.emplace_back(s); }
    bool says(const char *s) const { return std::find(text.begin(), text.end(), s) != text.end(); }
};
struct PlatformWindow {
    int dpi = 192;
    bool closed = false;
    int GetEffectiveDPI() { return dpi; }
    void Close() { closed = true; }
};
struct PlatformKey { bool isKeyDown = true, isEscape = false, isEnter = false; };
enum class PlatformWindowInputEventType { OnPaint, OnKeyEvent, OnMouseClick };
enum class PlatformWindowButtonState { Pressed, Released };
struct PlatformWindowInterfaceInputEvent {
    PlatformWindowInputEventType type;
    struct {
        struct { PlatformDrawingContext *ctx; PlatformBitmap *target; } OnPaint;
        struct { PlatformKey *key; } OnKeyEvent;
        struct { Coord mouseX, mouseY; PlatformWindowButtonState state; } OnMouseClick;
    } Data{};
};
namespace MementoR2Impl {
static int paletteSize = 16;
struct R2_Palette { static uint32_t Count() { return paletteSize; } };
struct R2_Vga640x400 { static void ScreenSize(int32 &w, int32 &h) { w = 640; h = 400; } };
struct Dim { int n; int intValue() const { return n; } };
struct R2_BitmapImpl : PlatformBitmap {
    int bw, bh;
    std::vector<uint8_t> pixels;
    R2_BitmapImpl(int w, int h, int bw, int bh) : bw(bw), bh(bh), pixels((size_t)bw * bh + 32, 253)
    { this->w = w; this->h = h; }
    uint8_t *GetPixels() { return pixels.data(); }
    Dim GetRealWidth() { return {bw}; }
    Dim GetRealHeight() { return {bh}; }
};
} // namespace MementoR2Impl

#include "../../windows/image_viewer_window.cpp"

static void paint(ImageViewerWindow &viewer, MementoR2Impl::R2_BitmapImpl &bitmap)
{
    static PlatformDrawingContext dc;
    PlatformWindowInterfaceInputEvent event{};
    event.type = PlatformWindowInputEventType::OnPaint;
    event.Data.OnPaint = {&dc, &bitmap};
    ImageViewerWindow::onEvent(&viewer, &event);
}

static void load(const char *name)
{
    char path[128];
    std::snprintf(path, sizeof(path), "img/%s", name);
    FILE *f = std::fopen(path, "rb");
    CHECK(f);
    if (!f)
        std::exit(1);
    std::fseek(f, 0, SEEK_END);
    file.resize((size_t)std::ftell(f));
    std::rewind(f);
    CHECK(std::fread(file.data(), 1, file.size(), f) == file.size());
    std::fclose(f);
    fileSize = (uint32_t)file.size();
    failReadAt = SIZE_MAX;
    reads = 0;
}

// Check the actual written extent, including the padded allocation and guard
// bytes. Fixed expected rectangles catch DPI, centering and aspect mistakes.
static void extent(const MementoR2Impl::R2_BitmapImpl &bm, int x, int y, int w, int h)
{
    for (size_t i = 0; i < bm.pixels.size(); i++)
    {
        int px = (int)(i % bm.bw), py = (int)(i / bm.bw);
        bool inside = px >= x && px < x + w && py >= y && py < y + h;
        if ((bm.pixels[i] != 253) != inside)
        {
            CHECK(false);
            return;
        }
    }
}

static void formats()
{
    for (const char *path : {"/mnt/tmp/a.PNG", "/mnt/iso/photos/a.JpEg", "/mnt/fat/a.JPG",
                            "/mnt/fat/a.jpe", "/mnt/tmp/a.GIF", "/mnt/fat/a.BMP"})
        CHECK(ImageViewerWindow::accepts(path));
    for (const char *path : {"a.txt", "a.png.txt", "/mnt/a.png/file", "a", "a.svg", "a.webp"})
        CHECK(!ImageViewerWindow::accepts(path));
    for (int colours : {16, 256})
        for (const char *name : {"bw.png", "clear.png", "red.jpg", "wide.gif", "blue.bmp"})
        {
            MementoR2Impl::paletteSize = colours;
            load(name);
            {
                ImageViewerWindow viewer("/mnt/iso/photos/IMAGE.PNG");
                CHECK(requestedPath == "/mnt/iso/photos/IMAGE.PNG" && reads > 1);
                CHECK(allocations == 1); // Only decoded pixels survive loading.
                PlatformWindow window;
                viewer.SetWindow(&window);
                MementoR2Impl::R2_BitmapImpl bm(290, 150, 600, 300);
                paint(viewer, bm);
                CHECK(bm.says("Back") && bm.text.size() == 3);
                CHECK(std::any_of(bm.pixels.begin(), bm.pixels.end(), [](uint8_t p) { return p != 253; }));
            }
            CHECK(allocations == 0);
        }
}

static void layoutAndLifetime()
{
    MementoR2Impl::paletteSize = 16;
    load("wide.gif"); // 100x50, left at its native size in a roomy window.
    {
        ImageViewerWindow first("/mnt/tmp/ONE.GIF");
        PlatformWindow window;
        first.SetWindow(&window);
        MementoR2Impl::R2_BitmapImpl full(290, 150, 600, 450);
        paint(first, full);
        extent(full, 240, 125, 100, 50);
        CHECK(full.says("100x50"));

        // Width limits the image, then height, and a maximized client keeps
        // small images at native size rather than enlarging them.
        MementoR2Impl::R2_BitmapImpl narrow(30, 100, 150, 300);
        paint(first, narrow);
        extent(narrow, 4, 87, 52, 26);
        MementoR2Impl::R2_BitmapImpl shortWindow(100, 40, 300, 150);
        paint(first, shortWindow);
        extent(shortWindow, 84, 32, 32, 16);
        MementoR2Impl::R2_BitmapImpl maximized(320, 180, 750, 450);
        paint(first, maximized);
        extent(maximized, 270, 155, 100, 50);
        MementoR2Impl::R2_BitmapImpl tiny(3, 20, 150, 150);
        paint(first, tiny);
        extent(tiny, 0, 0, 0, 0);
        window.dpi = 96;
        MementoR2Impl::R2_BitmapImpl lowDpi(290, 150, 450, 300);
        paint(first, lowDpi);
        extent(lowDpi, 95, 50, 100, 50);

        load("red.jpg");
        { ImageViewerWindow second("/mnt/fat/TWO.JPG"); CHECK(allocations == 2); }
        CHECK(allocations == 1);
        MementoR2Impl::R2_BitmapImpl afterClose(290, 150, 450, 300);
        paint(first, afterClose);
        CHECK(afterClose.pixels == lowDpi.pixels); // Opening another did not replace this image.

        PlatformWindowInterfaceInputEvent click{};
        click.type = PlatformWindowInputEventType::OnMouseClick;
        click.Data.OnMouseClick = {20, 50, PlatformWindowButtonState::Pressed};
        ImageViewerWindow::onEvent(&first, &click);
        CHECK(!window.closed); // Looking at the picture does not dismiss it.
        click.Data.OnMouseClick = {110, 140, PlatformWindowButtonState::Pressed};
        ImageViewerWindow::onEvent(&first, &click);
        CHECK(window.closed);
    }
    CHECK(allocations == 0);
}

static void expectError(const char *message)
{
    {
        ImageViewerWindow viewer("/mnt/tmp/BAD.PNG");
        PlatformWindow window;
        viewer.SetWindow(&window);
        MementoR2Impl::R2_BitmapImpl bm(290, 150, 600, 300);
        paint(viewer, bm);
        CHECK(bm.says(message));
        extent(bm, 0, 0, 0, 0);
        PlatformKey key;
        key.isEscape = true;
        PlatformWindowInterfaceInputEvent event{};
        event.type = PlatformWindowInputEventType::OnKeyEvent;
        event.Data.OnKeyEvent.key = &key;
        ImageViewerWindow::onEvent(&viewer, &event);
        CHECK(window.closed);
    }
    CHECK(allocations == 0);
}

static void errors()
{
    fileSize.reset();
    expectError("Cannot read the file");
    fileSize = 0;
    expectError("Empty image file");
    fileSize = 32u * 1024 * 1024 + 1;
    expectError("Image file too large");
    load("bw.png");
    failReadAt = 14;
    expectError("Cannot read the whole file");
    failReadAt = SIZE_MAX;
    fileSize = (uint32_t)file.size() + 1;
    expectError("Cannot read the whole file");
    load("bw.png");
    file[0] = 0;
    expectError("Unsupported or damaged image (PNG, JPEG, GIF, BMP)");
    load("bw.png");
    allocationBudget = 0;
    expectError("Out of memory");
    allocationBudget = 1; // File buffer succeeds; decoded pixel allocation fails.
    expectError("out of memory");
    allocationBudget = 2; // The decoder itself runs out of memory.
    expectError("out of memory");
    allocationBudget = -1;
    // A recognizable header that advertises more than six million pixels.
    load("bw.png");
    const uint8_t dimension[4] = {0, 0, 0x0b, 0xb8}; // 3000
    std::memcpy(file.data() + 16, dimension, 4);
    std::memcpy(file.data() + 20, dimension, 4);
    expectError("too big to decode");
}

static void largeFile()
{
    // Larger than the text viewer's 32 KiB limit; still a modest image.
    std::vector<uint8_t> px(600 * 360);
    uint8_t palette[16 * 3];
    for (int i = 0; i < 16 * 3; i++)
        palette[i] = (uint8_t)(i * 5);
    uint32_t random = 17;
    for (auto &p : px)
    {
        random = random * 1664525 + 1013904223;
        p = (uint8_t)((random >> 28) & 15);
    }
    web::Buf png{true};
    CHECK(web::encodePng(px.data(), 600, 360, palette, 16, png));
    CHECK(png.len > 32768);
    file.assign(png.data, png.data + png.len);
    fileSize = (uint32_t)file.size();
    png.release();
    readChunk = 4096;
    {
        ImageViewerWindow viewer("/mnt/tmp/LARGE.PNG");
        PlatformWindow window;
        viewer.SetWindow(&window);
        MementoR2Impl::R2_BitmapImpl bm(290, 150, 600, 300);
        paint(viewer, bm);
        CHECK(bm.says("600x360"));
        extent(bm, 93, 32, 393, 236);
    }
    CHECK(allocations == 0);
}

int main()
{
    formats();
    layoutAndLifetime();
    errors();
    largeFile();
    std::printf("image viewer: %d failures\n", failures);
    return failures ? 1 : 0;
}
