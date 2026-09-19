/*
 *  gfx.cpp — canvases, fonts and the two ways onto the screen.
 *
 *  The glyph loop and the mode-13h brightness reduction come from the r2
 *  backend of Memento (cpp/memento-hello): 8 pixels per row, bit 7 leftmost,
 *  and a two-colour split at a brightness of 300 out of 765.
 */

#include "r2/gfx.hpp"
#include "r2/math.hpp"
#include "r2/libc.hpp"

namespace r2::gfx {

/* ------------------------------------------------------------------------ *
 *  Font
 * ------------------------------------------------------------------------ */

optional<Font> Font::kernel() {
    Font font;

    /*  The syscall reports the glyph height in its return value, so the buffer
     *  has to be big enough for the tallest font before we know which one it
     *  is: 256 glyphs of at most 32 rows.  */
    if (!font.glyphs_.resize(MAX_GLYPH_BYTES))
        return nullopt;

    int64_t char_size =
        raw_syscall(Sys::GetKernelFont, (int64_t)font.glyphs_.data(), (int64_t)MAX_GLYPH_BYTES);

    if (char_size <= 0 || char_size > 32)
        return nullopt;

    font.height_ = (uint32_t)char_size;
    (void)font.glyphs_.resize(256 * font.height_);
    return font;
}

/* ------------------------------------------------------------------------ *
 *  Canvas
 * ------------------------------------------------------------------------ */

optional<Canvas> Canvas::create(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return nullopt;

    Canvas canvas;
    if (!canvas.pixels_.resize((size_t)width * height))
        return nullopt;

    canvas.width_ = width;
    canvas.height_ = height;
    return canvas;
}

void Canvas::clear(Color c) {
    uint32_t packed = c.packed();
    uint32_t *p = pixels_.data();
    size_t count = (size_t)width_ * height_;

    for (size_t i = 0; i < count; i++)
        p[i] = packed;
}

void Canvas::blend_pixel(int32_t x, int32_t y, Color c, uint8_t alpha) {
    if (x < 0 || y < 0 || (uint32_t)x >= width_ || (uint32_t)y >= height_)
        return;
    if (alpha == 0xFF) {
        set_pixel(x, y, c);
        return;
    }

    Color base = get_pixel(x, y);
    set_pixel(x, y, base.blend(c, alpha));
}

void Canvas::fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, Color c) {
    if (w <= 0 || h <= 0)
        return;

    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > (int32_t)width_)
        x1 = (int32_t)width_;
    if (y1 > (int32_t)height_)
        y1 = (int32_t)height_;

    uint32_t packed = c.packed();
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *line = pixels_.data() + (size_t)row * width_;
        for (int32_t col = x0; col < x1; col++)
            line[col] = packed;
    }
}

void Canvas::draw_hline(int32_t x, int32_t y, int32_t len, Color c) {
    fill_rect(x, y, len, 1, c);
}

void Canvas::draw_vline(int32_t x, int32_t y, int32_t len, Color c) {
    fill_rect(x, y, 1, len, c);
}

void Canvas::draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, Color c) {
    if (w <= 0 || h <= 0)
        return;
    draw_hline(x, y, w, c);
    draw_hline(x, y + h - 1, w, c);
    draw_vline(x, y, h, c);
    draw_vline(x + w - 1, y, h, c);
}

