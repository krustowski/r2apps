#pragma once
#include "../memento-hello/web/doc.h"
#include "../memento-hello/web/loader.h"
#include "jsr2.h"

namespace web {

//
//  A page's scripts: libjsr2's engine (QuickJS, the event loop, the Web
//  APIs) with r2web's DOM, js/dom.js, built from the page.
//
//  The DOM is the page from the moment its scripts start: the browser lays
//  out what render() hands it, HTML in which every link and control carries
//  onclick="r2:N", and passes what the user does to node N back here
//  (click, input, state, submitFrom).  Pages without scripts or inline
//  handlers never start an engine.
//
//  Between the user's actions the page runs on its own: tick() from the
//  browser's idle loop runs its timers, animation frames and network
//  (fetch, XMLHttpRequest, EventSource, and the scripts it adds itself), on
//  connections of its own (ScriptNet in script.cpp) next to the browser's.
//
class ScriptNet;

class ScriptPage
{
public:
    static constexpr size_t HeapLimit = 12u << 20;
    static constexpr size_t PageLimit = 768 * 1024;
    static constexpr size_t RenderLimit = 4 * PageLimit;
    static constexpr int MaxScripts = 64;

    ScriptPage();
    ~ScriptPage();
    ScriptPage(const ScriptPage &) = delete;
    ScriptPage &operator=(const ScriptPage &) = delete;

    void clear();
    //  The DOM from the page (its bytes, in `charset` or sniffed); false when
    //  it has no scripts or handlers, or the engine could not start.
    bool start(const Buf &html, const char *charset, const char *url);
    bool active() const;

    //  The page's own scripts, in document order.
    int count() const { return nScripts_; }
    const char *src(int i) const { return scripts_[i].url; }
    const char *source(int i) { return code_.cstr() + scripts_[i].source; }
    bool isModule(int i) const { return scripts_[i].module; }
    //  Runs script i with this text (its own, or what its src gave).
    bool run(int i, const char *code, size_t len);
    //  Every script of the page ran: DOMContentLoaded, then load.
    void parsed();
    //  A javascript: address.
    bool eval(const char *source);

    //  What the user did to rendered node N, from an "r2:N" handler.
    static int nodeOf(const char *handler);
    bool click(int node);              // true: the browser goes on with the default
    void input(int node, const char *text);
    void controlChanged(int node);
    bool state(int node, bool checked, int selected);
    bool submitFrom(int node);         // Enter in a text field; true: the browser submits
    bool key(const char *keyName);     // true when the page took the key
    void focus(int node);

    //  The loop.
    void tick();
    bool busy() const;
    //  Frames are run at the browser's paint rate (see browser.cpp).
    bool wantsFrame() const;
    void frame(double ms);

    //  The page as HTML when it changed since the last render (or always,
    //  when forced); its title goes to *title.
    bool changed() const;
    bool render(Buf &html, char *title, size_t titleCap, bool force = false);

    //  What the page asked of the browser.  Each is taken once.
    const char *error() const;
    const char *navigation() const { return nextUrl_; }
    bool navigationReplaces() const { return nextReplace_; }
    void clearNavigation() { nextUrl_[0] = 0; }
    bool takeStatus(char *out, size_t cap);
    bool takeUrl(char *out, size_t cap);     // pushState / hash changes
    bool takeOpen(char *out, size_t cap);    // window.open
    int takeHistory();                       // history.go(n) past this page: n, or 0
    struct Submit
    {
        char action[1200];
        bool post;
        Buf data;
    };
    bool takeSubmit(Submit &s);

    //  Legacy viewport setter, for callers measuring in cells.
    void setViewport(int cols, int rows, int cellW, int cellH, bool dark);
    // Pixel geometry is a synchronous snapshot of the same document and
    // layout used to paint. Text mode retains its approximate geometry.
    void setPixelViewport(int width, int height, int cellW, int lineH, int scroll, bool enabled, bool css, bool dark = false);
    void setLayoutSheets(const StyleSheetText *sheets, int count);
    void setLayoutImages(const Document *document);
    size_t heapBytes() const;

private:
    friend struct ScriptNatives;
    struct Script
    {
        uint32_t source;
        char url[1200];
        bool module;
    };
    struct Fetch; // a script the page added, being fetched

    jsr2::Engine engine_;
    ScriptNet *net_ = nullptr;
    JSValue bridge_ = JS_UNDEFINED;
    Script scripts_[MaxScripts];
    int nScripts_ = 0;
    Buf code_{true};
    Buf renderedHtml_{true};
    Fetch *fetches_ = nullptr;
    int nFetches_ = 0;
    bool dirty_ = false;
    char nextUrl_[1200] = {}, newUrl_[1200] = {}, openUrl_[1200] = {}, status_[160] = {};
    bool nextReplace_ = false, haveSubmit_ = false;
    int history_ = 0;
    Submit submit_;
    int vp_[8] = {80, 25, 8, 16, 0, 640, 400, 0};
    char pageUrl_[1200] = {};
    Document geometryDoc_;
    Buf geometryHtml_{true}, geometrySheets_{true};
    bool geometryValid_ = false, pixelMode_ = true, geometryCss_ = true;
    int pixelWidth_ = 640, pixelHeight_ = 400, pixelScroll_ = 0;
    const Document *geometryImages_ = nullptr;
    uint32_t geometryImageHash_ = 0;

    bool callBridge(const char *fn, int argc, JSValueConst *argv, JSValue *result = nullptr);
    void pollFetches();
};

//  The last 4 KiB of what page scripts logged (about:console).
const char *scriptConsole(size_t *len);

} // namespace web
