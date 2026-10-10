//
//  engine.cpp --- jsr2::Engine: the runtime, tasks, the event loop, and the
//  natives the prelude (prelude.js) builds the Web APIs on.  See jsr2.h.
//
//  The natives are one object, __jsr2, which the prelude and an embedder's
//  own prelude (r2web's DOM) call into.  Each callback into the page is a
//  task: begin() starts its clock, end() runs the microtask checkpoint
//  within the same time and reports what was left unhandled.
//
#include "jsr2.h"
#include <string.h>

extern "C" const uint8_t jsr2_prelude[];
extern "C" const size_t jsr2_prelude_size;

namespace jsr2 {
namespace {

Platform g_platform = {};

//  Appends src to the string in dst (cap bytes), cutting what does not fit.
void cat(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(dst);
    while (*src && n + 1 < cap)
        dst[n++] = *src++;
    dst[n] = 0;
}

//  An array's or a string's length.
int64_t lengthOf(JSContext *ctx, JSValueConst v)
{
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    int64_t n = 0;
    if (JS_ToInt64(ctx, &n, l) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, l);
    return n;
}

//  Bytes in UTF-8: a code point below 0x110000.
size_t putUtf8(uint8_t *out, uint32_t c)
{
    if (c < 0x80)
    {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800)
    {
        out[0] = (uint8_t)(0xC0 | (c >> 6));
        out[1] = (uint8_t)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000)
    {
        out[0] = (uint8_t)(0xE0 | (c >> 12));
        out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (c >> 18));
    out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (c & 0x3F));
    return 4;
}

} // namespace

void setPlatform(const Platform &p) { g_platform = p; }
const Platform &platform() { return g_platform; }

} // namespace jsr2

//  The C side (src/libc.c on r2) reaches the platform through these.
extern "C" void jsr2_platform_log(int level, const char *text, size_t len)
{
    if (jsr2::g_platform.log)
        jsr2::g_platform.log(level, text, len);
}
extern "C" double jsr2_platform_epoch_ms(void) { return jsr2::g_platform.epochMs ? jsr2::g_platform.epochMs() : 0; }
extern "C" unsigned long long jsr2_platform_monotonic_ms(void)
{
    return jsr2::g_platform.monotonicMs ? jsr2::g_platform.monotonicMs() : 0;
}

namespace jsr2 {

//  ── Memory ──────────────────────────────────────────────────────────────────
//
//  QuickJS's allocator hooks keep its own count: malloc_size decides when the
//  collector runs, malloc_limit is JS_SetMemoryLimit's.

namespace {
const size_t OVERHEAD = 16;

void *qMalloc(JSMallocState *s, size_t n)
{
    Heap *h = (Heap *)s->opaque;
    if (s->malloc_size + n > s->malloc_limit)
        return nullptr;
    void *p = h->alloc(n);
    if (!p)
        return nullptr;
    s->malloc_count++;
    s->malloc_size += h->usable(p) + OVERHEAD;
    return p;
}

void qFree(JSMallocState *s, void *p)
{
    if (!p)
        return;
    Heap *h = (Heap *)s->opaque;
    s->malloc_count--;
    s->malloc_size -= h->usable(p) + OVERHEAD;
    h->free(p);
}

void *qRealloc(JSMallocState *s, void *p, size_t n)
{
    Heap *h = (Heap *)s->opaque;
    if (!p)
        return n ? qMalloc(s, n) : nullptr;
    size_t old = h->usable(p);
    if (!n)
    {
        qFree(s, p);
        return nullptr;
    }
    if (s->malloc_size + n - old > s->malloc_limit)
        return nullptr;
    void *fresh = h->realloc(p, n);
    if (!fresh)
        return nullptr;
    s->malloc_size += h->usable(fresh) - old;
    return fresh;
}

size_t qUsable(const void *) { return 0; }

const JSMallocFunctions MALLOC = {qMalloc, qFree, qRealloc, qUsable};

} // namespace

//  ── Errors ──────────────────────────────────────────────────────────────────

void describe(JSContext *ctx, JSValueConst v, char *out, size_t cap)
{
    out[0] = 0;
    if (!cap)
        return;
    size_t n = 0;
    auto put = [&](const char *s) {
        while (s && *s && n + 1 < cap)
            out[n++] = *s++;
        out[n] = 0;
    };
    if (JS_IsError(ctx, v))
    {
        JSValue name = JS_GetPropertyStr(ctx, v, "name"), msg = JS_GetPropertyStr(ctx, v, "message");
        const char *a = JS_ToCString(ctx, name), *b = JS_ToCString(ctx, msg);
        put(a ? a : "Error");
        if (b && *b)
        {
            put(": ");
            put(b);
        }
        JS_FreeCString(ctx, a);
        JS_FreeCString(ctx, b);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, msg);
        //  Where: the first frame of the stack, "    at f (page.js:3:7)".
        JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
        const char *s = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
        if (s)
        {
            const char *open = strchr(s, '('), *close = open ? strchr(open, ')') : nullptr;
            const char *nl = strchr(s, '\n');
            if (open && close && (!nl || open < nl))
            {
                put(" ");
                char where[96];
                size_t w = (size_t)(close - open + 1) < sizeof(where) ? (size_t)(close - open + 1) : sizeof(where) - 1;
                memcpy(where, open, w);
                where[w] = 0;
                put(where);
            }
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, stack);
        return;
    }
    const char *s = JS_ToCString(ctx, v);
    if (s)
    {
        put(s);
        JS_FreeCString(ctx, s);
    }
    else
    {
        JS_FreeValue(ctx, JS_GetException(ctx));
        put("(a value that cannot be shown)");
    }
}

