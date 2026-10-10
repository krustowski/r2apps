#pragma once
// The browser tests exercise its UI state without drawing frames or calling
// the r2 graphics driver. Keep the real platform interfaces and stub its bitmap.
#include "ui/platform/PlatformBitmap.h"
namespace Memento::MementoR2Impl {
struct R2_Palette { static unsigned Count() { return 16; } };
struct R2_BitmapImpl : PlatformBitmap {
    unsigned char *GetPixels() { return nullptr; }
    Dim GetRealWidth() { return Dim(640); }
    Dim GetRealHeight() { return Dim(480); }
};
}
