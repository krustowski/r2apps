/*
 *  io.cpp — the console writer.
 *
 *  Syscall 0x10 takes a pointer and a length and prints bytes until it reaches
 *  the length or hits a NUL.  The pointer has to be inside 0x600_000-0xA00_000
 *  or the call is rejected, so everything goes through this object's own
 *  buffer, which is .bss and therefore always in range --- text that a program
 *  parked in the kernel heap (r2::heap::kernel_allocate) would otherwise
 *  silently fail to print.
 */

#include "r2/io.hpp"
#include "r2/libc.hpp"

namespace r2 {

Writer out;

void Writer::flush() {
    if (len_ == 0)
        return;

    buf_[len_] = '\0';
    raw_syscall(Sys::PrintString, (int64_t)buf_, (int64_t)len_);
    len_ = 0;
}

void Writer::put(char c) {
    /*  One byte short of the buffer: flush() needs room for the NUL.  */
    if (len_ + 1 >= BUFFER_SIZE)
        flush();

    buf_[len_++] = c;

    if (c == '\n')
        flush();
}

void Writer::write(string_view text) {
    const char *p = text.data();
    size_t remaining = text.size();

    while (remaining > 0) {
        if (len_ + 1 >= BUFFER_SIZE)
            flush();

        size_t room = BUFFER_SIZE - 1 - len_;
        size_t chunk = remaining < room ? remaining : room;

        memcpy(buf_ + len_, p, chunk);

        /*  Flush on the last newline in the chunk so that line-oriented output
         *  appears as it is produced rather than when the buffer happens to
         *  fill.  */
        bool has_newline = memchr(buf_ + len_, '\n', chunk) != nullptr;

        len_ += chunk;
        p += chunk;
        remaining -= chunk;

        if (has_newline)
            flush();
    }
}

void write_console(string_view text) {
    /*  Unbuffered, but still staged: the caller's pointer may be anywhere.  */
    static char staging[256];

    const char *p = text.data();
    size_t remaining = text.size();

    while (remaining > 0) {
        size_t chunk = remaining < sizeof(staging) - 1 ? remaining : sizeof(staging) - 1;
        memcpy(staging, p, chunk);
        staging[chunk] = '\0';

        raw_syscall(Sys::PrintString, (int64_t)staging, (int64_t)chunk);

        p += chunk;
        remaining -= chunk;
    }
}

} // namespace r2