void Engine::setError(const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof(error_))
        n = sizeof(error_) - 1;
    memcpy(error_, s, n);
    error_[n] = 0;
    errors_++;
    jsr2_platform_log(Error, error_, n);
}

void Engine::reportException()
{
    JSValue ex = JS_GetException(ctx_);
    if (timedOut_)
        setError("JavaScript stopped: execution limit exceeded.");
    else
    {
        char msg[sizeof(error_) - 10];
        describe(ctx_, ex, msg, sizeof(msg));
        char line[sizeof(error_)] = "Uncaught ";
        cat(line, msg, sizeof(line));
        setError(line);
    }
    JS_FreeValue(ctx_, ex);
}

//  ── The engine ──────────────────────────────────────────────────────────────

struct Engine::Timer
{
    int id;
    bool repeat;
    uint32_t interval;
    uint64_t due;
    JSValue fn, args;
};

struct Engine::Frame
{
    int id;
    JSValue fn;
};

struct Engine::Request
{
    int id;
    int handle;
    JSValue cb;
};

namespace {

//  Unhandled rejections are reported at the end of the microtask
//  checkpoint, as a browser does, so a handler attached in the same task
//  still counts.
const int MAX_REJECTIONS = 16;
struct Rejection
{
    JSValue promise, reason;
};

struct EngineState
{
    Rejection rejections[MAX_REJECTIONS];
    int nRejections = 0;
    JSValue natives = JS_UNDEFINED;
    uint64_t origin = 0;
};

EngineState *stateOf(Engine *e);

template <typename T> bool grow(Heap &h, T *&arr, int &cap, int need)
{
    if (need <= cap)
        return true;
    int fresh = cap ? cap * 2 : 8;
    while (fresh < need)
        fresh *= 2;
    T *p = (T *)h.realloc(arr, sizeof(T) * (size_t)fresh);
    if (!p)
        return false;
    arr = p;
    cap = fresh;
    return true;
}

} // namespace

//  Not part of jsr2.h: the natives need the engine's insides.
struct Natives;

Engine::Engine() : heap_(8u << 20) {}
Engine::~Engine() { stop(); }

Engine *Engine::from(JSContext *ctx) { return (Engine *)JS_GetContextOpaque(ctx); }

int Engine::interrupt(JSRuntime *, void *opaque)
{
    Engine *e = (Engine *)opaque;
    if (e->depth_ > 0 && e->deadline_ && platform().monotonicMs() > e->deadline_)
    {
        e->timedOut_ = true;
        return 1;
    }
    return 0;
}

namespace {
EngineState g_states[4];
Engine *g_owners[4] = {};
EngineState *stateOf(Engine *e)
{
    for (int i = 0; i < 4; i++)
        if (g_owners[i] == e)
            return &g_states[i];
    return nullptr;
}
EngineState *claimState(Engine *e)
{
    for (int i = 0; i < 4; i++)
        if (!g_owners[i])
        {
            g_owners[i] = e;
            g_states[i] = EngineState();
            return &g_states[i];
        }
    return nullptr;
}
void dropState(Engine *e)
{
    for (int i = 0; i < 4; i++)
        if (g_owners[i] == e)
            g_owners[i] = nullptr;
}
} // namespace

void Engine::rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, int handled, void *opaque)
{
    Engine *e = (Engine *)opaque;
    EngineState *st = stateOf(e);
    if (!st)
        return;
    if (handled)
    {
        for (int i = 0; i < st->nRejections; i++)
            if (JS_VALUE_GET_PTR(st->rejections[i].promise) == JS_VALUE_GET_PTR(promise))
            {
                JS_FreeValue(ctx, st->rejections[i].promise);
                JS_FreeValue(ctx, st->rejections[i].reason);
                st->rejections[i] = st->rejections[--st->nRejections];
                return;
            }
        return;
    }
    if (st->nRejections == MAX_REJECTIONS)
        return;
    st->rejections[st->nRejections++] = {JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)};
}

