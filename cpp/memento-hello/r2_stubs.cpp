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
//  The process gets one private 2 MiB frame at 0x600_000 for everything — code,
//  data, the stack from _crt0.o and this arena — so the arena is what is left
//  after the rest. The window manager's own frame buffer and the VGA plane
//  staging take 380 KiB of .bss between them, so this is 768 KiB: enough for
//  the desktop's full-screen bitmap (244 KiB at one byte per pixel) plus the
//  four or five windows over it.
//
R2_HEAP_ARENA(1024 * 1024)

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
