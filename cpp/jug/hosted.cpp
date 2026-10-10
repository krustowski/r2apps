//
//  hosted.cpp --- jug.elf --host 0x<address>: the contents of Memento's Jug
//  window, drawn here and shown there.
//
//  The plumbing is r2web's (../r2web/main.cpp), on the same block layout:
//  Memento queues the window's input, this process draws into an offscreen
//  surface with Memento's own renderer and publishes the frame, and each side
//  beats so the other can tell when it is gone.
//

#include <r2/heap.hpp>
#include <r2/io.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>

#include "../memento-hello/web/wbase.h"
#include "app.h"
#include "fetch.h"
#include "host.h"
#include "jug.h"

static r2web::HostBlock *g_host;
#include "../r2web/offscreen.h"

#include "ui/platform/PlatformColor.h"
#include "ui/platform/PlatformFont.h"
#include "ui/platform/PlatformKey.h"

using namespace Memento;

//  Ctrl+C in the window: onto Memento's clipboard.
static void clipboardSet(const char *s)
{
    if (!r2web::load(&g_host->copyPending))
    {
        web::scopy(g_host->copyText, s, sizeof(g_host->copyText));
        r2web::store(&g_host->copyPending, 1);
    }
}

//  Fresh builds: Memento shows their count over the taskbar clock and marks
//  the window (red title and taskbar button) unless it has the focus.
static void notifyUpdates(uint32_t count)
{
    r2web::store(&g_host->attentionPending, jughost::UpdateNotification | count);
}

#include "window.cpp"

namespace {

bool command(r2web::Command &c)
{
    uint32_t tail = r2web::load(&g_host->tail), head = r2web::load(&g_host->head);
    if (head - tail > r2web::Queue)
    {
        web::scopy(g_host->error, "Invalid input queue.", sizeof(g_host->error));
        r2web::store(&g_host->quit, 1);
        return false;
    }
    if (head == tail)
        return false;
    c = g_host->commands[tail % r2web::Queue];
    c.text[sizeof(c.text) - 1] = 0;
    r2web::store(&g_host->tail, tail + 1);
    return true;
}

void input(BrowserSurface *surface, const r2web::Command &c)
{
    PlatformWindowInterfaceInputEventLow e{};
    PlatformKey key{};
    switch (c.op)
    {
    case r2web::Resize:
        if (r2web::dimensions(g_host, c.x, c.y))
            surface->geometry(c.x, c.y, false);
        return;
    case r2web::Key:
#define DECODE_FLAG(name, bit) key.name = (c.flags & (1u << bit)) != 0;
        R2WEB_KEY_FLAGS(DECODE_FLAG)
#undef DECODE_FLAG
        key.theChar = (char)c.value;
        key.f = (int8)c.extra;
        e.type = PlatformWindowInputEventType::OnKeyEvent;
        e.Data.OnKeyEvent.key = &key;
        break;
    case r2web::MouseMove:
        e.type = PlatformWindowInputEventType::OnMouseMove;
        e.Data.OnMouseMove.mouseX = Dim(c.x);
        e.Data.OnMouseMove.mouseY = Dim(c.y);
        break;
    case r2web::MouseButton:
        e.type = PlatformWindowInputEventType::OnMouseClick;
        e.Data.OnMouseClick.mouseX = Dim(c.x);
        e.Data.OnMouseClick.mouseY = Dim(c.y);
        e.Data.OnMouseClick.button = c.value ? PlatformWindowMouseButton::Right : PlatformWindowMouseButton::Left;
        e.Data.OnMouseClick.state = c.extra ? PlatformWindowButtonState::Pressed : PlatformWindowButtonState::Released;
        break;
    case r2web::Wheel:
        e.type = PlatformWindowInputEventType::OnMouseWheel;
        e.Data.OnMouseWheel.up = c.value != 0;
        break;
    default:
        return;
    }
    surface->input(e);
}

bool publish(BrowserSurface *surface)
{
    uint32_t front = r2web::load(&g_host->front);
    uint32_t back = front == 0 ? 1 : 0;
    auto &f = g_host->frames[back];
    if (!r2web::claim(&f.state, r2web::Writing))
        return false;
    auto *bitmap = surface->paint();
    if (bitmap && bitmap->GetPixels())
    {
        uint32_t w = surface->width, h = surface->height;
        uint32_t stride = bitmap->GetRealWidth().intValue();
        if (r2web::dimensions(g_host, w, h))
        {
            for (uint32_t y = 0; y < h; ++y)
                memcpy(r2web::pixels(g_host, back) + size_t(y) * w, bitmap->GetPixels() + size_t(y) * stride, w);
            f.width = w;
            f.height = h;
            web::scopy(f.title, surface->title, sizeof(f.title));
            f.serial = r2web::load(&g_host->frame) + 1;
            r2web::store(&f.state, r2web::Ready);
            r2web::store(&g_host->front, back);
            r2web::store(&g_host->frame, f.serial);
            return true;
        }
    }
    r2web::store(&f.state, r2web::Ready);
    return false;
}

//  The block Memento made, from the address on the command line, once it is
//  plainly one: on the user heap (never Memento's private image, which is
//  another frame in this process), and saying what a jug block says.
r2web::HostBlock *attach(r2::string_view arg)
{
    uintptr_t p = 0;
    size_t i = arg.size() > 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X') ? 2 : 0;
    if (i == arg.size() || arg.size() - i > 16)
        return nullptr;
    for (; i < arg.size(); ++i)
    {
        char c = arg[i];
        unsigned n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (n == 16)
            return nullptr;
        p = (p << 4) | n;
    }
    if (p % alignof(r2web::HostBlock) || p < 0xc00000 || p >= 0x40000000 ||
        sizeof(r2web::HostBlock) > 0x40000000 - p)
        return nullptr;
    auto *b = (r2web::HostBlock *)p;
    if (b->magic != jughost::Magic || b->version != r2web::Version || !b->maxWidth || !b->maxHeight ||
        b->maxWidth > r2web::MaxDimension || b->maxHeight > r2web::MaxDimension ||
        b->capacity != b->maxWidth * b->maxHeight || r2web::blockSize(b->capacity) > 0x40000000 - p ||
        (b->colours != 16 && b->colours != 256) || b->portBase != jughost::PortBase)
        return nullptr;
    return b;
}

} // namespace

