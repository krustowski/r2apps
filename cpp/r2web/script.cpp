//
//  script.cpp --- ScriptPage: a page's engine, its DOM and its network.
//  See script.h.
//
//  The DOM (js/dom.js, compiled into obj/gen/dom.c) is a function of two
//  objects: the natives below as `b`, and libjsr2's helpers.  It returns the
//  bridge whose methods this file calls (load, scripts, render, click ...).
//
#include "script.h"
#include "htmlparse.h"
#ifndef WEB_HOST
#include "../memento-hello/web/net_r2.h"
#endif

extern "C" const uint8_t r2web_dom[];
extern "C" const size_t r2web_dom_size;

namespace web {

//  ── The machine, for libjsr2 ────────────────────────────────────────────────

namespace {

//  The last of what pages logged, for whoever wants to look (about:console).
char g_console[4096];
size_t g_consoleLen = 0;

void logLine(int level, const char *text, size_t len)
{
    static const char *const prefix[] = {"", "", "warn: ", "error: ", "debug: "};
    const char *p = level >= 0 && level <= 4 ? prefix[level] : "";
    size_t pl = strlen(p), need = pl + len + 1;
    if (need > sizeof(g_console))
        return;
    if (g_consoleLen + need > sizeof(g_console))
    {
        size_t drop = g_consoleLen + need - sizeof(g_console);
        memmove(g_console, g_console + drop, g_consoleLen - drop);
        g_consoleLen -= drop;
    }
    memcpy(g_console + g_consoleLen, p, pl);
    g_consoleLen += pl;
    memcpy(g_console + g_consoleLen, text, len);
    g_consoleLen += len;
    g_console[g_consoleLen++] = '\n';
}

//  r2's clock reads whole seconds: Date.now() is the RTC at the first call
//  plus the ticks since.
double epochMs()
{
#ifdef WEB_HOST
    return 1.7e12 + (double)now_ms();
#else
    static double base = -1;
    static uint64_t at = 0;
    if (base < 0)
    {
        unsigned long days, seconds;
        currentTime(&days, &seconds);
        base = days ? ((double)days - 719528) * 86400000.0 + seconds * 1000.0 : 0;
        at = now_ms();
    }
    return base ? base + (double)(now_ms() - at) : 0;
#endif
}

void entropy(uint8_t *out, size_t n)
{
#ifdef WEB_HOST
    static uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; i++)
    {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        out[i] = (uint8_t)x;
    }
#else
    gatherEntropy(out, n);
#endif
}

void ensurePlatform()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    jsr2::setPlatform({big_alloc, big_free, now_ms, epochMs, entropy, logLine});
}

//  Does the page need an engine at all?  Scripts, inline handlers or
//  javascript: links say so.
bool needsScripts(const Buf &html)
{
    const char *s = (const char *)html.data;
    size_t n = html.len;
    if (!s)
        return false;
    if (ifind(s, n, "<script") || ifind(s, n, "javascript:"))
        return true;
    for (const char *p = s; (p = ifind(p, n - (size_t)(p - s), " on")); p += 3)
    {
        const char *q = p + 3;
        while (q < s + n && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')))
            q++;
        if (q > p + 4 && q < s + n && *q == '=')
            return true;
    }
    return false;
}

//  Pages keep their localStorage, sessionStorage and cookies for as long as
//  the browser runs, per origin.
struct Stored
{
    char kind[8];
    char origin[128];
    Buf json;
    uint64_t used;
};
Stored g_store[24];
const size_t MAX_STORED = 64 * 1024;

Stored *findStore(const char *kind, const char *origin, bool create)
{
    Stored *oldest = &g_store[0];
    for (Stored &s : g_store)
    {
        if (!strcmp(s.kind, kind) && !strcmp(s.origin, origin))
        {
            s.used = now_ms();
            return &s;
        }
        if (s.used < oldest->used)
            oldest = &s;
    }
    if (!create)
        return nullptr;
    scopy(oldest->kind, kind, sizeof(oldest->kind));
    scopy(oldest->origin, origin, sizeof(oldest->origin));
    oldest->json.release();
    oldest->used = now_ms();
    return oldest;
}

} // namespace

//  ── The network under fetch, XMLHttpRequest and EventSource ─────────────────
//
//  A few Loaders of its own, next to the browser's: the browser keeps eight
//  TCP ports, and its page, pictures and style sheets take one at a time.
//  Requests past the slots wait their turn.  Bodies are handed on as they
//  arrive and taken out of the loader (HttpResponse::consume), so an event
//  stream can run for as long as the page.

#ifndef WEB_HOST
class ScriptNet : public jsr2::Transport
{
public:
    static const int Slots = 3;
    static const int MaxRequests = 24;

