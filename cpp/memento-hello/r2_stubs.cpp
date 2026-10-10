//
//  r2_stubs.cpp — what this program has to supply that libc++r2 does not.
//
//  There used to be a great deal here: malloc and free over a bump heap,
//  memset, memmove, the string functions, round, the C++ ABI stubs, the
//  _Unwind_* placeholders libstdc++ referenced but never called.  All of that
//  now comes from libc++r2 and libc++r2compat (see the library's README, "Using
//  a host libstdc++"), which is where the stubs were collected from in the
//  first place.  What is left is the arena size and the two hooks Memento's
//  platform layer expects the application to answer.
//

#include <r2/heap.hpp>
#include <r2/types.hpp>

//
//  The heap.
//
//  640 KiB in .bss, growing onto the kernel's user heap (0xC00_000 up) in
//  256 KiB pieces when that is full.  The desktop's full-screen bitmap is 244
//  KiB at one byte per pixel and every window is a bitmap of its own, so a few
//  windows fit and more grow the arena rather than fail to open.
//
//  Not the user heap from the start: the Web window keeps a page there --- the
//  body, its text and its layout come to about four times a page's size --- and
//  the heap is 4 MiB for every process together.  With a 1 MiB arena sitting in
//  it, a 700 KiB page no longer fitted.  The image has the room since the arena
//  left it once (the private 2 MiB frame has to hold code, data and stack).
//  It was 768 KiB until the H.264 decoder for Telegram's GIFs (web/mp4.cpp,
//  about 80 KiB of code) needed the room; Telegram has since moved out to
//  go/telegram, and Memento's text with it is about 150 KiB smaller.
//
R2_HEAP_ARENA_GROWING(640 * 1024)

//
//  Memento platform hooks.
//
//  Font discovery does not exist on r2: R2_FontImpl carries an embedded
//  Terminus and ignores the path entirely.  These return non-null values so
//  PlatformFontManager::HasError() is satisfied.
//
namespace Memento
{
    using mchar = char;
    using int32 = int;
    using uint32 = unsigned int;

    static const mchar s_r2FontName[] = "r2font";
    static const mchar s_r2FontPath[] = "/r2font";

    const mchar *getSystemFontName(int32 *sz)
    {
        if (sz)
            *sz = 16;
        return s_r2FontName;
    }

    bool findFond(const mchar *, mchar *buf, uint32 len)
    {
        if (buf && len > sizeof(s_r2FontPath))
        {
            for (uint32 i = 0; i < sizeof(s_r2FontPath); i++)
                buf[i] = s_r2FontPath[i];
        }
        return true;
    }
}