void Canvas::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color c) {
    /*  Bresenham, all octants.  */
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    for (;;) {
        set_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            return;

        int32_t e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void Canvas::draw_circle(int32_t cx, int32_t cy, int32_t radius, Color c) {
    if (radius <= 0)
        return;

    int32_t x = radius;
    int32_t y = 0;
    int32_t err = 1 - radius;

    while (x >= y) {
        set_pixel(cx + x, cy + y, c);
        set_pixel(cx + y, cy + x, c);
        set_pixel(cx - y, cy + x, c);
        set_pixel(cx - x, cy + y, c);
        set_pixel(cx - x, cy - y, c);
        set_pixel(cx - y, cy - x, c);
        set_pixel(cx + y, cy - x, c);
        set_pixel(cx + x, cy - y, c);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void Canvas::fill_circle(int32_t cx, int32_t cy, int32_t radius, Color c) {
    if (radius <= 0)
        return;

    for (int32_t dy = -radius; dy <= radius; dy++) {
        int32_t span = (int32_t)r2::isqrt((uint64_t)(radius * radius - dy * dy));
        draw_hline(cx - span, cy + dy, span * 2 + 1, c);
    }
}

int32_t Canvas::draw_text(const Font &font, int32_t x, int32_t y, string_view text, Color fg,
                          uint32_t scale) {
    if (scale == 0)
        scale = 1;

    uint32_t glyph_height = font.char_height();
    uint32_t glyph_width = font.char_width();

    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t *glyph = font.glyph((uint8_t)text[i]);
        int32_t origin_x = x + (int32_t)(i * glyph_width * scale);

        if (!glyph) {
            /*  No kernel font: a filled box keeps the layout honest instead of
             *  silently drawing nothing.  */
            draw_rect(origin_x, y, (int32_t)(glyph_width * scale),
                      (int32_t)(glyph_height * scale), fg);
            continue;
        }

        for (uint32_t row = 0; row < glyph_height; row++) {
            uint8_t bits = glyph[row];
            if (bits == 0)
                continue;

            for (uint32_t col = 0; col < glyph_width; col++) {
                if ((bits & (0x80u >> col)) == 0)
                    continue;

                if (scale == 1) {
                    set_pixel(origin_x + (int32_t)col, y + (int32_t)row, fg);
                } else {
                    fill_rect(origin_x + (int32_t)(col * scale), y + (int32_t)(row * scale),
                              (int32_t)scale, (int32_t)scale, fg);
                }
            }
        }
    }

    return x + (int32_t)(text.size() * glyph_width * scale);
}

int32_t Canvas::draw_text_bg(const Font &font, int32_t x, int32_t y, string_view text, Color fg,
                             Color bg, uint32_t scale) {
    if (scale == 0)
        scale = 1;

    fill_rect(x, y, (int32_t)(text.size() * font.char_width() * scale),
              (int32_t)(font.char_height() * scale), bg);

    return draw_text(font, x, y, text, fg, scale);
}

void Canvas::blit(const Canvas &src, int32_t x, int32_t y) {
    for (uint32_t row = 0; row < src.height_; row++) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || (uint32_t)dy >= height_)
            continue;

        for (uint32_t col = 0; col < src.width_; col++) {
            int32_t dx = x + (int32_t)col;
            if (dx < 0 || (uint32_t)dx >= width_)
                continue;

            pixels_[(size_t)dy * width_ + (size_t)dx] =
                src.pixels_[(size_t)row * src.width_ + (size_t)col];
        }
    }
}

/* ------------------------------------------------------------------------ *
 *  Display — the VESA framebuffer
 * ------------------------------------------------------------------------ */

optional<Display> Display::open() {
    Display display;
    memset(&display.info_, 0, sizeof(display.info_));

    if (raw_syscall(Sys::GetFbInfo, (int64_t)&display.info_, 0) != 0)
        return nullopt;

    /*
     *  A text-mode boot has no framebuffer, but syscall 0x16 still succeeds:
     *  it reports the 80x25 character buffer instead, 8 bits deep.  Accepting
     *  that and blitting a canvas into it puts 32-bit pixels in text VRAM,
     *  where the VGA reads each one as a character/attribute pair --- the
     *  screen fills with coloured letters instead of the picture.  A real
     *  linear framebuffer is true colour and at least a mode-13h screen in
     *  size; c/them/gfx.c rejects the character buffer the same way.
     */
    if (display.info_.bpp < 24)
        return nullopt;

    if (display.info_.width < VGA13_WIDTH || display.info_.height < VGA13_HEIGHT)
        return nullopt;

    return display;
}

bool Display::present(const Canvas &canvas) {
    const uint32_t *pixels = canvas.pixels();
    if (!pixels)
        return false;

    if (canvas.width() == info_.width && canvas.height() == info_.height)
        return raw_syscall(Sys::BlitBuffer, (int64_t)pixels, 0) == 0;

    /*  Second form of syscall 0x17: source dimensions packed into arg2, and
     *  the kernel scales with nearest-neighbour sampling on the way out.  */
    int64_t dimensions = ((int64_t)canvas.width() << 16) | (int64_t)canvas.height();
    return raw_syscall(Sys::BlitBuffer, (int64_t)pixels, dimensions) == 0;
}

optional<Canvas> Display::make_canvas(uint32_t max_width) const {
    if (info_.width == 0 || info_.height == 0)
        return nullopt;

    uint32_t width = info_.width < max_width ? info_.width : max_width;
    uint32_t height = (uint32_t)((uint64_t)width * info_.height / info_.width);
    if (height == 0)
        height = 1;

    return Canvas::create(width, height);
}

/* ------------------------------------------------------------------------ *
 *  VGA mode 13h
 * ------------------------------------------------------------------------ */

optional<Vga13> Vga13::open() {
    /*
     *  VRAM first, mode second, which is the order gfxtest, cube and them use.
     *  Mapping can fail --- and if it does, the screen is still the text mode
     *  the shell left behind, rather than a graphics mode with nothing able to
     *  draw into it.
     */
    uint64_t base = 0;
    raw_syscall(Sys::MapVram, 0, (int64_t)&base);
    if (base == 0)
        return nullopt;

    if (raw_syscall(Sys::SetVideoMode, 0x13, 0) != 0)
        return nullopt;

    Vga13 vga;
    vga.vram_ = (uint8_t *)base;
    return vga;
}

Vga13::Vga13(Vga13 &&other) noexcept : vram_(other.vram_) { other.vram_ = nullptr; }

Vga13 &Vga13::operator=(Vga13 &&other) noexcept {
    if (this != &other) {
        close();
        vram_ = other.vram_;
        other.vram_ = nullptr;
    }
    return *this;
}

Vga13::~Vga13() { close(); }

void Vga13::close() {
    if (!vram_)
        return;
    vram_ = nullptr;
    restore_text_mode();
}

void Vga13::clear(uint8_t index) {
    if (vram_)
        memset(vram_, index, (size_t)VGA13_WIDTH * VGA13_HEIGHT);
}

void Vga13::fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t index) {
    if (!vram_ || w <= 0 || h <= 0)
        return;

    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;
    if (x1 > (int32_t)VGA13_WIDTH)
        x1 = (int32_t)VGA13_WIDTH;
    if (y1 > (int32_t)VGA13_HEIGHT)
        y1 = (int32_t)VGA13_HEIGHT;

    for (int32_t row = y0; row < y1; row++)
        memset(vram_ + (size_t)row * VGA13_WIDTH + x0, index, (size_t)(x1 - x0));
}