    ScriptNet()
    {
        for (int i = 0; i < Slots; i++)
        {
            loaders_[i] = new Loader(r2Net(), gatherEntropy, currentTime);
            owner_[i] = -1;
        }
        for (Req &q : reqs_)
            q.reset();
    }
    ~ScriptNet()
    {
        for (int i = 0; i < Slots; i++)
            delete loaders_[i];
    }

    int open(const Request &r, const char **error) override
    {
        int h = -1;
        for (int i = 0; i < MaxRequests && h < 0; i++)
            if (!reqs_[i].used)
                h = i;
        if (h < 0)
        {
            *error = "too many requests at once";
            return -1;
        }
        Req &q = reqs_[h];
        q.reset();
        if ((!istarts(r.url, "http://") && !istarts(r.url, "https://")) || !urlFromInput(r.url, q.url))
        {
            *error = "only http and https addresses can be fetched";
            return -1;
        }
        q.used = true;
        q.stream = r.stream;
        scopy(q.method, r.method && r.method[0] ? r.method : "GET", sizeof(q.method));
        for (char *p = q.method; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 32);
        //  The content type goes where the loader puts it; the rest of the
        //  page's headers go as they are, less what the loader writes itself.
        for (const char *line = r.headers ? r.headers : ""; *line;)
        {
            const char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line + 1) : strlen(line);
            if (istarts(line, "content-type:"))
            {
                const char *v = line + 13;
                while (*v == ' ')
                    v++;
                size_t vl = (size_t)(line + len - v);
                while (vl && (v[vl - 1] == '\r' || v[vl - 1] == '\n'))
                    vl--;
                scopyn(q.contentType, v, vl, sizeof(q.contentType));
            }
            else if (!istarts(line, "host:") && !istarts(line, "content-length:") && !istarts(line, "connection:") &&
                     !istarts(line, "user-agent:") && !istarts(line, "accept-encoding:"))
                q.headers.append(line, len);
            line += len;
        }
        if (strcmp(q.method, "GET") && strcmp(q.method, "HEAD"))
        {
            q.hasBody = true;
            if (r.body && r.bodyLen)
                q.body.append(r.body, r.bodyLen);
        }
        tryStart(h);
        return h;
    }

    bool next(int h, Event &e) override
    {
        if (h < 0 || h >= MaxRequests || !reqs_[h].used)
            return false;
        Req &q = reqs_[h];
        if (q.slot < 0 && !tryStart(h))
            return false;
        Loader &L = *loaders_[q.slot];
        if (L.busy())
            L.step();
        HttpResponse &r = L.response();
        if (!q.gotHeaders)
        {
            if (L.phase() == Loader::FAILED)
                return failed(h, e, L.error());
            //  A redirect's own head is not the answer: the loader follows it.
            bool final = r.headersDone && !r.isRedirect();
            if (!final && L.phase() != Loader::DONE)
                return false;
            q.gotHeaders = true;
            //  The head less its status line: "Name: value\r\n" lines.
            const char *head = r.rawHead.len ? r.rawHead.cstr() : "";
            const char *nl = strchr(head, '\n');
            q.text.clear();
            q.text.appendStr(nl ? nl + 1 : "");
            q.text.push(0);
            urlFormat(L.url(), q.finalUrl, sizeof(q.finalUrl));
            e.kind = Event::HEADERS;
            e.status = r.status;
            e.statusText = r.reason;
            e.headers = (const char *)q.text.data;
            e.url = q.finalUrl;
            return true;
        }
        if (r.body.len)
        {
            q.text.clear();
            q.text.append(r.body.data, r.body.len);
            r.consume(r.body.len);
            e.kind = Event::DATA;
            e.data = q.text.data;
            e.len = q.text.len;
            return true;
        }
        if (L.phase() == Loader::DONE)
        {
            release(h);
            e.kind = Event::END;
            return true;
        }
        if (L.phase() == Loader::FAILED)
            return failed(h, e, L.error());
        return false;
    }

    void close(int h) override
    {
        if (h >= 0 && h < MaxRequests && reqs_[h].used)
            release(h);
    }

private:
    struct Req
    {
        bool used, stream, gotHeaders, hasBody;
        int slot;
        Url url;
        char method[12], contentType[96], finalUrl[1200], error[160];
        Buf headers;
        Buf body{true};
        Buf text{true}; // what the last event points at
        void reset()
        {
            used = stream = gotHeaders = hasBody = false;
            slot = -1;
            url = Url();
            method[0] = contentType[0] = finalUrl[0] = error[0] = 0;
            headers.release();
            body.release();
            text.release();
        }
    };
    Loader *loaders_[Slots];
    int owner_[Slots];
    Req reqs_[MaxRequests];

    bool tryStart(int h)
    {
        Req &q = reqs_[h];
        for (int i = 0; i < Slots; i++)
            if (owner_[i] < 0)
            {
                owner_[i] = h;
                q.slot = i;
                Loader &L = *loaders_[i];
                L.setIdleTimeout(q.stream ? 120000 : 30000);
                L.start(q.url, false, q.hasBody ? (q.body.data ? q.body.data : (const uint8_t *)"") : nullptr, q.body.len,
                        q.contentType[0] ? q.contentType : nullptr, q.method, q.headers.len ? q.headers.cstr() : nullptr);
                return true;
            }
        return false;
    }

    bool failed(int h, Event &e, const char *why)
    {
        scopy(reqs_[h].error, why && *why ? why : "network error", sizeof(reqs_[h].error));
        release(h);
        e.kind = Event::FAIL;
        e.error = reqs_[h].error;
        return true;
    }

    void release(int h)
    {
        Req &q = reqs_[h];
        if (q.slot >= 0)
        {
            Loader &L = *loaders_[q.slot];
            if (L.busy())
                L.cancel();
            L.response().body.release();
            owner_[q.slot] = -1;
            q.slot = -1;
        }
        q.used = false;
        q.headers.release();
        q.body.release();
    }
};
#else
class ScriptNet
{
};
#endif