bool Engine::begin()
{
    if (!ctx_)
        return false;
    if (depth_++ == 0)
    {
        timedOut_ = false;
        deadline_ = platform().monotonicMs() + limits_.taskMs;
        JS_UpdateStackTop(rt_);
    }
    return true;
}

void Engine::microtasks()
{
    for (int n = 0;; n++)
    {
        if (!JS_IsJobPending(rt_))
            break;
        //  An endless chain of promises would keep this loop going while
        //  each job stays short; the task's time covers the whole checkpoint.
        if ((n & 63) == 63 && platform().monotonicMs() > deadline_)
        {
            setError("JavaScript stopped: microtasks exceeded the execution limit.");
            break;
        }
        JSContext *jctx;
        if (JS_ExecutePendingJob(rt_, &jctx) < 0)
            reportException();
    }
    EngineState *st = stateOf(this);
    if (st)
    {
        for (int i = 0; i < st->nRejections; i++)
        {
            char msg[200];
            describe(ctx_, st->rejections[i].reason, msg, sizeof(msg));
            char line[sizeof(error_)] = "Uncaught (in promise) ";
            cat(line, msg, sizeof(line));
            setError(line);
            JS_FreeValue(ctx_, st->rejections[i].promise);
            JS_FreeValue(ctx_, st->rejections[i].reason);
        }
        st->nRejections = 0;
    }
}

bool Engine::end(bool ok)
{
    if (!ok)
        reportException();
    if (--depth_ == 0)
    {
        microtasks();
        deadline_ = 0;
    }
    return ok;
}

bool Engine::eval(const char *src, size_t len, const char *name, int flags)
{
    if (!begin())
        return false;
    JSValue r = JS_Eval(ctx_, src, len, name, flags);
    bool ok = !JS_IsException(r);
    if (ok && (flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE && JS_IsObject(r))
    {
        //  A module's evaluation is a promise (top-level await).
        end(true);
        int state = JS_PromiseState(ctx_, r);
        if (state == JS_PROMISE_REJECTED)
        {
            JSValue why = JS_PromiseResult(ctx_, r);
            char msg[200];
            describe(ctx_, why, msg, sizeof(msg));
            char line[sizeof(error_)] = "Uncaught ";
            cat(line, msg, sizeof(line));
            setError(line);
            JS_FreeValue(ctx_, why);
            ok = false;
        }
        JS_FreeValue(ctx_, r);
        return ok;
    }
    JS_FreeValue(ctx_, r);
    return end(ok);
}

bool Engine::evalBinary(const uint8_t *buf, size_t len, const char *, JSValue *result)
{
    if (result)
        *result = JS_UNDEFINED;
    if (!begin())
        return false;
    JSValue fn = JS_ReadObject(ctx_, buf, len, JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA);
    bool ok = !JS_IsException(fn);
    if (ok)
    {
        JSValue r = JS_EvalFunction(ctx_, fn);
        ok = !JS_IsException(r);
        if (ok && result)
            *result = r;
        else
            JS_FreeValue(ctx_, r);
    }
    return end(ok);
}

bool Engine::call(JSValueConst fn, JSValueConst self, int argc, JSValueConst *argv, JSValue *result)
{
    if (result)
        *result = JS_UNDEFINED;
    if (!begin())
        return false;
    JSValue r = JS_Call(ctx_, fn, self, argc, argv);
    bool ok = !JS_IsException(r);
    if (ok && result)
        *result = r;
    else
        JS_FreeValue(ctx_, r);
    return end(ok);
}

//  ── Timers, frames, requests ────────────────────────────────────────────────

uint64_t Engine::nextTimer() const
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < nTimers_; i++)
        if (timers_[i].due < best)
            best = timers_[i].due;
    return best;
}

bool Engine::busy() const { return ctx_ && (nTimers_ || nFrames_ || nRequests_); }

