/*
 *  start.cpp — what runs between _start and main.
 *
 *  _crt0.asm has switched to the private stack and passed the argv frame on;
 *  the job here is to bring the heap up, run the constructors of objects with
 *  static storage duration, call main, and then unwind all of that in reverse.
 *
 *  Order matters in both directions.  The heap comes first because a global
 *  constructor may well allocate.  On the way out the console is flushed
 *  before any destructor runs, so that a message printed by main is not lost
 *  behind a destructor that faults.
 */

#include "r2/heap.hpp"
#include "r2/io.hpp"
#include "r2/process.hpp"
#include "r2/syscall.hpp"

extern "C" {

/*  Bracketed by the linker script; see linker.ld.  */
extern void (*__init_array_start[])(int, char **, char **);
extern void (*__init_array_end[])(int, char **, char **);
extern void (*__fini_array_start[])();
extern void (*__fini_array_end[])();

/*  The program's entry point.  Declared with the full signature: a program
 *  that defines `int main()` still links, because main is never mangled.  */
int main(int argc, char **argv);

void __r2_run_static_destructors();

} // extern "C"

namespace {

constexpr int MAX_ATEXIT = 32;

int g_argc = 0;
char **g_argv = nullptr;

void (*g_atexit[MAX_ATEXIT])() = {};
int g_atexit_count = 0;

bool g_exiting = false;

[[noreturn]] void leave(int code) {
    /*  arg1 is the pid the kernel is being told about, and 0 means "me" ---
     *  the same thing _crt0.asm does on the fallback path.  */
    r2::raw_syscall(r2::Sys::Exit, 0, code);
    for (;;) {
    }
}

} // namespace

extern "C" [[noreturn]] void __r2_start(long argc, char **argv) {
    g_argc = (int)argc;
    g_argv = argv;

    r2::heap::init();

    size_t init_count = (size_t)(__init_array_end - __init_array_start);
    for (size_t i = 0; i < init_count; i++)
        __init_array_start[i]((int)argc, argv, nullptr);

    int code = main((int)argc, argv);

    r2::exit(code);
}

namespace r2 {

int arg_count() noexcept { return g_argc; }

string_view arg(int index) noexcept {
    if (index < 0 || index >= g_argc || !g_argv || !g_argv[index])
        return string_view();
    return string_view(g_argv[index]);
}

bool at_exit(void (*fn)()) {
    if (!fn || g_atexit_count >= MAX_ATEXIT)
        return false;
    g_atexit[g_atexit_count++] = fn;
    return true;
}

void exit(int code) {
    /*  A destructor that itself calls exit() must not start the whole sequence
     *  again.  */
    if (g_exiting)
        leave(code);
    g_exiting = true;

    out.flush();

    while (g_atexit_count > 0) {
        void (*fn)() = g_atexit[--g_atexit_count];
        if (fn)
            fn();
    }

    __r2_run_static_destructors();

    size_t fini_count = (size_t)(__fini_array_end - __fini_array_start);
    for (size_t i = fini_count; i > 0; i--)
        __fini_array_start[i - 1]();

    out.flush();
    leave(code);
}

void quick_exit(int code) { leave(code); }

void panic(string_view message) {
    /*  Unbuffered, and not through anything that allocates: whatever went
     *  wrong may well be the heap.  */
    write_console(string_view("\npanic: "));
    write_console(message);
    write_console(string_view("\n"));
    leave(1);
}

void panic_at(string_view message, source_location where) {
    write_console(string_view("\npanic: "));
    write_console(message);
    write_console(string_view(" ["));
    write_console(string_view(where.file_name()));
    write_console(string_view(":"));

    /*  Formatting the line number by hand, because format() allocates and the
     *  heap is one of the things that might have gone wrong.  */
    char digits[12];
    size_t count = 0;
    uint32_t line = where.line();
    do {
        digits[count++] = (char)('0' + line % 10);
        line /= 10;
    } while (line != 0 && count < sizeof(digits));
    while (count > 0)
        write_console(string_view(&digits[--count], 1));

    write_console(string_view("]\n"));
    leave(1);
}

} // namespace r2
