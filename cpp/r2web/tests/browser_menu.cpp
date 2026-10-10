// Test the browser's actual DOM rebuild with a host window and no network.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#define MEMENTO_BACKEND_WEB
#define sprintf_s std::sprintf
#include "ui/platform/PlatformWindow.h"
#include "ui/platform/PlatformBitmap.h"
#include "ui/platform/PlatformDrawingContext.h"
#include "ui/platform/PlatformFont.h"
#include "ui/platform/PlatformColor.h"
#include "ui/platform/PlatformKey.h"
#include "../host.h"
#include "../script.h"
#include "../../memento-hello/web/image.h"
using namespace Memento;

namespace r2 {
struct byte_span { byte_span(uint8_t *, size_t) {} };
namespace fs {
static std::optional<uint32_t> size_of(const char *) { return {}; }
static int64_t read_at(const char *, byte_span, uint64_t) { return -1; }
}
}
static r2web::HostBlock *g_host;
static const char *clipboardGet() { return "clipboard"; }
static void clipboardSet(const char *) {}
static bool openBrowserWindow(const char *) { return false; }
#define private public
#ifndef BROWSER_SOURCE
#define BROWSER_SOURCE "../browser.cpp"
#endif
#include BROWSER_SOURCE
#undef private
namespace Memento {
PlatformWindowInterface::DataT::DataT() {}
PlatformWindowInterface::DataT::~DataT() {}
PlatformWindowInterface::PlatformWindowInterface() {}
PlatformWindowInterface::~PlatformWindowInterface() {}
const mchar *stripProjectPath(const mchar *s) { return s; }
void log_error(mchar *, const mchar *, uint32) { std::abort(); }
LinkedListItem *PlatformLinkedItemConvert::Convert(PlatformDrawingContext *s) { return s; }
PlatformDrawingContext *PlatformLinkedItemConvert::ConvertBack(LinkedListItem *s, PlatformDrawingContext *)
{
    return static_cast<PlatformDrawingContext *>(s);
}
PlatformWindow::PlatformWindow(PlatformWindowConstrData *) {}
PlatformWindow::~PlatformWindow() {}
void PlatformWindow::Repaint() {}
bool PlatformWindow::SetTitle(const mchar *) { return true; }
}
struct Window : PlatformWindow {
    Window() : PlatformWindow(nullptr) {}
    bool doEvent(PlatformWindowInterface *) override { return true; }
};
namespace web {
void *alloc(size_t n) { return std::malloc(n); }
void *realloc(void *p, size_t n) { return std::realloc(p, n); }
void free(void *p) { std::free(p); }
void *big_alloc(size_t n) { return std::malloc(n); }
void *big_realloc(void *p, size_t n) { return std::realloc(p, n); }
void big_free(void *p) { std::free(p); }
uint64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct NoNet : NetIf {
    void poll() override {}
    int resolve(const char *, uint8_t[4]) override { return -1; }
    int connect(const uint8_t[4], uint16_t) override { return -1; }
    int status(int) override { return FAILED; }
    size_t send(int, const uint8_t *, size_t) override { return 0; }
    size_t recv(int, uint8_t *, size_t) override { return 0; }
    void close(int) override {}
    const char *lastError() override { return "offline"; }
};
NetIf &r2Net()
{
    static NoNet net;
    return net;
}
void gatherEntropy(uint8_t *, size_t) {}
void currentTime(unsigned long *, unsigned long *) {}
Loader::Loader(NetIf &net, SeedFn seed, TimeFn time) : net_(net), seed_(seed), time_(time) {}
Loader::~Loader() {}
void Loader::start(const Url &, bool, const uint8_t *, size_t, const char *, const char *, const char *)
{
    std::abort(); // No test should start a network request.
}
void Picture::release()
{
    big_free(px);
    px = nullptr;
}
const char *decodePicture(const uint8_t *, size_t, int, int, int, uint32_t, Picture &) { return "offline"; }
}
static int failures;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void refresh(BrowserWindow &b, const char *js)
{
    CHECK(b.script.eval(js));
    CHECK(b.script.render(b.pageBody, nullptr, 0));
    b.renderScriptPage();
}

static int link(BrowserWindow &b, const char *href)
{
    for (int i = 0; i < b.doc.linkCount(); ++i)
        if (!std::strcmp(b.doc.linkHref(i), href))
            return i;
    return -1;
}

int main()
{
    Window window;
    BrowserWindow b;
    b.wnd = &window;
    b.imagesOn = false;
    web::Buf html{true};
    html.appendStr("<p id=clock>0</p><a id=target href='/target'>Target</a><input id=field><script></script>");
    CHECK(b.script.start(html, "utf-8", "http://localhost/ui"));
    b.script.parsed();
    CHECK(b.script.render(b.pageBody, nullptr, 0));
    b.renderScriptPage();

    // A keyboard-opened page menu keeps its selection and position while a
    // clock (like GARN's) keeps changing the page behind it.
    b.openMenu(-1, 12, 34, true);
    b.moveMenuSel(+1);
    int sel = b.menuSel, len = b.menuLen;
    b.scrollRow = 123;
    for (int i = 0; i < 10; ++i)
    {
        refresh(b, "document.getElementById('clock').textContent += 'x'");
        CHECK(b.menuOpen && b.menuLink == -1);
        CHECK(b.menuSel == sel && b.menuLen == len && b.menuX == 12 && b.menuY == 34);
        CHECK(b.scrollRow == 123);
    }

    // Inserting a link changes native indexes. Actions must still refer to
    // the node on which the menu was opened.
    int target = link(b, "/target");
    CHECK(target >= 0);
    b.openMenu(target, 23, 45, true);
    int node = web::ScriptPage::nodeOf(b.doc.linkHandler(target));
    refresh(b, "document.body.insertAdjacentHTML('afterbegin','<a href=/other>Other</a>')");
    CHECK(b.menuOpen && b.menuLink != target);
    CHECK(web::ScriptPage::nodeOf(b.doc.linkHandler(b.menuLink)) == node);
    CHECK(b.menuSel == 0 && b.menuX == 23 && b.menuY == 45);
    refresh(b, "document.getElementById('target').remove()");
    CHECK(!b.menuOpen);

    // The same remapping is needed for a text field's Paste action.
    int field = -1;
    for (int i = 0; i < b.doc.linkCount(); ++i)
        if (b.doc.linkControl(i) >= 0)
            field = i;
    CHECK(field >= 0);
    b.openMenu(field, 56, 78, true);
    node = web::ScriptPage::nodeOf(b.doc.linkHandler(field));
    refresh(b, "document.body.insertAdjacentHTML('afterbegin','<input id=another>')");
    CHECK(b.menuOpen && b.menuLink != field);
    CHECK(web::ScriptPage::nodeOf(b.doc.linkHandler(b.menuLink)) == node);
    CHECK(b.menu[0].action == BrowserWindow::A_PASTE_FIELD);

    b.closeMenu();
    refresh(b, "document.getElementById('clock').textContent='done'");
    CHECK(!b.menuOpen);
    b.openMenu(-1, 1, 2, true);
    b.showDocument("about:home", false, 0);
    CHECK(!b.menuOpen);
    if (!failures)
        std::puts("r2web: context menus survive DOM refreshes and retain their targets; removal, dismissal and navigation close them");
    return failures ? 1 : 0;
}