//  ── The natives dom.js calls ────────────────────────────────────────────────

struct ScriptPage::Fetch
{
    int id = 0;
    int handle = -1;
    bool module = false, failed = false;
    char url[1200] = {};
    Buf body{true};
};

struct ScriptNatives
{
    static ScriptPage *P(JSContext *ctx) { return (ScriptPage *)jsr2::Engine::from(ctx)->opaque; }

    static void copyArg(JSContext *ctx, JSValueConst v, char *out, size_t cap)
    {
        const char *s = JS_ToCString(ctx, v);
        scopy(out, s ? s : "", cap);
        JS_FreeCString(ctx, s);
    }

    //  parse(html, fragment, context) -> the ops of htmlparse.h
    static JSValue parse(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t len = 0;
        const char *s = argc ? JS_ToCStringLen(ctx, &len, argv[0]) : nullptr;
        if (!s)
            return JS_EXCEPTION;
        char context[32] = "";
        if (argc > 2)
            copyArg(ctx, argv[2], context, sizeof(context));
        JSValue r = parseHtml(ctx, s, len, argc > 1 && JS_ToBool(ctx, argv[1]), context);
        JS_FreeCString(ctx, s);
        return r;
    }

    static JSValue invalidate(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        P(ctx)->dirty_ = true;
        return JS_UNDEFINED;
    }

    static JSValue navigate(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc)
            copyArg(ctx, argv[0], p->nextUrl_, sizeof(p->nextUrl_));
        p->nextReplace_ = argc > 1 && JS_ToBool(ctx, argv[1]);
        return JS_UNDEFINED;
    }

    static JSValue setUrl(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc)
        {
            copyArg(ctx, argv[0], p->newUrl_, sizeof(p->newUrl_));
            scopy(p->pageUrl_, p->newUrl_, sizeof(p->pageUrl_));
            p->engine_.setBaseUrl(p->pageUrl_);
        }
        return JS_UNDEFINED;
    }

    static JSValue history(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int d = 0;
        if (argc)
            JS_ToInt32(ctx, &d, argv[0]);
        P(ctx)->history_ = d;
        return JS_UNDEFINED;
    }

    static JSValue open(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc)
            copyArg(ctx, argv[0], P(ctx)->openUrl_, sizeof(P(ctx)->openUrl_));
        return JS_UNDEFINED;
    }

    static JSValue alert(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc)
            copyArg(ctx, argv[0], p->status_, sizeof(p->status_));
        //  The status line holds one line.
        for (char *c = p->status_; *c; c++)
            if (*c == '\n' || *c == '\r' || *c == '\t')
                *c = ' ';
        if (!p->status_[0])
            scopy(p->status_, " ", sizeof(p->status_));
        return JS_UNDEFINED;
    }

    //  loadScript(url, id, module): an external script the page added.
    static JSValue loadScript(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc < 2)
            return JS_UNDEFINED;
        int id = 0;
        JS_ToInt32(ctx, &id, argv[1]);
        ScriptPage::Fetch *f = nullptr;
        for (int i = 0; i < 8 && !f; i++)
            if (!p->fetches_[i].id)
                f = &p->fetches_[i];
        if (!f || !id)
        {
            //  Too many at once: it fails, as a load error does.
            JSValue a[2] = {JS_NewInt32(ctx, id), JS_FALSE};
            p->callBridge("scriptLoaded", 2, a);
            return JS_UNDEFINED;
        }
        f->id = id;
        f->module = argc > 2 && JS_ToBool(ctx, argv[2]);
        f->failed = false;
        f->body.clear();
        copyArg(ctx, argv[0], f->url, sizeof(f->url));
        f->handle = -1;
#ifndef WEB_HOST
        const char *why = "";
        jsr2::Transport::Request r = {"GET", f->url, "Accept: */*\r\n", nullptr, 0, false};
        f->handle = p->net_->open(r, &why);
