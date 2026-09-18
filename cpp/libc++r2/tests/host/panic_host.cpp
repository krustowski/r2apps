/*
 *  panic_host.cpp — panic() for the host test binaries.
 *
 *  The real one (start.cpp) prints through the kernel and makes the exit
 *  syscall, neither of which exists on the host.  The host tests link this
 *  instead so that a panic shows up as a message and a non-zero exit status
 *  rather than an illegal instruction.
 *
 *  It lives here, beside the tests, rather than in src/ --- the library
 *  Makefile compiles everything in src/ into libc++r2.a, and this definition
 *  must not be in there competing with the real one in start.cpp.
 */

#include "r2/panic.hpp"

extern "C" int printf(const char *format, ...);
extern "C" [[noreturn]] void _exit(int code);

namespace r2 {

void panic(string_view message) {
    printf("\npanic: %.*s\n", (int)message.size(), message.data());
    _exit(1);
}

void panic_at(string_view message, source_location where) {
    printf("\npanic: %.*s (%s:%u)\n", (int)message.size(), message.data(), where.file_name(),
           (unsigned)where.line());
    _exit(1);
}

} // namespace r2
