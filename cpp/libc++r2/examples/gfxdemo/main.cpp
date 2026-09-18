/*
 *  gfxdemo — drawing, text and the mouse.
 *
 *  Draws into an off-screen 32-bit canvas and sends it to whichever display
 *  the kernel actually has:
 *
 *    - booted with the graphical kernel, the VESA framebuffer takes the canvas
 *      directly and scales it up;
 *    - booted in text mode, the program switches the VGA to mode 13h and
 *      pushes the same canvas through the brightness reduction.
 *
 *  Escape, or the right mouse button, quits --- and mode 13h is restored to
 *  text on the way out, by Vga13's destructor.
 */

#include <r2.hpp>

using namespace r2;
using namespace r2::gfx;

namespace {

constexpr uint32_t CANVAS_WIDTH = 320;
constexpr uint32_t CANVAS_HEIGHT = 200;

struct Ball {
    double x, y;
    double dx, dy;
    Color color;

    void step(double width, double height, double radius) {
        x += dx;
        y += dy;
        if (x < radius || x > width - radius)
            dx = -dx;
        if (y < radius || y > height - radius)
            dy = -dy;
    }
};

void draw_frame(Canvas &canvas, const Font *font, const vector<Ball> &balls, int32_t mouse_x,
                int32_t mouse_y, uint64_t frame) {
    canvas.clear(colors::DarkBlue);

    /*  A grid, to make the scaling on a VESA screen visible.  */
    for (uint32_t x = 0; x < canvas.width(); x += 40)
        canvas.draw_vline((int32_t)x, 0, (int32_t)canvas.height(), Color(30, 40, 90));
    for (uint32_t y = 0; y < canvas.height(); y += 40)
        canvas.draw_hline(0, (int32_t)y, (int32_t)canvas.width(), Color(30, 40, 90));

    for (const Ball &ball : balls)
        canvas.fill_circle((int32_t)ball.x, (int32_t)ball.y, 8, ball.color);

    /*  A wave, drawn with the library's own sin().  */
    for (int32_t x = 0; x < (int32_t)canvas.width(); x++) {
        double phase = (double)x * 0.05 + (double)frame * 0.1;
        int32_t y = (int32_t)(canvas.height() - 30 + sin(phase) * 12.0);
        canvas.set_pixel(x, y, colors::Cyan);
    }

    if (font) {
        canvas.draw_text(*font, 8, 8, string_view("libc++r2 gfx demo"), colors::White);
        canvas.draw_text(*font, 8, 8 + (int32_t)font->char_height() + 2,
                         format("frame {}  mouse {},{}", frame, mouse_x, mouse_y).view(),
                         colors::Yellow);
        canvas.draw_text(*font, 8, (int32_t)canvas.height() - (int32_t)font->char_height() - 8,
                         string_view("ESC or right button to quit"), colors::Gray);
    }

    /*  The mouse pointer.  */
    canvas.draw_line(mouse_x, mouse_y, mouse_x, mouse_y + 10, colors::White);
    canvas.draw_line(mouse_x, mouse_y, mouse_x + 7, mouse_y + 7, colors::White);
    canvas.draw_line(mouse_x + 7, mouse_y + 7, mouse_x, mouse_y + 10, colors::White);
}

} // namespace

int main() {
    auto canvas = Canvas::create(CANVAS_WIDTH, CANVAS_HEIGHT);
    if (!canvas) {
        println("gfxdemo: no room in the arena for a 320x200 canvas");
        return 1;
    }

    auto font = Font::kernel();

    /*  Prefer the framebuffer; fall back to VGA mode 13h.  */
    auto display = Display::open();
    optional<Vga13> vga;

    if (display) {
        printf("gfxdemo: framebuffer {}x{} at {} bpp\n", display->width(), display->height(),
               display->bpp());
    } else {
        vga = Vga13::open();
        if (!vga) {
            println("gfxdemo: no framebuffer and no VGA; nothing to draw on");
            return 1;
        }
    }

    vector<Ball> balls;
    (void)balls.push_back(Ball{60, 50, 1.7, 1.1, colors::Red});
    (void)balls.push_back(Ball{160, 120, -1.3, 1.9, colors::Green});
    (void)balls.push_back(Ball{240, 80, 2.1, -1.5, colors::Yellow});

    input::Keyboard keyboard;
    input::Mouse mouse((int32_t)CANVAS_WIDTH, (int32_t)CANVAS_HEIGHT);
    keyboard.drain();

    FrameTimer frame_timer(20); /*  50 frames a second  */
    uint64_t frame = 0;
    bool running = true;

    while (running) {
        for (uint8_t scancode = keyboard.next_scancode(); scancode != 0;
             scancode = keyboard.next_scancode()) {
            if (!input::is_release(scancode) && input::make_code(scancode) == input::SC_ESCAPE)
                running = false;
        }

        mouse.poll();
        if (mouse.right())
            running = false;

        for (Ball &ball : balls)
            ball.step(CANVAS_WIDTH, CANVAS_HEIGHT, 8);

        draw_frame(*canvas, font ? &*font : nullptr, balls, mouse.x(), mouse.y(), frame);

        if (display)
            display->present(*canvas);
        else
            vga->present_reduced(*canvas);

        frame++;
        frame_timer.wait();
    }

    if (vga)
        vga->close();

    println("gfxdemo: ", frame, " frames drawn");
    return 0;
}