#endif
        if (f->handle < 0)
            f->failed = true;
        p->nFetches_++;
        return JS_UNDEFINED;
    }

    //  evalScript(source, name, module): an inline script added to the page.
    static JSValue evalScript(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        size_t len = 0;
        const char *s = argc ? JS_ToCStringLen(ctx, &len, argv[0]) : nullptr;
        if (!s)
            return JS_EXCEPTION;
        char name[256] = "script";
        if (argc > 1)
            copyArg(ctx, argv[1], name, sizeof(name));
        bool module = argc > 2 && JS_ToBool(ctx, argv[2]);
        p->engine_.eval(s, len, name, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }

    //  storage(kind, origin, json): stores json; with null, answers what is stored.
    static JSValue storage(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 3)
            return JS_NULL;
        char kind[8], origin[128];
        copyArg(ctx, argv[0], kind, sizeof(kind));
        copyArg(ctx, argv[1], origin, sizeof(origin));
        if (JS_IsNull(argv[2]) || JS_IsUndefined(argv[2]))
        {
            Stored *s = findStore(kind, origin, false);
            return s && s->json.len ? JS_NewStringLen(ctx, (const char *)s->json.data, s->json.len) : JS_NULL;
        }
        size_t len = 0;
        const char *j = JS_ToCStringLen(ctx, &len, argv[2]);
        if (!j)
            return JS_EXCEPTION;
        if (len > MAX_STORED)
        {
            JS_FreeCString(ctx, j);
            return JS_ThrowRangeError(ctx, "Storage quota exceeded (%u bytes).", (unsigned)MAX_STORED);
        }
        Stored *s = findStore(kind, origin, true);
        s->json.clear();
        s->json.append(j, len);
        JS_FreeCString(ctx, j);
        return JS_TRUE;
    }

    static JSValue viewport(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        ScriptPage *p = P(ctx);
        JSValue a = JS_NewArray(ctx);
        for (int i = 0; i < 8; i++)
            JS_SetPropertyUint32(ctx, a, (uint32_t)i, JS_NewInt32(ctx, p->vp_[i]));
        return a;
    }

    // Each batch is copied straight to native memory; null starts a new
    // snapshot. No complete HTML string needs to live in the script heap.
    static JSValue snapshot(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc < 2)
            return JS_FALSE;
        bool geometry = JS_ToBool(ctx, argv[1]);
        if (geometry && !p->pixelMode_)
            return JS_FALSE;
        Buf &html = geometry ? p->geometryHtml_ : p->renderedHtml_;
        if (JS_IsNull(argv[0]))
        {
            html.clear();
            if (geometry)
                p->geometryValid_ = false;
            return JS_TRUE;
        }
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        bool fits = len <= ScriptPage::RenderLimit - html.len;
        bool copied = fits && html.append(s, len);
        JS_FreeCString(ctx, s);
        if (!copied)
        {
            html.clear();
            scopy(p->status_, fits ? "Not enough memory to show the page." : "The page grew too big to show.", sizeof(p->status_));
            return JS_FALSE;
        }
        return JS_TRUE;
    }

    static JSValue geometry(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p=P(ctx);
        if (!p->pixelMode_ || argc<1) return JS_NULL;
        uint32_t uid=0; JS_ToUint32(ctx,&uid,argv[0]);
        if (!p->geometryValid_) {
            StyleSheetText sheet={p->geometrySheets_.data,p->geometrySheets_.len};
            p->geometryDoc_.loadHtml(p->geometryHtml_.data,p->geometryHtml_.len,"utf-8",&sheet,1,p->geometryCss_);
            if (p->geometryImages_) {
                for(int i=0;i<p->geometryDoc_.imageCount();++i)
                    for(int j=0;j<p->geometryImages_->imageCount();++j)
                        if (!strcmp(p->geometryDoc_.imageSrc(i),p->geometryImages_->imageSrc(j))) {
                            // Images already have their intrinsic sizes in the painted document.
                            const Document &d=*p->geometryImages_;
                            // Expose size without depending on the painted line being present.
                            int w,h; d.imageSize(j,w,h);p->geometryDoc_.setImageSize(i,w,h);break;
                        }
            }
            p->geometryDoc_.layoutPixels(p->pixelWidth_,p->vp_[2],p->vp_[3],p->pixelHeight_);
            p->geometryValid_=!p->geometryDoc_.outOfMemory();
        }
        const Box *b=p->geometryDoc_.boxForNode(uid);
        int values[16]={};
        if (b && b->style.display!=CssStyle::D_NONE) {
            const Box *parent=&p->geometryDoc_.box(b->parent);
            while(parent->parent && !parent->block && !parent->atomic)parent=&p->geometryDoc_.box(parent->parent);
            values[0]=b->x;values[1]=b->y-p->pixelScroll_;values[2]=b->w;values[3]=b->h;
            values[4]=b->clientW;values[5]=b->clientH;values[6]=b->scrollW;values[7]=b->scrollH;
            values[8]=b->border[3];values[9]=b->border[0];
            values[10]=b->x-parent->x-parent->border[3];values[11]=b->y-parent->y-parent->border[0];
            values[12]=(int)parent->uid;values[13]=b->contentW;values[14]=b->contentH;values[15]=b->style.box.sizing;
        }
        JSValue a=JS_NewArray(ctx);
        for(int i=0;i<16;++i)JS_SetPropertyUint32(ctx,a,(uint32_t)i,JS_NewInt32(ctx,values[i]));
        return a;
    }

    //  submit(action, method, data): a form sent by the DOM.
    static JSValue submit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptPage *p = P(ctx);
        if (argc < 3)
            return JS_UNDEFINED;
        copyArg(ctx, argv[0], p->submit_.action, sizeof(p->submit_.action));
        char method[8];
        copyArg(ctx, argv[1], method, sizeof(method));
        p->submit_.post = ieq(method, "post");
        size_t len = 0;
        const char *d = JS_ToCStringLen(ctx, &len, argv[2]);
        p->submit_.data.clear();
        if (d)
            p->submit_.data.append(d, len);
        JS_FreeCString(ctx, d);
        p->haveSubmit_ = true;
        return JS_UNDEFINED;
    }
};

