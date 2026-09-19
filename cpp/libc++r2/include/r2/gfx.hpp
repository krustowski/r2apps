#ifndef _R2CXX_GFX_HPP_
#define _R2CXX_GFX_HPP_

/*
 *  gfx.hpp — drawing.
 *
 *  r2 offers two ways onto the screen and they are not interchangeable:
 *
 *  The VESA framebuffer.  get_fb_info (0x16) describes it and blit_buffer
 *  (0x17) takes a 32-bit 0x00RRGGBB buffer for the whole frame.  A full-screen
 *  buffer at 1024x768 is 3 MiB, which does not fit in a process that has 2 MiB
 *  for everything, so Display scales: draw into a small Canvas --- 320x200 is
 *  256 KiB --- and let the kernel stretch it on the way out.  That is what the
 *  second form of syscall 0x17 is for.
 *
 *  VGA mode 13h.  map_vram (0x14) plus set_video_mode(0x13) gives a 320x200
 *  byte-per-pixel surface mapped straight into the process.  One byte per
 *  pixel, 256 palette entries, no blit syscall per frame.  Vga13 restores the
 *  text mode in its destructor, so a program that returns to the shell does
 *  not leave it in graphics mode.
 *
 *  Text comes from the kernel's own PSF font (syscall 0x18): 8 pixels wide,
 *  one byte per row, most significant bit leftmost.
 */

#include "optional.hpp"
#include "span.hpp"
#include "string.hpp"
#include "syscall.hpp"
#include "vector.hpp"

namespace r2::gfx {

/* ------------------------------------------------------------------------ *
 *  Color
 * ------------------------------------------------------------------------ */

struct Color {
    uint8_t r, g, b;

    constexpr Color() : r(0), g(0), b(0) {}
    constexpr Color(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

    /*  The packed form the blit syscall wants.  */
    constexpr uint32_t packed() const { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

    static constexpr Color from_packed(uint32_t v) {
        return Color((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    }

    /*  Sum of the channels, 0..765 --- the cheap stand-in for luminance that
     *  the mode-13h reduction uses.  */
    constexpr uint16_t brightness() const { return (uint16_t)r + g + b; }

    constexpr Color blend(Color other, uint8_t alpha) const {
        return Color((uint8_t)((r * (255 - alpha) + other.r * alpha) / 255),
                     (uint8_t)((g * (255 - alpha) + other.g * alpha) / 255),
                     (uint8_t)((b * (255 - alpha) + other.b * alpha) / 255));
    }
};

namespace colors {
inline constexpr Color Black{0, 0, 0};
inline constexpr Color White{255, 255, 255};
inline constexpr Color Red{200, 40, 40};
inline constexpr Color Green{40, 200, 40};
inline constexpr Color Blue{40, 80, 220};
inline constexpr Color Yellow{230, 220, 60};
inline constexpr Color Cyan{60, 210, 210};
inline constexpr Color Magenta{200, 60, 200};
inline constexpr Color Gray{128, 128, 128};
inline constexpr Color DarkGray{64, 64, 64};
inline constexpr Color DarkBlue{20, 25, 70};
} // namespace colors

/* ------------------------------------------------------------------------ *
 *  Font
 * ------------------------------------------------------------------------ */

/*
 *  The kernel's embedded PSF font.  Glyph n occupies char_height() bytes, one
 *  per row, 8 pixels wide with bit 7 on the left.
 */
class Font {
  public:
    static constexpr uint32_t CHAR_WIDTH = 8;
    static constexpr size_t MAX_GLYPH_BYTES = 256 * 32;

    /*  Loads the kernel font (syscall 0x18).  nullopt if the kernel has none. */
    static optional<Font> kernel();

    uint32_t char_width() const noexcept { return CHAR_WIDTH; }
    uint32_t char_height() const noexcept { return height_; }

    /*  The rows of one glyph, or nullptr when the font failed to load.  */
    const uint8_t *glyph(uint8_t ch) const noexcept {
        return glyphs_.empty() ? nullptr : glyphs_.data() + (size_t)ch * height_;
    }

    uint32_t text_width(string_view text) const noexcept {
        return (uint32_t)text.size() * CHAR_WIDTH;
    }

  private:
    Font() : height_(0) {}
    vector<uint8_t> glyphs_;
    uint32_t height_;
};

/* ------------------------------------------------------------------------ *
 *  Canvas — a 32-bit off-screen surface
 * ------------------------------------------------------------------------ */

class Canvas {
  public:
    /*  Allocates width*height*4 bytes from the arena.  A 320x200 canvas is
     *  256 KiB, so check the result: it is a large fraction of the heap.  */
    static optional<Canvas> create(uint32_t width, uint32_t height);

    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }

    uint32_t *pixels() noexcept { return pixels_.data(); }
    const uint32_t *pixels() const noexcept { return pixels_.data(); }

    void clear(Color c);

    void set_pixel(int32_t x, int32_t y, Color c) {
        if (x >= 0 && y >= 0 && (uint32_t)x < width_ && (uint32_t)y < height_)
            pixels_[(size_t)y * width_ + (size_t)x] = c.packed();
    }

    Color get_pixel(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || (uint32_t)x >= width_ || (uint32_t)y >= height_)
            return Color();
        return Color::from_packed(pixels_[(size_t)y * width_ + (size_t)x]);
    }

    void blend_pixel(int32_t x, int32_t y, Color c, uint8_t alpha);

    void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, Color c);
    void draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, Color c);
    void draw_hline(int32_t x, int32_t y, int32_t len, Color c);
    void draw_vline(int32_t x, int32_t y, int32_t len, Color c);
    void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, Color c);
    void draw_circle(int32_t cx, int32_t cy, int32_t radius, Color c);
    void fill_circle(int32_t cx, int32_t cy, int32_t radius, Color c);