void Engine::runTimers()
{
    uint64_t now = platform().monotonicMs();
    //  Only what was due when this tick began, oldest first, and a bounded
    //  number: a page re-arming zero-delay timers cannot starve the window.
    for (int budget = 0; budget < 64 && ctx_; budget++)
    {
        int pick = -1;
        for (int i = 0; i < nTimers_; i++)
            if (timers_[i].due <= now && (pick < 0 || timers_[i].due < timers_[pick].due ||
                                          (timers_[i].due == timers_[pick].due && timers_[i].id < timers_[pick].id)))
                pick = i;
        if (pick < 0)
            break;
        Timer &t = timers_[pick];
        JSValue fn = JS_DupValue(ctx_, t.fn), args = JS_DupValue(ctx_, t.args);
        if (t.repeat)
            t.due = now + (t.interval ? t.interval : 1);
        else
        {
            JS_FreeValue(ctx_, t.fn);
            JS_FreeValue(ctx_, t.args);
            timers_[pick] = timers_[--nTimers_];
        }
        if (JS_IsString(fn))
        {
            size_t len;
            const char *src = JS_ToCStringLen(ctx_, &len, fn);
            if (src)
                eval(src, len, "timer", 0);
            JS_FreeCString(ctx_, src);
        }
        else
        {
            JSValue argv[16];
            int argc = 0;
            if (JS_IsArray(ctx_, args))
            {
                int64_t n = lengthOf(ctx_, args);
                for (; argc < n && argc < 16; argc++)
                    argv[argc] = JS_GetPropertyUint32(ctx_, args, (uint32_t)argc);
            }
            call(fn, JS_UNDEFINED, argc, argv);
            for (int i = 0; i < argc; i++)
                JS_FreeValue(ctx_, argv[i]);
        }
        JS_FreeValue(ctx_, fn);
        JS_FreeValue(ctx_, args);
    }
}

void Engine::frame(double timestamp)
{
    if (!ctx_ || !nFrames_)
        return;
    //  Callbacks asked for during this frame wait for the next one.
    int n = nFrames_;
    Frame local[64];
    if (n > 64)
        n = 64;
    memcpy(local, frames_, sizeof(Frame) * (size_t)n);
    memmove(frames_, frames_ + n, sizeof(Frame) * (size_t)(nFrames_ - n));
    nFrames_ -= n;
    JSValue ts = JS_NewFloat64(ctx_, timestamp);
    for (int i = 0; i < n; i++)
    {
        if (ctx_)
            call(local[i].fn, JS_UNDEFINED, 1, &ts);
        if (ctx_)
            JS_FreeValue(ctx_, local[i].fn);
    }
}

void Engine::pollRequests()
{
    if (!transport_)
        return;
    //  By id, not by index: a callback may close or open requests.
    for (int i = 0; i < nRequests_ && ctx_; i++)
    {
        int id = requests_[i].id;
        Transport::Event ev;
        for (int guard = 0; guard < 32; guard++)
        {
            int at = -1;
            for (int k = 0; k < nRequests_; k++)
                if (requests_[k].id == id)
                    at = k;
            if (at < 0)
                break;
            ev = Transport::Event();
            if (!transport_->next(requests_[at].handle, ev))
                break;
            JSValue cb = JS_DupValue(ctx_, requests_[at].cb);
            bool last = ev.kind == Transport::Event::END || ev.kind == Transport::Event::FAIL;
            if (last)
            {
                transport_->close(requests_[at].handle);
                JS_FreeValue(ctx_, requests_[at].cb);
                requests_[at] = requests_[--nRequests_];
            }
            JSValue argv[5];
            int argc = 1;
            switch (ev.kind)
            {
            case Transport::Event::HEADERS:
                argv[0] = JS_NewString(ctx_, "headers");
                argv[1] = JS_NewInt32(ctx_, ev.status);
                argv[2] = JS_NewString(ctx_, ev.statusText ? ev.statusText : "");
                argv[3] = JS_NewString(ctx_, ev.headers ? ev.headers : "");
                argv[4] = JS_NewString(ctx_, ev.url ? ev.url : "");
                argc = 5;
                break;
            case Transport::Event::DATA:
                argv[0] = JS_NewString(ctx_, "data");
                argv[1] = JS_NewArrayBufferCopy(ctx_, ev.data, ev.len);
                argc = 2;
                break;
            case Transport::Event::END:
                argv[0] = JS_NewString(ctx_, "end");
                break;
            default:
                argv[0] = JS_NewString(ctx_, "fail");
                argv[1] = JS_NewString(ctx_, ev.error && *ev.error ? ev.error : "network error");
                argc = 2;
                break;
            }
            call(cb, JS_UNDEFINED, argc, argv);
            if (ctx_)
            {
                for (int k = 0; k < argc; k++)
                    JS_FreeValue(ctx_, argv[k]);
                JS_FreeValue(ctx_, cb);
            }
            if (last)
            {
                i--; // the slot now holds another request
                break;
            }
        }
    }
}

void Engine::tick()
{
    if (!ctx_)
        return;
    pollRequests();
    runTimers();
}

//  ── Natives ─────────────────────────────────────────────────────────────────

struct Natives
{
    static Engine *E(JSContext *ctx) { return Engine::from(ctx); }

    static JSValue log(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int level = 0;
        if (argc > 0)
            JS_ToInt32(ctx, &level, argv[0]);
        size_t len = 0;
        const char *s = argc > 1 ? JS_ToCStringLen(ctx, &len, argv[1]) : nullptr;
        if (s)
            jsr2_platform_log(level, s, len);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }

    static JSValue reportError(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        char msg[200];
        describe(ctx, argc ? argv[0] : JS_UNDEFINED, msg, sizeof(msg));
        char line[256] = "Uncaught ";
        cat(line, msg, sizeof(line));
        E(ctx)->setError(line);
        return JS_UNDEFINED;
    }

    static JSValue now(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        EngineState *st = stateOf(E(ctx));
        return JS_NewFloat64(ctx, (double)(platform().monotonicMs() - (st ? st->origin : 0)));
    }

    static JSValue setTimer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        if (argc < 1 || (!JS_IsFunction(ctx, argv[0]) && !JS_IsString(argv[0])))
            return JS_NewInt32(ctx, 0);
        double delay = 0;
        if (argc > 1)
            JS_ToFloat64(ctx, &delay, argv[1]);
        if (!(delay >= 0))
            delay = 0;
        if (delay > 2147483647.0)
            delay = 2147483647.0;
        if (!grow(e->heap_, e->timers_, e->capTimers_, e->nTimers_ + 1))
            return JS_ThrowOutOfMemory(ctx);
        Engine::Timer &t = e->timers_[e->nTimers_++];
        t.id = e->nextTimerId_++;
        t.repeat = argc > 2 && JS_ToBool(ctx, argv[2]);
        t.interval = (uint32_t)delay;
        t.due = platform().monotonicMs() + t.interval;
        t.fn = JS_DupValue(ctx, argv[0]);
        t.args = argc > 3 ? JS_DupValue(ctx, argv[3]) : JS_UNDEFINED;
        return JS_NewInt32(ctx, t.id);
    }

    static JSValue clearTimer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        int id = 0;
        if (argc)
            JS_ToInt32(ctx, &id, argv[0]);
        for (int i = 0; i < e->nTimers_; i++)
            if (e->timers_[i].id == id)
            {
                JSValue fn = e->timers_[i].fn, args = e->timers_[i].args;
                e->timers_[i] = e->timers_[--e->nTimers_];
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, args);
                break;
            }
        return JS_UNDEFINED;
    }

    static JSValue requestFrame(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        if (!argc || !JS_IsFunction(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "requestAnimationFrame needs a function");
        if (!grow(e->heap_, e->frames_, e->capFrames_, e->nFrames_ + 1))
            return JS_ThrowOutOfMemory(ctx);
        Engine::Frame &f = e->frames_[e->nFrames_++];
        f.id = e->nextFrameId_++;
        f.fn = JS_DupValue(ctx, argv[0]);
        return JS_NewInt32(ctx, f.id);
    }

    static JSValue cancelFrame(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        int id = 0;
        if (argc)
            JS_ToInt32(ctx, &id, argv[0]);
        for (int i = 0; i < e->nFrames_; i++)
            if (e->frames_[i].id == id)
            {
                JSValue fn = e->frames_[i].fn;
                memmove(e->frames_ + i, e->frames_ + i + 1, sizeof(Engine::Frame) * (size_t)(e->nFrames_ - i - 1));
                e->nFrames_--;
                JS_FreeValue(ctx, fn);
                break;
            }
        return JS_UNDEFINED;
    }

    //  open(method, url, headers, body, stream, callback) -> id
    static JSValue open(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        if (argc < 6 || !JS_IsFunction(ctx, argv[5]))
            return JS_ThrowTypeError(ctx, "open: bad arguments");
        if (!e->transport_)
            return JS_ThrowTypeError(ctx, "This program has no network for scripts.");
        const char *method = JS_ToCString(ctx, argv[0]);
        const char *url = JS_ToCString(ctx, argv[1]);
        const char *headers = JS_ToCString(ctx, argv[2]);
        size_t bodyLen = 0;
        const uint8_t *body = nullptr;
        JSValue holder;
        bytesOf(ctx, argv[3], &body, &bodyLen, &holder);
        JSValue result;
        if (!method || !url || !headers)
            result = JS_EXCEPTION;
        else if (!grow(e->heap_, e->requests_, e->capRequests_, e->nRequests_ + 1))
            result = JS_ThrowOutOfMemory(ctx);
        else
        {
            Transport::Request r = {method, url, headers, body, bodyLen, JS_ToBool(ctx, argv[4]) != 0};
            const char *why = "network error";
            int h = e->transport_->open(r, &why);
            if (h < 0)
                result = JS_ThrowTypeError(ctx, "%s", why);
            else
            {
                Engine::Request &q = e->requests_[e->nRequests_++];
                q.id = e->nextRequestId_++;
                q.handle = h;
                q.cb = JS_DupValue(ctx, argv[5]);
                result = JS_NewInt32(ctx, q.id);
            }
        }
        JS_FreeValue(ctx, holder);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        JS_FreeCString(ctx, headers);
        return result;
    }

    static JSValue close(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        Engine *e = E(ctx);
        int id = 0;
        if (argc)
            JS_ToInt32(ctx, &id, argv[0]);
        for (int i = 0; i < e->nRequests_; i++)
            if (e->requests_[i].id == id)
            {
                if (e->transport_)
                    e->transport_->close(e->requests_[i].handle);
                JSValue cb = e->requests_[i].cb;
                e->requests_[i] = e->requests_[--e->nRequests_];
                JS_FreeValue(ctx, cb);
                break;
            }
        return JS_UNDEFINED;
    }

    //  UTF-8 <-> JS strings.  QuickJS hands out CESU-8-ish text for lone
    //  surrogates; those become U+FFFD as TextEncoder requires.
    static JSValue utf8Encode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t len = 0;
        const char *s = argc ? JS_ToCStringLen(ctx, &len, argv[0]) : nullptr;
        if (!s)
            return JS_EXCEPTION;
        //  Replace encoded surrogates (ED A0..BF xx) with EF BF BD.
        JSValue out = JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, len);
        size_t total;
        uint8_t *p = JS_GetArrayBuffer(ctx, &total, out);
        if (p)
            for (size_t i = 0; i + 2 < total; i++)
                if (p[i] == 0xED && p[i + 1] >= 0xA0)
                {
                    p[i] = 0xEF;
                    p[i + 1] = 0xBF;
                    p[i + 2] = 0xBD;
                    i += 2;
                }
        JS_FreeCString(ctx, s);
        return out;
    }

    //  The bytes of a typed array, a DataView or an ArrayBuffer.  *holder
    //  keeps the buffer alive; free it when done.
    static bool bytesOf(JSContext *ctx, JSValueConst v, const uint8_t **p, size_t *n, JSValue *holder)
    {
        *p = nullptr;
        *n = 0;
        *holder = JS_UNDEFINED;
        if (!JS_IsObject(v))
            return false;
        size_t off = 0, len = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
        if (!JS_IsException(buf))
        {
            size_t total;
            uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
            *holder = buf;
            if (!base)
                return false;
            *p = base + off;
            *n = len;
            return true;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t total;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, v);
        if (!base)
        {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return false;
        }
        *p = base;
        *n = total;
        return true;
    }

    //  utf8Decode(view, stream, fatal) -> [text, used].  Malformed input
    //  becomes U+FFFD, one per maximal subpart (WHATWG Encoding); with stream,
    //  an unfinished sequence at the end is left for the next call.
    static JSValue utf8Decode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        const uint8_t *p;
        size_t n;
        JSValue holder;
        bytesOf(ctx, argc ? argv[0] : JS_UNDEFINED, &p, &n, &holder);
        bool stream = argc > 1 && JS_ToBool(ctx, argv[1]);
        bool fatal = argc > 2 && JS_ToBool(ctx, argv[2]);
        uint8_t *out = (uint8_t *)js_malloc(ctx, n * 3 + 1);
        if (!out)
        {
            JS_FreeValue(ctx, holder);
            return JS_EXCEPTION;
        }
        size_t i = 0, k = 0;
        bool bad = false;
        while (i < n)
        {
            uint8_t c = p[i];
            if (c < 0x80)
            {
                out[k++] = c;
                i++;
                continue;
            }
            size_t need;
            uint8_t lo = 0x80, hi = 0xBF;
            if (c >= 0xC2 && c <= 0xDF)
                need = 1;
            else if (c >= 0xE0 && c <= 0xEF)
            {
                need = 2;
                if (c == 0xE0)
                    lo = 0xA0;
                if (c == 0xED)
                    hi = 0x9F;
            }
            else if (c >= 0xF0 && c <= 0xF4)
            {
                need = 3;
                if (c == 0xF0)
                    lo = 0x90;
                if (c == 0xF4)
                    hi = 0x8F;
            }
            else
            {
                k += putUtf8(out + k, 0xFFFD);
                bad = true;
                i++;
                continue;
            }
            size_t j = 1;
            for (; j <= need && i + j < n; j++)
            {
                uint8_t b = p[i + j];
                if (b < (j == 1 ? lo : 0x80) || b > (j == 1 ? hi : 0xBF))
                    break;
            }
            if (j <= need)
            {
                if (i + j >= n && stream)
                    break; // the rest may come in the next piece
                k += putUtf8(out + k, 0xFFFD);
                bad = true;
                i += j;
                continue;
            }
            memcpy(out + k, p + i, need + 1);
            k += need + 1;
            i += need + 1;
        }
        JS_FreeValue(ctx, holder);
        if (bad && fatal)
        {
            js_free(ctx, out);
            return JS_ThrowTypeError(ctx, "The encoded data was not valid UTF-8.");
        }
        JSValue text = JS_NewStringLen(ctx, (const char *)out, k);
        js_free(ctx, out);
        if (JS_IsException(text))
            return text;
        JSValue r = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, r, 0, text);
        JS_SetPropertyUint32(ctx, r, 1, JS_NewInt64(ctx, (int64_t)i));
        return r;
    }

    static int b64(int c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }

    //  atob(text) -> a string of bytes, or null when the text is not base64.
    static JSValue atob(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t len = 0;
        const char *s = argc ? JS_ToCStringLen(ctx, &len, argv[0]) : nullptr;
        if (!s)
            return JS_EXCEPTION;
        uint8_t *out = (uint8_t *)js_malloc(ctx, len * 2 + 1);
        if (!out)
        {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        uint32_t acc = 0;
        int bits = 0, chars = 0, pad = 0;
        size_t k = 0;
        bool bad = false;
        for (size_t i = 0; i < len && !bad; i++)
        {
            int c = (unsigned char)s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
                continue;
            if (c == '=')
            {
                pad++;
                continue;
            }
            int v = b64(c);
            if (v < 0 || pad)
            {
                bad = true;
                break;
            }
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            chars++;
            if (bits >= 8)
            {
                bits -= 8;
                k += putUtf8(out + k, (acc >> bits) & 0xFF);
            }
        }
        if ((chars + pad) % 4 == 1 || (pad && (chars + pad) % 4) || pad > 2 || (chars % 4 == 1))
            bad = true;
        JS_FreeCString(ctx, s);
        JSValue r = bad ? JS_NULL : JS_NewStringLen(ctx, (const char *)out, k);
        js_free(ctx, out);
        return r;
    }

    //  btoa(text) -> base64, or null when a character is above U+00FF.
    static JSValue btoa(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (!argc)
            return JS_NULL;
        JSValue str = JS_ToString(ctx, argv[0]);
        if (JS_IsException(str))
            return str;
        int64_t n = lengthOf(ctx, str);
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, str);
        JS_FreeValue(ctx, str);
        if (!s)
            return JS_EXCEPTION;
        //  Back to code units: the UTF-8 holds only 1- and 2-byte forms when
        //  every character is in Latin-1.
        uint8_t *bytes = (uint8_t *)js_malloc(ctx, (size_t)n + 1);
        size_t k = 0;
        bool bad = false;
        for (size_t i = 0; i < len && bytes; i++)
        {
            uint8_t c = (uint8_t)s[i];
            if (c < 0x80)
                bytes[k++] = c;
            else if ((c & 0xE0) == 0xC0 && i + 1 < len && c <= 0xC3)
            {
                bytes[k++] = (uint8_t)(((c & 0x1F) << 6) | (s[i + 1] & 0x3F));
                i++;
            }
            else
            {
                bad = true;
                break;
            }
        }
        JS_FreeCString(ctx, s);
        if (!bytes)
            return JS_EXCEPTION;
        if (bad)
        {
            js_free(ctx, bytes);
            return JS_NULL;
        }
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t outLen = (k + 2) / 3 * 4;
        char *out = (char *)js_malloc(ctx, outLen + 1);
        if (!out)
        {
            js_free(ctx, bytes);
            return JS_EXCEPTION;
        }
        size_t o = 0;
        for (size_t i = 0; i < k; i += 3)
        {
            uint32_t v = (uint32_t)bytes[i] << 16;
            if (i + 1 < k)
                v |= (uint32_t)bytes[i + 1] << 8;
            if (i + 2 < k)
                v |= bytes[i + 2];
            out[o++] = T[(v >> 18) & 63];
            out[o++] = T[(v >> 12) & 63];
            out[o++] = i + 1 < k ? T[(v >> 6) & 63] : '=';
            out[o++] = i + 2 < k ? T[v & 63] : '=';
        }
        JSValue r = JS_NewStringLen(ctx, out, o);
        js_free(ctx, out);
        js_free(ctx, bytes);
        return r;
    }

    static JSValue randomFill(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (!argc)
            return JS_UNDEFINED;
        size_t off = 0, len = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
        if (JS_IsException(buf))
            return buf;
        size_t total;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
        if (base && platform().entropy)
            platform().entropy(base + off, len);
        JS_FreeValue(ctx, buf);
        return JS_DupValue(ctx, argv[0]);
    }
};

