#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/process.hpp>
#include <r2/io.hpp>
#include <r2/time.hpp>
#include "../memento-hello/web/wbase.h"
#include "host.h"
static r2web::HostBlock *g_host;
#include "offscreen.h"
#include "ui/platform/PlatformKey.h"
#include "ui/platform/PlatformFont.h"
#include "ui/platform/PlatformColor.h"

using namespace Memento;
static char g_clipboard[r2web::TextCapacity];
static const char *clipboardGet() { return g_clipboard; }
static void clipboardSet(const char *s)
{
    web::scopy(g_clipboard, s, sizeof(g_clipboard));
    if (!r2web::load(&g_host->copyPending)) {
        web::scopy(g_host->copyText, s, sizeof(g_host->copyText));
        r2web::store(&g_host->copyPending, 1);
    }
}
static bool openBrowserWindow(const char *url)
{
    if (r2web::load(&g_host->openPending)) return false;
    web::scopy(g_host->openUrl, url, sizeof(g_host->openUrl));
    r2web::store(&g_host->openPending, 1); return true;
}
#include "browser.cpp"

static bool command(r2web::Command &c)
{
    uint32_t tail = r2web::load(&g_host->tail), head = r2web::load(&g_host->head);
    if (head-tail > r2web::Queue) {
        web::scopy(g_host->error, "Invalid browser input queue.", sizeof(g_host->error));
        r2web::store(&g_host->quit, 1); return false;
    }
    if (head == tail) return false;
    c = g_host->commands[tail % r2web::Queue];
    c.text[sizeof(c.text)-1] = 0;
    r2web::store(&g_host->tail, tail+1); return true;
}
static void input(BrowserSurface *surface, const r2web::Command &c)
{
    PlatformWindowInterfaceInputEventLow e{};
    PlatformKey key{};
    switch (c.op) {
    case r2web::Resize:
        if (r2web::dimensions(g_host, c.x, c.y)) surface->geometry(c.x, c.y, false);
        return;
    case r2web::Key:
#define DECODE_FLAG(name, bit) key.name = (c.flags & (1u << bit)) != 0;
        R2WEB_KEY_FLAGS(DECODE_FLAG)
#undef DECODE_FLAG
        key.theChar = (char)c.value; key.f = (int8)c.extra;
        if (key.isChar && (key.theChar == 'v' || key.theChar == 'V') && (key.isLeftControl || key.isRightControl))
            web::scopy(g_clipboard, c.text, sizeof(g_clipboard));
        e.type = PlatformWindowInputEventType::OnKeyEvent;
        e.Data.OnKeyEvent.key = &key; break;
    case r2web::MouseMove:
        e.type = PlatformWindowInputEventType::OnMouseMove;
        e.Data.OnMouseMove.mouseX = Dim(c.x); e.Data.OnMouseMove.mouseY = Dim(c.y); break;
    case r2web::MouseButton:
        e.type = PlatformWindowInputEventType::OnMouseClick;
        e.Data.OnMouseClick.mouseX = Dim(c.x); e.Data.OnMouseClick.mouseY = Dim(c.y);
        e.Data.OnMouseClick.button = c.value ? PlatformWindowMouseButton::Right : PlatformWindowMouseButton::Left;
        e.Data.OnMouseClick.state = c.extra ? PlatformWindowButtonState::Pressed : PlatformWindowButtonState::Released; break;
    case r2web::Wheel:
        e.type = PlatformWindowInputEventType::OnMouseWheel;
        e.Data.OnMouseWheel.up = c.value != 0; break;
    default: return;
    }
    surface->input(e);
}
static bool publish(BrowserSurface *surface)
{
    uint32_t front = r2web::load(&g_host->front);
    uint32_t back = front == 0 ? 1 : 0;
    auto &f = g_host->frames[back];
    if (!r2web::claim(&f.state, r2web::Writing)) return false;
    auto *bitmap = surface->paint();
    if (bitmap && bitmap->GetPixels()) {
        uint32_t w = surface->width, h = surface->height;
        uint32_t stride = bitmap->GetRealWidth().intValue();
        if (r2web::dimensions(g_host, w, h)) {
            for (uint32_t y = 0; y < h; ++y)
                memcpy(r2web::pixels(g_host, back)+size_t(y)*w, bitmap->GetPixels()+size_t(y)*stride, w);
            f.width = w; f.height = h;
            web::scopy(f.title, surface->title, sizeof(f.title));
            f.serial = r2web::load(&g_host->frame)+1;
            r2web::store(&f.state, r2web::Ready);
            r2web::store(&g_host->front, back);
            r2web::store(&g_host->frame, f.serial); return true;
        }
    }
    r2web::store(&f.state, r2web::Ready); return false;
}
static r2web::HostBlock *attach(r2::string_view arg)
{
    uintptr_t p = 0;
    size_t i = arg.size() > 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X') ? 2 : 0;
    if (i == arg.size() || arg.size()-i > 16) return nullptr;
    for (; i < arg.size(); ++i) {
        char c = arg[i];
        unsigned n = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
        if (n == 16) return nullptr;
        p = (p << 4) | n;
    }
    // Shared addresses must be in the kernel user heap, never the parent's
    // private image, which means a different frame in this process.
    // The user heap has disjoint regions: the original 4 MiB and a later
    // RAM-backed extension. MemInfo.heap_size sums them, so it must not be
    // treated as a contiguous interval starting at heap_start.
    if (p % alignof(r2web::HostBlock) || p < 0xc00000 || p >= 0x40000000 ||
        sizeof(r2web::HostBlock) > 0x40000000-p) return nullptr;
    auto *b = (r2web::HostBlock *)p;
    if (b->magic != r2web::Magic || b->version != r2web::Version ||
        !b->maxWidth || !b->maxHeight || b->maxWidth > r2web::MaxDimension || b->maxHeight > r2web::MaxDimension ||
        b->capacity != b->maxWidth*b->maxHeight ||
        r2web::blockSize(b->capacity) > 0x40000000-p ||
        (b->colours != 16 && b->colours != 256) || b->portBase < 48000 || b->portBase > 48224) return nullptr;
    b->initialUrl[sizeof(b->initialUrl)-1] = 0;
    return b;
}
extern "C" int main()
{
    if (r2::arg_count() != 3 || r2::arg(1) != r2::string_view("--host") || !(g_host = attach(r2::arg(2)))) {
        r2::print("r2web.elf is launched by Memento's Web window.\n"); return 1;
    }
    web::r2NetSetPortBase((uint16_t)g_host->portBase);
    MementoR2Impl::R2_Palette::SetCount(g_host->colours);
    {
        BrowserWindow browser;
        BrowserRoot root;
        PlatformUIRoot::PlatformWindowOptions options{};
        options.useCustomDPI = true; options.customDPI = 192;
        int w = g_host->maxWidth < 620 ? g_host->maxWidth : 620;
        int h = g_host->maxHeight < 352 ? g_host->maxHeight : 352;
        auto *surface = static_cast<BrowserSurface *>(root.CreateWindow("Web", Coord(w/2.0), Coord(h/2.0), BrowserWindow::onEvent, &browser, &options, nullptr, nullptr));
        if (!surface) {
            web::scopy(g_host->error, "No memory for the browser surface.", sizeof(g_host->error));
        } else {
            surface->geometry(w, h, true);
            browser.SetWindow(surface);
            uint32_t heard = r2web::load(&g_host->hostBeat);
            uint64_t heardAt = r2::ticks();
            while (!r2web::load(&g_host->quit) && !surface->closed) {
                r2web::store(&g_host->clientBeat, r2web::load(&g_host->clientBeat)+1);
                uint32_t beat = r2web::load(&g_host->hostBeat);
                if (beat != heard) { heard = beat; heardAt = r2::ticks(); }
                else if (r2::ticks()-heardAt > 10000) {
                    web::scopy(g_host->error, "Memento heartbeat timed out.", sizeof(g_host->error)); break;
                }
                r2web::Command c{};
                for (int i = 0; i < 32 && command(c); ++i) input(surface, c);
                if (surface->immediate) {
                    PlatformWindowInterfaceInputEventLow e{};
                    e.type = PlatformWindowInputEventType::OnImmediateModeIdleLoop;
                    surface->input(e);
                }
                if (surface->dirty) (void)publish(surface);
                r2::sleep(5);
            }
        }
    }
    // Last access to the block: the host can now reclaim it.
    r2web::store(&g_host->exited, 1);
    return 0;
}