static const JSCFunctionListEntry NATIVES[] = {
    JS_CFUNC_DEF("parse", 3, ScriptNatives::parse),
    JS_CFUNC_DEF("invalidate", 0, ScriptNatives::invalidate),
    JS_CFUNC_DEF("navigate", 2, ScriptNatives::navigate),
    JS_CFUNC_DEF("setUrl", 1, ScriptNatives::setUrl),
    JS_CFUNC_DEF("history", 1, ScriptNatives::history),
    JS_CFUNC_DEF("open", 1, ScriptNatives::open),
    JS_CFUNC_DEF("alert", 1, ScriptNatives::alert),
    JS_CFUNC_DEF("loadScript", 3, ScriptNatives::loadScript),
    JS_CFUNC_DEF("evalScript", 3, ScriptNatives::evalScript),
    JS_CFUNC_DEF("storage", 3, ScriptNatives::storage),
    JS_CFUNC_DEF("viewport", 0, ScriptNatives::viewport),
    JS_CFUNC_DEF("snapshot", 2, ScriptNatives::snapshot),
    JS_CFUNC_DEF("geometry", 1, ScriptNatives::geometry),
    JS_CFUNC_DEF("submit", 3, ScriptNatives::submit),
};

//  ── ScriptPage ──────────────────────────────────────────────────────────────

ScriptPage::ScriptPage() { fetches_ = new Fetch[8]; }

ScriptPage::~ScriptPage()
{
    clear();
    delete[] fetches_;
}

bool ScriptPage::active() const { return engine_.running() && !JS_IsUndefined(bridge_); }

void ScriptPage::clear()
{
    geometryDoc_.clear();geometryHtml_.release();geometrySheets_.release();geometryValid_=false;
    if (active())
        callBridge("unload", 0, nullptr);
    if (engine_.running())
        JS_FreeValue(engine_.context(), bridge_);
    bridge_ = JS_UNDEFINED;
    for (int i = 0; i < 8; i++)
    {
#ifndef WEB_HOST
        if (fetches_[i].id && fetches_[i].handle >= 0)
            net_->close(fetches_[i].handle);
#endif
        fetches_[i].id = 0;
        fetches_[i].body.release();
    }
    nFetches_ = 0;
    engine_.stop();
#ifndef WEB_HOST
    delete net_;
#endif
    net_ = nullptr;
    nScripts_ = 0;
    code_.release();
    renderedHtml_.release();
    dirty_ = haveSubmit_ = nextReplace_ = false;
    nextUrl_[0] = newUrl_[0] = openUrl_[0] = pageUrl_[0] = 0;
    history_ = 0;
}

bool ScriptPage::callBridge(const char *fn, int argc, JSValueConst *argv, JSValue *result)
{
    if (result)
        *result = JS_UNDEFINED;
    if (!active())
        return false;
    JSContext *ctx = engine_.context();
    JSValue f = JS_GetPropertyStr(ctx, bridge_, fn);
    bool ok = engine_.call(f, bridge_, argc, argv, result);
    JS_FreeValue(ctx, f);
    return ok;
}