static const JSCFunctionListEntry NATIVES[] = {
    JS_CFUNC_DEF("log", 2, Natives::log),
    JS_CFUNC_DEF("reportError", 1, Natives::reportError),
    JS_CFUNC_DEF("now", 0, Natives::now),
    JS_CFUNC_DEF("setTimer", 4, Natives::setTimer),
    JS_CFUNC_DEF("clearTimer", 1, Natives::clearTimer),
    JS_CFUNC_DEF("requestFrame", 1, Natives::requestFrame),
    JS_CFUNC_DEF("cancelFrame", 1, Natives::cancelFrame),
    JS_CFUNC_DEF("open", 6, Natives::open),
    JS_CFUNC_DEF("close", 1, Natives::close),
    JS_CFUNC_DEF("utf8Encode", 1, Natives::utf8Encode),
    JS_CFUNC_DEF("utf8Decode", 3, Natives::utf8Decode),
    JS_CFUNC_DEF("atob", 1, Natives::atob),
    JS_CFUNC_DEF("btoa", 1, Natives::btoa),
    JS_CFUNC_DEF("randomFill", 1, Natives::randomFill),
};

bool Engine::installApis()
{
    EngineState *st = stateOf(this);
    JSValue n = JS_NewObject(ctx_);
    if (JS_IsException(n))
        return false;
    JS_SetPropertyFunctionList(ctx_, n, NATIVES, (int)(sizeof(NATIVES) / sizeof(NATIVES[0])));
    JS_SetPropertyStr(ctx_, n, "baseUrl", JS_NewString(ctx_, ""));
    st->natives = JS_DupValue(ctx_, n);
    JSValue global = JS_GetGlobalObject(ctx_);
    JS_DefinePropertyValueStr(ctx_, global, "__jsr2", n, JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx_, global);
    if (!evalBinary(jsr2_prelude, jsr2_prelude_size, "prelude"))
        return false;
    return true;
}

