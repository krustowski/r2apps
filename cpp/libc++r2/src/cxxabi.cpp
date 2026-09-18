/*
 *  cxxabi.cpp — the Itanium C++ ABI entry points the compiler emits calls to.
 *
 *  None of this is code an application calls directly.  It is what the
 *  compiler expects to find at link time: the hook for a pure virtual call,
 *  the guards around a function-local static, the registration of destructors
 *  for objects with static storage duration, and the stack-protector handler.
 *
 *  The guards are the single-threaded versions.  The Makefile builds with
 *  -fno-threadsafe-statics so that most programs never reach them, but a
 *  translation unit compiled without that flag still links.
 *
 *  The unwinder stubs at the bottom exist so that a program which links a host
 *  libstdc++ (as cpp/memento-hello does) resolves, and so that an accidental
 *  throw halts with a message rather than jumping into nothing.
 */

#include "r2/io.hpp"
#include "r2/process.hpp"

namespace {

/*
 *  Destructors registered by __cxa_atexit, run in reverse at exit.  Thirty-two
 *  is generous for a program in a 2 MiB frame; registration past that is
 *  dropped, and the destructor simply does not run at exit.
 */
constexpr int MAX_EXIT_ENTRIES = 32;

struct ExitEntry {
    void (*fn)(void *);
    void *arg;
};

ExitEntry g_exit_entries[MAX_EXIT_ENTRIES];
int g_exit_count = 0;

} // namespace

/*  The address of this object identifies this program to __cxa_atexit.  */
extern "C" {
void *__dso_handle = &__dso_handle;
}

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *) {
    if (g_exit_count >= MAX_EXIT_ENTRIES)
        return -1;
    g_exit_entries[g_exit_count].fn = fn;
    g_exit_entries[g_exit_count].arg = arg;
    g_exit_count++;
    return 0;
}

/*  Runs the registered destructors.  Called by r2::exit(); also the symbol a
 *  host libstdc++ expects.  */
extern "C" int __cxa_finalize(void *) {
    while (g_exit_count > 0) {
        g_exit_count--;
        ExitEntry entry = g_exit_entries[g_exit_count];
        if (entry.fn)
            entry.fn(entry.arg);
    }
    return 0;
}

extern "C" void __r2_run_static_destructors() { __cxa_finalize(nullptr); }

extern "C" void __cxa_pure_virtual() {
    r2::panic(r2::string_view("pure virtual function called"));
}

extern "C" void __cxa_deleted_virtual() {
    r2::panic(r2::string_view("deleted virtual function called"));
}

/*
 *  Function-local statics.  The guard byte is 0 before construction and 1
 *  after; with one thread there is no contention to arbitrate, and a non-zero
 *  guard simply means the object is already built.
 */
extern "C" int __cxa_guard_acquire(uint64_t *guard) {
    return *(char *)guard == 0 ? 1 : 0;
}

extern "C" void __cxa_guard_release(uint64_t *guard) { *(char *)guard = 1; }

extern "C" void __cxa_guard_abort(uint64_t *) {}

/*
 *  Exceptions are disabled, but libstdc++ and any translation unit built
 *  without -fno-exceptions still reference these.  Reaching one means the
 *  program tried to throw, which it cannot recover from here.
 */
extern "C" void __cxa_throw(void *, void *, void (*)(void *)) {
    r2::panic(r2::string_view("exception thrown; libc++r2 is built without exceptions"));
}

extern "C" void *__cxa_allocate_exception(size_t) {
    r2::panic(r2::string_view("exception allocated; libc++r2 is built without exceptions"));
}

extern "C" void __cxa_rethrow() { r2::panic(r2::string_view("exception rethrown")); }

extern "C" void __cxa_begin_catch(void *) {}
extern "C" void __cxa_end_catch() {}

extern "C" void _ZSt9terminatev() { r2::panic(r2::string_view("std::terminate")); }

extern "C" void abort() { r2::panic(r2::string_view("abort")); }

/*
 *  Stack protector.  The Makefile builds with -fno-stack-protector, but Fedora
 *  and other distributions turn it on by default, so an application built
 *  without that flag would otherwise fail to link.
 */
extern "C" {
uintptr_t __stack_chk_guard = 0x00000aff'deadbeefULL;

void __stack_chk_fail() { r2::panic(r2::string_view("stack smashing detected")); }
}
