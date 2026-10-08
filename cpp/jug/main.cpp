//
//  main.cpp --- jug: `jug <command>` on the console, or `jug.elf --host` in
//  Memento's Jug window.
//

#include <r2/heap.hpp>
#include <r2/libc.hpp>
#include <r2/process.hpp>

#include "app.h"

//  On the kernel's user heap, growing as it needs: the window's surface and
//  the TLS engine come and go, and the image keeps its 2 MiB for code.  The
//  downloads themselves are on the user heap too (web_r2.cpp's big pool).
R2_HEAP_ARENA_KERNEL(512 * 1024)

//  What Memento's font code asks the platform (see ../r2web/runtime.cpp).
namespace Memento {
const char *getSystemFontName(int *size)
{
    if (size)
        *size = 16;
    return "r2font";
}
bool findFond(const char *, char *path, unsigned len)
{
    if (path && len > 8)
        strcpy(path, "/r2font");
    return true;
}
} // namespace Memento

extern "C" int main()
{
    if (r2::arg_count() == 3 && r2::arg(1) == r2::string_view("--host"))
        return jug::hosted(r2::arg(2));
    return jug::cli();
}