void Engine::setBaseUrl(const char *url)
{
    EngineState *st = stateOf(this);
    if (ctx_ && st)
        JS_SetPropertyStr(ctx_, st->natives, "baseUrl", JS_NewString(ctx_, url ? url : ""));
}

bool Engine::start(const Limits &limits)
{
    stop();
    limits_ = limits;
    heap_.setLimit(limits.heapBytes);
    EngineState *st = claimState(this);
    if (!st)
        return false;
    st->origin = platform().monotonicMs();
    rt_ = JS_NewRuntime2(&MALLOC, &heap_);
    if (!rt_)
    {
        stop();
        return false;
    }
    JS_SetMemoryLimit(rt_, limits.heapBytes);
    JS_SetMaxStackSize(rt_, limits.stackBytes);
    JS_SetInterruptHandler(rt_, interrupt, this);
    JS_SetHostPromiseRejectionTracker(rt_, rejection, this);
    ctx_ = JS_NewContext(rt_);
    if (!ctx_)
    {
        stop();
        return false;
    }
    JS_SetContextOpaque(ctx_, this);
    if (!installApis())
    {
        char why[sizeof(error_)];
        memcpy(why, error_, sizeof(why));
        stop();
        memcpy(error_, why, sizeof(error_));
        return false;
    }
    error_[0] = 0;
    errors_ = 0;
    return true;
}