    /*  Draws text with the given font.  scale draws each glyph pixel as a
     *  scale x scale block.  Returns the x coordinate just past the text.  */
    int32_t draw_text(const Font &font, int32_t x, int32_t y, string_view text, Color fg,
                      uint32_t scale = 1);

    /*  Same, with a filled background behind the glyphs.  */
    int32_t draw_text_bg(const Font &font, int32_t x, int32_t y, string_view text, Color fg,
                         Color bg, uint32_t scale = 1);

    /*  Copies another canvas in at (x, y), clipped to this one.  */
    void blit(const Canvas &src, int32_t x, int32_t y);

  private:
    Canvas() : width_(0), height_(0) {}
    vector<uint32_t> pixels_;
    uint32_t width_;
    uint32_t height_;
};

/* ------------------------------------------------------------------------ *
 *  Display — the VESA framebuffer
 * ------------------------------------------------------------------------ */

class Display {
  public:
    /*
     *  Queries the framebuffer (syscall 0x16).  nullopt when the kernel was
     *  booted in text mode and has none --- note that the syscall still
     *  succeeds there and describes the 80x25 character buffer, so open()
     *  additionally requires true colour and at least a 320x200 screen before
     *  it believes the answer.  Fall back to Vga13 when this returns nullopt.
     */
    static optional<Display> open();

    uint32_t width() const noexcept { return info_.width; }
    uint32_t height() const noexcept { return info_.height; }
    uint32_t pitch() const noexcept { return info_.pitch; }
    uint32_t bpp() const noexcept { return info_.bpp; }
    const FbInfo &info() const noexcept { return info_; }

    /*
     *  Sends a frame.  When the canvas is the size of the framebuffer the
     *  buffer goes straight out; otherwise the kernel scales it with
     *  nearest-neighbour sampling, which is how a 320x200 canvas fills a
     *  1024x768 screen without a 3 MiB buffer.
     */
    bool present(const Canvas &canvas);

    /*  A canvas sized to keep the framebuffer's aspect ratio, no wider than
     *  max_width.  The natural argument for present().  */
    optional<Canvas> make_canvas(uint32_t max_width = 320) const;

  private:
    Display() : info_{0, 0, 0, 0} {}
    FbInfo info_;
};

/* ------------------------------------------------------------------------ *
 *  VGA mode 13h — 320x200, one byte per pixel
 * ------------------------------------------------------------------------ */

inline constexpr uint32_t VGA13_WIDTH = 320;
inline constexpr uint32_t VGA13_HEIGHT = 200;

class Vga13 {
  public:
    /*  Switches to mode 13h and maps VGA RAM into the process.  */
    static optional<Vga13> open();

    Vga13(Vga13 &&other) noexcept;
    Vga13 &operator=(Vga13 &&other) noexcept;
    Vga13(const Vga13 &) = delete;
    Vga13 &operator=(const Vga13 &) = delete;

    /*  Restores 80x25 text mode.  */
    ~Vga13();

    uint8_t *vram() noexcept { return vram_; }

    void clear(uint8_t index);

    void set_pixel(int32_t x, int32_t y, uint8_t index) {
        if (x >= 0 && y >= 0 && (uint32_t)x < VGA13_WIDTH && (uint32_t)y < VGA13_HEIGHT)
            vram_[(size_t)y * VGA13_WIDTH + (size_t)x] = index;
    }

    void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t index);

    void draw_text(const Font &font, int32_t x, int32_t y, string_view text, uint8_t index);

    /*
     *  Reduces a 32-bit canvas to two palette entries by brightness, scaling
     *  it to 320x200 on the way.  Crude, but it is what makes a canvas-drawn
     *  interface visible in mode 13h without a color quantizer.
     */
    void present_reduced(const Canvas &canvas, uint8_t bright_index = 15,
                         uint8_t dark_index = 1, uint16_t threshold = 300);

    /*  Restores text mode early; the destructor then does nothing.  */
    void close();

  private:
    Vga13() : vram_(nullptr) {}
    uint8_t *vram_;
};

/*
 *  Renders a 320x200 palette-indexed buffer through the kernel (syscall 0x13)
 *  rather than through VGA hardware.  palette is 768 bytes (256 x RGB) or
 *  empty for the kernel's default.
 */
bool present_indexed(const_byte_span surface, const_byte_span palette = const_byte_span());

/*  Sets a VGA text or graphics mode directly (syscall 0x15).  */
inline bool set_video_mode(uint8_t mode) {
    return raw_syscall(Sys::SetVideoMode, mode, 0) == 0;
}

/*  Restores the 80x25 text console.  */
inline void restore_text_mode() { set_video_mode(0x03); }

} // namespace r2::gfx

#endif