void Vga13::draw_text(const Font &font, int32_t x, int32_t y, string_view text, uint8_t index) {
    if (!vram_)
        return;

    uint32_t glyph_height = font.char_height();

    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t *glyph = font.glyph((uint8_t)text[i]);
        if (!glyph)
            continue;

        int32_t origin_x = x + (int32_t)(i * font.char_width());

        for (uint32_t row = 0; row < glyph_height; row++) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < font.char_width(); col++)
                if (bits & (0x80u >> col))
                    set_pixel(origin_x + (int32_t)col, y + (int32_t)row, index);
        }
    }
}

void Vga13::present_reduced(const Canvas &canvas, uint8_t bright_index, uint8_t dark_index,
                            uint16_t threshold) {
    if (!vram_ || canvas.width() == 0 || canvas.height() == 0)
        return;

    const uint32_t *src = canvas.pixels();
    uint32_t src_w = canvas.width();
    uint32_t src_h = canvas.height();

    for (uint32_t dy = 0; dy < VGA13_HEIGHT; dy++) {
        uint32_t sy = dy * src_h / VGA13_HEIGHT;
        const uint32_t *src_row = src + (size_t)sy * src_w;
        uint8_t *dst_row = vram_ + (size_t)dy * VGA13_WIDTH;

        for (uint32_t dx = 0; dx < VGA13_WIDTH; dx++) {
            uint32_t sx = dx * src_w / VGA13_WIDTH;
            uint32_t pixel = src_row[sx];
            uint16_t brightness = (uint16_t)(((pixel >> 16) & 0xFF) + ((pixel >> 8) & 0xFF) +
                                             (pixel & 0xFF));
            dst_row[dx] = brightness > threshold ? bright_index : dark_index;
        }
    }
}

bool present_indexed(const_byte_span surface, const_byte_span palette) {
    if (surface.size() < (size_t)VGA13_WIDTH * VGA13_HEIGHT)
        return false;

    return raw_syscall(Sys::WriteVga, (int64_t)surface.data(),
                       palette.empty() ? 0 : (int64_t)palette.data()) == 0;
}

} // namespace r2::gfx
