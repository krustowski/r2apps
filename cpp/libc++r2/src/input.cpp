/*
 *  input.cpp — keyboard and mouse, both through the kernel's pipe syscall.
 *
 *  The keyboard has two delivery paths and a program has to read both.  The
 *  kernel pushes each scancode into a per-process ring buffer (read with
 *  0x03/0x03) and also writes it into a mailbox byte the process registered at
 *  subscribe time.  The ring buffer needs the kernel to resolve the current
 *  pid, which it cannot always do while another task holds the scheduler; the
 *  mailbox does not.  Reading the ring first and falling back to the mailbox
 *  catches the keystrokes that the ring path misses --- and clearing the
 *  mailbox whenever the ring delivers stops the same key being handled twice.
 *  That behaviour is inherited from the Memento r2 backend, where it was
 *  worked out the hard way.
 */

#include "r2/input.hpp"
#include "r2/io.hpp"
#include "r2/libc.hpp"
#include "r2/time.hpp"

namespace r2::input {

namespace {

/*  Pipe operations (syscall 0x03, arg1).  */
constexpr int64_t PIPE_SUBSCRIBE = 0x01;
constexpr int64_t PIPE_UNSUBSCRIBE = 0x02;
constexpr int64_t PIPE_READ = 0x03;
constexpr int64_t MOUSE_SUBSCRIBE = 0x04;
constexpr int64_t MOUSE_READ = 0x05;
constexpr int64_t MOUSE_UNSUBSCRIBE = 0x06;

/*  PS/2 set 1, unshifted then shifted, for make codes 0x00-0x39.  */
const char SCANCODE_LOWER[] = "\0\0" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
const char SCANCODE_UPPER[] = "\0\0" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

} // namespace

char scancode_to_char(uint8_t scancode, bool shift) {
    if (scancode >= sizeof(SCANCODE_LOWER) - 1)
        return 0;

    char c = shift ? SCANCODE_UPPER[scancode] : SCANCODE_LOWER[scancode];
    return c;
}

/* ------------------------------------------------------------------------ *
 *  Keyboard
 * ------------------------------------------------------------------------ */

Keyboard::Keyboard()
    : mailbox_(0), pending_len_(0), pending_pos_(0), shift_(false), ctrl_(false),
      subscribed_(false) {
    memset(pending_, 0, sizeof(pending_));
    subscribed_ = raw_syscall(Sys::PipeSubscribe, PIPE_SUBSCRIBE, (int64_t)&mailbox_) == 0;
}

Keyboard::~Keyboard() {
    if (subscribed_)
        raw_syscall(Sys::PipeSubscribe, PIPE_UNSUBSCRIBE, (int64_t)&mailbox_);
}

uint8_t Keyboard::next_scancode() {
    if (pending_pos_ < pending_len_)
        return pending_[pending_pos_++];

    memset(pending_, 0, sizeof(pending_));
    pending_pos_ = 0;
    pending_len_ = 0;

    raw_syscall(Sys::PipeSubscribe, PIPE_READ, (int64_t)pending_);

    if (pending_[0] != 0) {
        /*  The ring delivered: drop the mailbox copy of the same keystroke. */
        mailbox_ = 0;
        while (pending_len_ < sizeof(pending_) && pending_[pending_len_] != 0)
            pending_len_++;
        return pending_[pending_pos_++];
    }

    if (mailbox_ != 0) {
        uint8_t scancode = mailbox_;
        mailbox_ = 0;
        return scancode;
    }

    return 0;
}

char Keyboard::next_char() {
    for (;;) {
        uint8_t scancode = next_scancode();
        if (scancode == 0)
            return 0;

        uint8_t code = make_code(scancode);
        bool released = is_release(scancode);

        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            shift_ = !released;
            continue;
        }
        if (code == SC_CTRL) {
            ctrl_ = !released;
            continue;
        }
        if (released)
            continue;

        switch (code) {
        case SC_ENTER:
            return '\n';
        case SC_BACKSPACE:
            return '\b';
        case SC_ESCAPE:
            return (char)27;
        case SC_TAB:
            return '\t';
        default:
            break;
        }

        char c = scancode_to_char(code, shift_);
        if (c != 0)
            return c;
    }
}