namespace jug {

int hosted(r2::string_view block)
{
    if (!(g_host = attach(block)))
    {
        r2::print("jug: --host is for Memento's Jug window; `jug help` for the rest\n");
        return 1;
    }
    use_ports((uint16_t)g_host->portBase);
    MementoR2Impl::R2_Palette::SetCount(g_host->colours);
    {
        JugWindow window;
        BrowserRoot root;
        PlatformUIRoot::PlatformWindowOptions options{};
        options.useCustomDPI = true;
        options.customDPI = 192;
        int w = g_host->maxWidth < 600 ? g_host->maxWidth : 600;
        int h = g_host->maxHeight < 340 ? g_host->maxHeight : 340;
        auto *surface = static_cast<BrowserSurface *>(
            root.CreateWindow("Jug", Coord(w / 2.0), Coord(h / 2.0), JugWindow::onEvent, &window, &options, nullptr, nullptr));
        if (!surface)
            web::scopy(g_host->error, "No memory for the Jug window.", sizeof(g_host->error));
        else
        {
            surface->geometry(w, h, true);
            window.SetWindow(surface);
            uint32_t heard = r2web::load(&g_host->hostBeat);
            uint64_t heardAt = r2::ticks();
            while (!r2web::load(&g_host->quit) && !surface->closed)
            {
                r2web::store(&g_host->clientBeat, r2web::load(&g_host->clientBeat) + 1);
                uint32_t beat = r2web::load(&g_host->hostBeat);
                if (beat != heard)
                {
                    heard = beat;
                    heardAt = r2::ticks();
                }
                else if (r2::ticks() - heardAt > 10000)
                {
                    web::scopy(g_host->error, "Memento stopped answering.", sizeof(g_host->error));
                    break;
                }
                r2web::Command c{};
                for (int i = 0; i < 32 && command(c); ++i)
                    input(surface, c);
                if (surface->immediate)
                {
                    PlatformWindowInterfaceInputEventLow e{};
                    e.type = PlatformWindowInputEventType::OnImmediateModeIdleLoop;
                    surface->input(e);
                }
                if (surface->dirty)
                    (void)publish(surface);
                r2::sleep(surface->immediate ? 1 : 5);
            }
        }
        linger(300);
    }
    //  Last touch of the block: Memento may free it now.
    r2web::store(&g_host->exited, 1);
    return 0;
}

} // namespace jug