bool ScriptPage::start(const Buf &html, const char *charset, const char *url)
{
    clear();
    status_[0] = 0;
    if (!needsScripts(html))
        return false;
    ensurePlatform();
    jsr2::Limits limits;
    limits.heapBytes = HeapLimit;
    limits.taskMs = 1000;
    limits.stackBytes = 160u << 10;
    if (!engine_.start(limits))
    {
        scopy(status_, "JavaScript could not start: ", sizeof(status_));
        scat(status_, engine_.error()[0] ? engine_.error() : "no memory", sizeof(status_));
        return false;
    }
    engine_.opaque = this;
    scopy(pageUrl_, url, sizeof(pageUrl_));
#ifndef WEB_HOST
    net_ = new ScriptNet;
    engine_.setTransport(net_);
#endif
    engine_.setBaseUrl(url);
    JSContext *ctx = engine_.context();

    //  The DOM: dom.js's function, called with the natives and libjsr2's
    //  helpers, returns the bridge.
    JSValue fn;
    if (!engine_.evalBinary(r2web_dom, r2web_dom_size, "dom.js", &fn) || !JS_IsFunction(ctx, fn))
    {
        JS_FreeValue(ctx, fn);
        char why[160];
        scopy(why, engine_.error(), sizeof(why));
        clear();
        scopy(status_, why, sizeof(status_));
        return false;
    }
    JSValue natives = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, natives, NATIVES, (int)(sizeof(NATIVES) / sizeof(NATIVES[0])));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue lib = JS_GetPropertyStr(ctx, global, "__jsr2lib");
    JSValue args[2] = {natives, lib};
    JSValue bridge;
    bool ok = engine_.call(fn, JS_UNDEFINED, 2, args, &bridge);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, natives);
    JS_FreeValue(ctx, lib);
    JS_FreeValue(ctx, global);
    if (!ok || !JS_IsObject(bridge))
    {
        JS_FreeValue(ctx, bridge);
        char why[160];
        scopy(why, engine_.error(), sizeof(why));
        clear();
        scopy(status_, why, sizeof(status_));
        return false;
    }
    bridge_ = bridge;

    //  The page into the DOM.
    Buf utf8{true};
    if (!pageToUtf8(html.data, html.len, charset, utf8))
    {
        clear();
        return false;
    }
    JSValue a[2] = {JS_NewString(ctx, url), parseHtml(ctx, utf8.cstr(), utf8.len, false, nullptr)};
    utf8.release();
    ok = callBridge("load", 2, a);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
    if (!ok)
    {
        char why[160];
        scopy(why, engine_.error(), sizeof(why));
        clear();
        scopy(status_, why, sizeof(status_));
        return false;
    }

    //  Its scripts: those that run as the page is read first, in order, then
    //  the deferred ones and modules, in order.
    JSValue list;
    if (callBridge("scripts", 0, nullptr, &list) && JS_IsArray(ctx, list))
    {
        JSValue lenv = JS_GetPropertyStr(ctx, list, "length");
        int32_t n = 0;
        JS_ToInt32(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        for (int pass = 0; pass < 2; pass++)
            for (int32_t i = 0; i < n && nScripts_ < MaxScripts; i++)
            {
                JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
                JSValue kind = JS_GetPropertyUint32(ctx, e, 0), src = JS_GetPropertyUint32(ctx, e, 1);
                JSValue text = JS_GetPropertyUint32(ctx, e, 2), defer = JS_GetPropertyUint32(ctx, e, 4);
                int32_t k = 0;
                JS_ToInt32(ctx, &k, kind);
                const char *u = JS_ToCString(ctx, src);
                //  Modules are deferred; defer means nothing to inline classics.
                bool later = k == 1 || (JS_ToBool(ctx, defer) && u && u[0]);
                if ((pass == 0) == !later)
                {
                    Script &s = scripts_[nScripts_];
                    s.module = k == 1;
                    scopy(s.url, u ? u : "", sizeof(s.url));
                    s.source = (uint32_t)code_.len;
                    if (!s.url[0])
                    {
                        size_t tl = 0;
                        const char *t = JS_ToCStringLen(ctx, &tl, text);
                        if (t)
                            code_.append(t, tl);
                        JS_FreeCString(ctx, t);
                    }
                    code_.push(0);
                    //  Which of the bridge's list it was, for currentScript.
                    code_.append(&i, sizeof(i));
                    nScripts_++;
                }
                JS_FreeCString(ctx, u);
                JS_FreeValue(ctx, kind);
                JS_FreeValue(ctx, src);
                JS_FreeValue(ctx, text);
                JS_FreeValue(ctx, defer);
                JS_FreeValue(ctx, e);
            }
    }
    JS_FreeValue(ctx, list);
    if (code_.failed)
        nScripts_ = 0;
    dirty_ = true;
    return true;
}

