#pragma once
#include "../memento-hello/web/doc.h"
#include "../third_party/mujs/mujs.h"

namespace web {
// A deliberately small browser API around an ES5 interpreter. Rendering CSS
// or pictures again never executes the page's scripts again.
class ScriptPage {
public:
    static constexpr size_t HeapLimit = 2*1024*1024, PageLimit = 768*1024;
    static constexpr int MaxScripts = 16;
    ScriptPage() = default;
    ~ScriptPage() { clear(); }
    ScriptPage(const ScriptPage &) = delete;
    ScriptPage &operator=(const ScriptPage &) = delete;
    void clear();
    bool start(Buf &html, Document &doc, const char *url);
    int count() const { return nScripts; }
    const char *source(int i) { return code.cstr()+scripts[i].source; }
    const char *src(int i) const { return scripts[i].url; }
    bool eval(const char *source, const char *thisId = nullptr);
    bool click(const char *handler, const char *id);
    bool changed() const { return dirty; }
    void rendered() { dirty = false; }
    const char *error() const { return diagnostic; }
    const char *navigation() const { return nextUrl; }
    void clearNavigation() { nextUrl[0] = 0; }
    size_t heapBytes() const { return used; }
    void poll();
private:
    struct Script { uint32_t source; char url[1200]; } scripts[MaxScripts]{};
    struct Allocation { Allocation *prev, *next; size_t size; };
    struct Node { ScriptPage *page; char id[128]; };
    struct Element { size_t start, tagEnd, innerEnd, end; char tag[24]; bool empty; };
    js_State *J = nullptr;
    Buf *html = nullptr;
    Document *document = nullptr;
    Buf code{true}, scratch{true};
    Allocation *allocations = nullptr;
    size_t used = 0;
    int nScripts = 0;
    bool dirty = false, running = false;
    uint32_t budget = 0;
    uint64_t deadline = 0;
    jmp_buf abortPoint;
    char diagnostic[128]{}, nextUrl[1200]{}, pageUrl[1200]{};
    void destroyVM();
    bool createVM();
    void bind();
    bool find(const char *id, Element &e) const;
    bool replace(size_t first, size_t last, const char *s, size_t len);
    void content(const Element &e, bool text);
    bool attribute(const Element &e, const char *name, Buf &out) const;
    bool setAttribute(const Element &e, const char *name, const char *value);
    void node(const char *id);
    void fail(const char *message);
    static void *allocate(void *, void *, int);
    static ScriptPage *self(js_State *J) { return (ScriptPage *)js_getcontext(J); }
    static int getNode(js_State *, void *, const char *);
    static int putNode(js_State *, void *, const char *);
    static void freeNode(js_State *, void *);
    static void getById(js_State *);
    static void write(js_State *);
    static void log(js_State *);
    static void getTitle(js_State *);
    static void putTitle(js_State *);
    static void getHref(js_State *);
    static void putHref(js_State *);
    static void report(js_State *, const char *);
};
} // namespace web