void Engine::stop()
{
    EngineState *st = stateOf(this);
    if (ctx_)
    {
        for (int i = 0; i < nTimers_; i++)
        {
            JS_FreeValue(ctx_, timers_[i].fn);
            JS_FreeValue(ctx_, timers_[i].args);
        }
        for (int i = 0; i < nFrames_; i++)
            JS_FreeValue(ctx_, frames_[i].fn);
        for (int i = 0; i < nRequests_; i++)
        {
            if (transport_)
                transport_->close(requests_[i].handle);
            JS_FreeValue(ctx_, requests_[i].cb);
        }
        if (st)
        {
            for (int i = 0; i < st->nRejections; i++)
            {
                JS_FreeValue(ctx_, st->rejections[i].promise);
                JS_FreeValue(ctx_, st->rejections[i].reason);
            }
            st->nRejections = 0;
            JS_FreeValue(ctx_, st->natives);
            st->natives = JS_UNDEFINED;
        }
        JS_FreeContext(ctx_);
    }
    else if (transport_)
        for (int i = 0; i < nRequests_; i++)
            transport_->close(requests_[i].handle);
    if (rt_)
    {
        JS_RunGC(rt_);
        JS_FreeRuntime(rt_);
    }
    ctx_ = nullptr;
    rt_ = nullptr;
    //  The arrays came from the heap; release() takes them with everything.
    timers_ = nullptr;
    frames_ = nullptr;
    requests_ = nullptr;
    nTimers_ = capTimers_ = nFrames_ = capFrames_ = nRequests_ = capRequests_ = 0;
    depth_ = 0;
    deadline_ = 0;
    heap_.release();
    dropState(this);
}

} // namespace jsr2