void Keyboard::drain() {
    /*
     *  Let any key still in flight arrive, then empty the ring.  Without the
     *  pause, the break code of the keypress that started this program lands
     *  in the buffer just after it was cleared.
     */
    sleep(200);

    for (int guard = 0; guard < 64; guard++) {
        uint8_t buffer[16];
        memset(buffer, 0, sizeof(buffer));
        raw_syscall(Sys::PipeSubscribe, PIPE_READ, (int64_t)buffer);
        if (buffer[0] == 0)
            break;
    }

    mailbox_ = 0;
    pending_len_ = 0;
    pending_pos_ = 0;
    shift_ = false;
    ctrl_ = false;
}

string Keyboard::read_line(string_view prompt) {
    if (!prompt.empty())
        print(prompt);

    string line;

    for (;;) {
        char c = next_char();

        if (c == 0) {
            /*  Nothing waiting: sleep rather than spin, or every other task on
             *  the system starves.  */
            sleep(10);
            continue;
        }

        if (c == '\n') {
            print("\n");
            return line;
        }

        if (c == '\b') {
            if (!line.empty()) {
                line.pop_back();
                print("\b \b");
            }
            continue;
        }

        if (line.append(c))
            print(c);
    }
}

/* ------------------------------------------------------------------------ *
 *  Mouse
 * ------------------------------------------------------------------------ */

Mouse::Mouse(int32_t screen_width, int32_t screen_height)
    : x_(screen_width / 2), y_(screen_height / 2), width_(screen_width), height_(screen_height),
      buttons_(0), prev_buttons_(0), subscribed_(false) {
    subscribed_ = raw_syscall(Sys::PipeSubscribe, MOUSE_SUBSCRIBE, 0) == 0;
}

Mouse::~Mouse() {
    if (subscribed_)
        raw_syscall(Sys::PipeSubscribe, MOUSE_UNSUBSCRIBE, 0);
}

bool Mouse::poll() {
    /*  The kernel hands over at most five packets per call, so drain in a
     *  loop; without it a fast movement lags behind by whole frames.  */
    constexpr int MAX_PACKETS = 16;
    constexpr int MAX_ROUNDS = 8;

    MousePacket packets[MAX_PACKETS];
    bool changed = false;

    prev_buttons_ = buttons_;

    for (int round = 0; round < MAX_ROUNDS; round++) {
        memset(packets, 0, sizeof(packets));

        int64_t bytes = raw_syscall(Sys::PipeSubscribe, MOUSE_READ, (int64_t)packets);
        if (bytes <= 0)
            break;

        int count = (int)(bytes / (int64_t)sizeof(MousePacket));
        if (count > MAX_PACKETS)
            count = MAX_PACKETS;

        for (int i = 0; i < count; i++) {
            int32_t nx = x_ + packets[i].dx;
            /*  PS/2 reports positive dy as upwards; the screen grows down.  */
            int32_t ny = y_ - packets[i].dy;

            if (nx < 0)
                nx = 0;
            if (nx >= width_)
                nx = width_ - 1;
            if (ny < 0)
                ny = 0;
            if (ny >= height_)
                ny = height_ - 1;

            if (nx != x_ || ny != y_)
                changed = true;

            x_ = nx;
            y_ = ny;

            uint8_t buttons = (uint8_t)(packets[i].buttons & 0x07);
            if (buttons != buttons_)
                changed = true;
            buttons_ = buttons;
        }

        if (count < 5)
            break;
    }

    return changed;
}

void Mouse::warp(int32_t x, int32_t y) noexcept {
    x_ = x < 0 ? 0 : (x >= width_ ? width_ - 1 : x);
    y_ = y < 0 ? 0 : (y >= height_ ? height_ - 1 : y);
}

} // namespace r2::input
