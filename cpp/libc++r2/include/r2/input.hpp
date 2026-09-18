#ifndef _R2CXX_INPUT_HPP_
#define _R2CXX_INPUT_HPP_

/*
 *  input.hpp — keyboard and mouse.
 *
 *  Both arrive through syscall 0x03, the kernel's pipe mechanism: a process
 *  subscribes, then drains a ring buffer whenever it likes.  Nothing blocks,
 *  so a program that only polls must sleep somewhere in its loop or it will
 *  starve every other task on the system.
 *
 *  The keyboard delivers raw PS/2 set-1 scancodes, not characters.  Keyboard
 *  translates them, tracking shift so that the ASCII it returns is the
 *  character the user actually typed.
 */

#include "string.hpp"
#include "syscall.hpp"

namespace r2::input {

/*  Scancodes worth having a name for (PS/2 set 1, make codes).  */
enum Scancode : uint8_t {
    SC_ESCAPE = 0x01,
    SC_BACKSPACE = 0x0E,
    SC_TAB = 0x0F,
    SC_ENTER = 0x1C,
    SC_CTRL = 0x1D,
    SC_LSHIFT = 0x2A,
    SC_RSHIFT = 0x36,
    SC_ALT = 0x38,
    SC_SPACE = 0x39,
    SC_F1 = 0x3B,
    SC_F2 = 0x3C,
    SC_F3 = 0x3D,
    SC_F4 = 0x3E,
    SC_F5 = 0x3F,
    SC_F6 = 0x40,
    SC_F7 = 0x41,
    SC_F8 = 0x42,
    SC_F9 = 0x43,
    SC_F10 = 0x44,
    SC_UP = 0x48,
    SC_LEFT = 0x4B,
    SC_RIGHT = 0x4D,
    SC_DOWN = 0x50,
    SC_DELETE = 0x53,
};

/*  Break codes have the top bit set.  */
inline constexpr bool is_release(uint8_t scancode) { return (scancode & 0x80) != 0; }
inline constexpr uint8_t make_code(uint8_t scancode) { return (uint8_t)(scancode & 0x7F); }

/*
 *  Keyboard — subscribes on construction, unsubscribes on destruction.
 *
 *      Keyboard kbd;
 *      while (uint8_t sc = kbd.next_scancode()) { ... }
 */
class Keyboard {
  public:
    Keyboard();
    ~Keyboard();

    Keyboard(const Keyboard &) = delete;
    Keyboard &operator=(const Keyboard &) = delete;

    /*  Next raw scancode, or 0 when nothing is waiting.  */
    uint8_t next_scancode();

    /*
     *  Next printable character, or 0 when nothing is waiting.  Shift is
     *  tracked across calls; key releases and modifiers are consumed and
     *  return 0.  Enter comes back as '\n', backspace as '\b', escape as 27.
     */
    char next_char();

    /*  Discards everything currently buffered.  Worth doing when a program
     *  takes over the screen, so a keypress meant for the shell does not land
     *  in its first prompt.  */
    void drain();

    /*  Reads a line, echoing it to the console.  Blocks, sleeping between
     *  polls; returns when Enter is pressed.  */
    string read_line(string_view prompt = string_view());

    bool shift_down() const noexcept { return shift_; }
    bool ctrl_down() const noexcept { return ctrl_; }

  private:
    uint8_t mailbox_; /*  the kernel's direct-delivery slot  */
    uint8_t pending_[16];
    uint8_t pending_len_;
    uint8_t pending_pos_;
    bool shift_;
    bool ctrl_;
    bool subscribed_;
};

/*  Translates a set-1 make code to ASCII; 0 when there is no character.  */
char scancode_to_char(uint8_t scancode, bool shift);

/*
 *  Mouse — accumulates the PS/2 deltas into a position clamped to the screen.
 *
 *      Mouse mouse(320, 200);
 *      while (mouse.poll()) { ... }   // poll() returns true if anything moved
 */
class Mouse {
  public:
    Mouse(int32_t screen_width, int32_t screen_height);
    ~Mouse();

    Mouse(const Mouse &) = delete;
    Mouse &operator=(const Mouse &) = delete;

    /*  Drains the packet queue.  Returns true when position or buttons
     *  changed.  */
    bool poll();

    int32_t x() const noexcept { return x_; }
    int32_t y() const noexcept { return y_; }

    bool left() const noexcept { return (buttons_ & 0x01) != 0; }
    bool right() const noexcept { return (buttons_ & 0x02) != 0; }
    bool middle() const noexcept { return (buttons_ & 0x04) != 0; }

    /*  True on the transition from up to down since the previous poll().  */
    bool left_pressed() const noexcept { return (buttons_ & ~prev_buttons_ & 0x01) != 0; }
    bool right_pressed() const noexcept { return (buttons_ & ~prev_buttons_ & 0x02) != 0; }
    bool left_released() const noexcept { return (prev_buttons_ & ~buttons_ & 0x01) != 0; }

    void warp(int32_t x, int32_t y) noexcept;

  private:
    int32_t x_, y_;
    int32_t width_, height_;
    uint8_t buttons_;
    uint8_t prev_buttons_;
    bool subscribed_;
};

} // namespace r2::input

#endif