bool ScriptPage::run(int i, const char *code, size_t len)
{
    if (!active() || i < 0 || i >= nScripts_)
        return false;
    JSContext *ctx = engine_.context();
    //  The bridge's index of this script follows its text in code_.
    const char *t = source(i);
    int32_t index;
    memcpy(&index, t + strlen(t) + 1, sizeof(index));
    JSValue a = JS_NewInt32(ctx, index);
    callBridge("current", 1, &a);
    bool ok = engine_.eval(code, len, scripts_[i].url[0] ? scripts_[i].url : pageUrl_,
                           scripts_[i].module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    a = JS_NewInt32(ctx, -1);
    callBridge("current", 1, &a);
    return ok;
}

void ScriptPage::parsed() { callBridge("parsed", 0, nullptr); }

bool ScriptPage::eval(const char *source)
{
    if (!active())
        return false;
    return engine_.eval(source, strlen(source), "javascript:", 0);
}

int ScriptPage::nodeOf(const char *handler)
{
    if (!handler || handler[0] != 'r' || handler[1] != '2' || handler[2] != ':')
        return -1;
    int n = 0;
    for (const char *p = handler + 3; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (*p - '0');
    return n;
}

bool ScriptPage::click(int node)
{
    if (!active())
        return true;
    JSValue a = JS_NewInt32(engine_.context(), node), r;
    if (!callBridge("click", 1, &a, &r))
        return false;
    int32_t v = 0;
    JS_ToInt32(engine_.context(), &v, r);
    JS_FreeValue(engine_.context(), r);
    return v != 0;
}

void ScriptPage::input(int node, const char *text)
{
    if (!active())
        return;
    JSContext *ctx = engine_.context();
    JSValue a[2] = {JS_NewInt32(ctx, node), JS_NewString(ctx, text)};
    callBridge("input", 2, a);
    JS_FreeValue(ctx, a[1]);
}

void ScriptPage::controlChanged(int node)
{
    if (!active())
        return;
    JSValue a = JS_NewInt32(engine_.context(), node);
    callBridge("changed", 1, &a);
}

bool ScriptPage::state(int node, bool checked, int selected)
{
    if (!active())
        return true;
    JSContext *ctx = engine_.context();
    JSValue a[3] = {JS_NewInt32(ctx, node), JS_NewBool(ctx, checked), JS_NewInt32(ctx, selected)}, r;
    if (!callBridge("state", 3, a, &r))
        return true;
    bool keep = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    return keep;
}

bool ScriptPage::submitFrom(int node)
{
    if (!active())
        return true;
    JSValue a = JS_NewInt32(engine_.context(), node), r;
    if (!callBridge("submitFrom", 1, &a, &r))
        return false;
    bool go = JS_ToBool(engine_.context(), r);
    JS_FreeValue(engine_.context(), r);
    return go;
}

bool ScriptPage::key(const char *keyName)
{
    if (!active())
        return false;
    JSContext *ctx = engine_.context();
    static const char *const types[] = {"keydown", "keyup"};
    int code = !strcmp(keyName, "Enter") ? 13 : !strcmp(keyName, "Escape") ? 27 : !strcmp(keyName, "Tab") ? 9 : 0;
    bool taken = false;
    for (const char *type : types)
    {
        JSValue a[7] = {JS_NewString(ctx, type), JS_NewString(ctx, keyName), JS_NewString(ctx, keyName),
                        JS_NewInt32(ctx, code), JS_FALSE, JS_FALSE, JS_FALSE};
        JSValue r;
        if (callBridge("key", 7, a, &r))
        {
            if (type == types[0])
                taken = JS_ToBool(ctx, r) != 0;
            JS_FreeValue(ctx, r);
        }
        for (int i = 0; i < 3; i++)
            JS_FreeValue(ctx, a[i]);
    }
    return taken;
}

void ScriptPage::focus(int node)
{
    if (!active())
        return;
    JSValue a = JS_NewInt32(engine_.context(), node);
    callBridge("focus", 1, &a);
}

void ScriptPage::pollFetches()
{
    if (!nFetches_)
        return;
    JSContext *ctx = engine_.context();
    for (int i = 0; i < 8; i++)
    {
        Fetch &f = fetches_[i];
        if (!f.id)
            continue;
        bool done = f.failed, ok = false;
#ifndef WEB_HOST
        jsr2::Transport::Event e;
        while (!done && net_->next(f.handle, e))
        {
            if (e.kind == jsr2::Transport::Event::HEADERS && e.status != 200)
                f.failed = true;
            else if (e.kind == jsr2::Transport::Event::DATA)
                f.body.append(e.data, e.len);
            else if (e.kind == jsr2::Transport::Event::END)
                done = true, ok = !f.failed && !f.body.failed;
            else if (e.kind == jsr2::Transport::Event::FAIL)
                done = true;
            e = jsr2::Transport::Event();
        }
#endif
        if (!done)
            continue;
        int id = f.id;
        f.id = 0;
        nFetches_--;
        if (ok)
            engine_.eval(f.body.cstr(), f.body.len, f.url, f.module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
        f.body.release();
        JSValue a[2] = {JS_NewInt32(ctx, id), JS_NewBool(ctx, ok)};
        callBridge("scriptLoaded", 2, a);
    }
}

void ScriptPage::tick()
{
    if (!active())
        return;
    engine_.tick();
    pollFetches();
}

bool ScriptPage::busy() const { return active() && (engine_.busy() || nFetches_ > 0); }
bool ScriptPage::wantsFrame() const { return active() && engine_.wantsFrame(); }
void ScriptPage::frame(double ms) { engine_.frame(ms); }
bool ScriptPage::changed() const { return active() && dirty_; }

bool ScriptPage::render(Buf &html, char *title, size_t titleCap, bool force)
{
    if (!active() || (!dirty_ && !force))
        return false;
    JSContext *ctx = engine_.context();
    JSValue a = JS_NewBool(ctx, force), r;
    dirty_ = false;
    if (!callBridge("render", 1, &a, &r))
        return false;
    bool done = false;
    if (JS_IsArray(ctx, r))
    {
        JSValue h = JS_GetPropertyUint32(ctx, r, 0), t = JS_GetPropertyUint32(ctx, r, 1);
        if (JS_ToBool(ctx, h) && !renderedHtml_.failed)
        {
            if (html.big)
            {
                html.swap(renderedHtml_);
                done = true;
            }
            else
            {
                html.clear();
                done = html.append(renderedHtml_.data, renderedHtml_.len);
            }
            renderedHtml_.clear();
        }
        const char *ts = JS_ToCString(ctx, t);
        if (title)
            scopy(title, ts ? ts : "", titleCap);
        JS_FreeCString(ctx, ts);
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, t);
    }
    JS_FreeValue(ctx, r);
    if (!done && !status_[0])
        scopy(status_, "The page grew too big to show.", sizeof(status_));
    return done;
}

const char *ScriptPage::error() const { return engine_.running() ? engine_.error() : ""; }

bool ScriptPage::takeStatus(char *out, size_t cap)
{
    if (!status_[0])
        return false;
    scopy(out, status_, cap);
    status_[0] = 0;
    return true;
}

bool ScriptPage::takeUrl(char *out, size_t cap)
{
    if (!newUrl_[0])
        return false;
    scopy(out, newUrl_, cap);
    newUrl_[0] = 0;
    return true;
}

bool ScriptPage::takeOpen(char *out, size_t cap)
{
    if (!openUrl_[0])
        return false;
    scopy(out, openUrl_, cap);
    openUrl_[0] = 0;
    return true;
}

int ScriptPage::takeHistory()
{
    int h = history_;
    history_ = 0;
    return h;
}

bool ScriptPage::takeSubmit(Submit &s)
{
    if (!haveSubmit_)
        return false;
    haveSubmit_ = false;
    scopy(s.action, submit_.action, sizeof(s.action));
    s.post = submit_.post;
    s.data.clear();
    s.data.append(submit_.data.data, submit_.data.len);
    submit_.data.release();
    return true;
}

void ScriptPage::setViewport(int cols, int rows, int cellW, int cellH, bool dark)
{
    vp_[0] = cols;
    vp_[1] = rows;

    vp_[4] = dark;
    vp_[5] = cols * cellW; vp_[6] = rows * cellH;
    setPixelViewport(vp_[5],vp_[6],cellW,cellH,pixelScroll_,pixelMode_,geometryCss_,dark);
}

void ScriptPage::setPixelViewport(int width,int height,int cellW,int lineH,int scroll,bool enabled,bool css,bool dark)
{
    if (pixelWidth_!=width || pixelHeight_!=height || vp_[2]!=cellW || vp_[3]!=lineH || geometryCss_!=css || pixelMode_!=enabled)
        geometryValid_=false;
    pixelWidth_=width;pixelHeight_=height;pixelScroll_=scroll;pixelMode_=enabled;geometryCss_=css;
    vp_[0]=width/(cellW>0?cellW:1);vp_[1]=height/(lineH>0?lineH:1);
    vp_[2]=cellW;vp_[3]=lineH;vp_[4]=dark;vp_[5]=width;vp_[6]=height;vp_[7]=scroll;
}
void ScriptPage::setLayoutSheets(const StyleSheetText *sheets,int count)
{
    geometrySheets_.clear();
    for(int i=0;i<count;++i) {geometrySheets_.append(sheets[i].data,sheets[i].len);geometrySheets_.push('\n');}
    geometryValid_=false;
}
void ScriptPage::setLayoutImages(const Document *document)
{
    uint32_t hash=2166136261u;
    if(document)for(int i=0;i<document->imageCount();++i) {
        int w,h;document->imageSize(i,w,h);
        hash=(hash^(uint32_t)w)*16777619u;hash=(hash^(uint32_t)h)*16777619u;
        for(const char *s=document->imageSrc(i);*s;++s)hash=(hash^(uint8_t)*s)*16777619u;
    }
    if(hash!=geometryImageHash_ || document!=geometryImages_)geometryValid_=false;
    geometryImageHash_=hash;geometryImages_=document;
}

size_t ScriptPage::heapBytes() const { return engine_.heapBytes(); }

const char *scriptConsole(size_t *len)
{
    *len = g_consoleLen;
    return g_console;
}

} // namespace web
