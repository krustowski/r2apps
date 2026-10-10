#pragma once
//
//  libjsr2 --- a JavaScript runtime for r2 programs.
//
//  QuickJS (ES2023 and later, ../third_party/quickjs), an event loop, and the
//  Web APIs that need no document: timers, microtasks, animation frames,
//  console, events, URL, TextEncoder/TextDecoder, atob/btoa, crypto random
//  values, fetch, XMLHttpRequest and EventSource.  A browser adds its DOM on
//  top (r2web does); a program without a window runs scripts as they are
//  (cli/, js.elf).
//
//  The program supplies the machine: memory, clocks, entropy, a log, and for
//  the network APIs a Transport.  Nothing here blocks or makes a syscall of
//  its own, so the same code runs on r2 and in the host tests.
//
//  One thread.  Callbacks into the page run as tasks from Engine::tick(),
//  which the program calls from its idle loop; every task is followed by a
//  microtask checkpoint, as in a browser.
//
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

namespace jsr2 {

//  What the machine provides.  Every hook must be set.
struct Platform
{
    //  Large blocks for the engine's heap: chunks of Heap::ChunkBytes and the
    //  occasional bigger block.  nullptr when there is no memory.
    void *(*pageAlloc)(size_t n);
    void (*pageFree)(void *p);
    //  Milliseconds from any fixed point, for timers and time limits.
    uint64_t (*monotonicMs)();
    //  Milliseconds since 1970 (UTC) for Date; 0 when the clock is not known.
    double (*epochMs)();
    //  Seeds Math.random and fills crypto.getRandomValues.
    void (*entropy)(uint8_t *out, size_t n);
    //  A line of console output (no newline).  Levels: Log, Info, ...
    void (*log)(int level, const char *text, size_t len);
};
enum LogLevel { Log = 0, Info, Warn, Error, Debug };
void setPlatform(const Platform &p);
const Platform &platform();

//
//  The memory one engine runs in.  QuickJS keeps blocks of up to 512 bytes in
//  4 KiB arenas of its own, so what comes here is arenas and bigger blocks.
//  They are cut from ChunkBytes chunks by size class and come back to their
//  class's free list; anything over MaxClassBytes is a page block of its
//  own.  release() gives every chunk back at once, so a page's script memory
//  is gone with the page whatever the script left behind.
//
class Heap
{
public:
    static const size_t ChunkBytes = 256 * 1024;
    static const size_t MaxClassBytes = 64 * 1024;

    explicit Heap(size_t limit = 8u << 20) : limit_(limit) {}
    ~Heap() { release(); }
    Heap(const Heap &) = delete;
    Heap &operator=(const Heap &) = delete;

    void *alloc(size_t n);
    void *realloc(void *p, size_t n);
    void free(void *p);
    size_t usable(const void *p) const;
    void release();

    void setLimit(size_t n) { limit_ = n; }
    size_t limit() const { return limit_; }
    //  Taken from the platform: chunks and big blocks.
    size_t reserved() const { return reserved_; }
    //  Handed out, in whole size classes.
    size_t used() const { return used_; }

private:
    struct Chunk;
    struct Big;
    static const int Classes = 48;
    Chunk *chunks_ = nullptr;
    Big *bigs_ = nullptr;
    void *free_[Classes] = {};
    uint8_t *bump_ = nullptr, *bumpEnd_ = nullptr;
    size_t limit_, reserved_ = 0, used_ = 0;
    void *carve(int cls);
};

//
//  The network under fetch, XMLHttpRequest and EventSource.  The program
//  implements it on its own stack (r2web: on web::Loader).  open() starts a
//  request; the engine then asks next() on every tick until END or FAIL.
//
class Transport
{
public:
    virtual ~Transport() {}

    struct Request
    {
        const char *method;   // "GET", "POST", ...
        const char *url;      // absolute
        const char *headers;  // "Name: value\r\n" lines; may be empty
        const uint8_t *body;
        size_t bodyLen;
        bool stream;          // body pieces as they arrive (EventSource)
    };

    struct Event
    {
        enum Kind { NONE, HEADERS, DATA, END, FAIL } kind = NONE;
        int status = 0;
        const char *statusText = "";
        const char *headers = "";  // "name: value\n" lines, names lower case
        const char *url = "";      // the final address, after redirects
        const uint8_t *data = nullptr;
        size_t len = 0;            // DATA: valid until the next call
        const char *error = "";    // FAIL
    };

    //  A handle, or -1 with *error saying why.
    virtual int open(const Request &r, const char **error) = 0;
    //  The next thing that happened on h; false when nothing has.
    virtual bool next(int h, Event &e) = 0;
    //  Done with h, finished or not.
    virtual void close(int h) = 0;
};

struct Limits
{
    size_t heapBytes = 8u << 20;
    //  One task (a script, a callback, its microtasks) may run this long; then
    //  it is stopped with an uncatchable error and the page goes on.
    uint32_t taskMs = 1000;
    //  Native stack the engine may use below where a task starts.
    size_t stackBytes = 160u << 10;
};

class Engine
{
public:
    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    //  The runtime, a context with the standard library, and the Web APIs.
    bool start(const Limits &limits = Limits());
    void stop();
    bool running() const { return ctx_ != nullptr; }
    JSContext *context() const { return ctx_; }
    JSRuntime *runtime() const { return rt_; }
    void *opaque = nullptr; // the embedder's

    //  Relative fetch/XHR/EventSource addresses resolve against this.
    void setBaseUrl(const char *url);
    void setTransport(Transport *t) { transport_ = t; }

    //  A task.  False on an uncaught exception or a timeout; error() says
    //  which.  `flags` are JS_EVAL_* (JS_EVAL_TYPE_MODULE for a module).
    bool eval(const char *src, size_t len, const char *name, int flags = 0);
    //  Compiled bytecode (JS_WriteObject, tools/jsr2c), run as a task.  The
    //  engine reads it in place: buf must stay valid while it runs.  The
    //  script's value goes to *result when asked for (free it).
    bool evalBinary(const uint8_t *buf, size_t len, const char *name, JSValue *result = nullptr);
    //  A call as a task.  *result, when asked for, must be freed.
    bool call(JSValueConst fn, JSValueConst self, int argc, JSValueConst *argv, JSValue *result = nullptr);

    //  The loop.  tick() runs the timers that are due and the network's news,
    //  each as a task.  frame() runs requestAnimationFrame callbacks.
    void tick();
    void frame(double timestamp);
    bool wantsFrame() const { return nFrames_ > 0; }
    //  Something is still to come: a timer, a frame callback, a request.
    bool busy() const;
    //  When the next timer is due (monotonic ms); UINT64_MAX when none is.
    uint64_t nextTimer() const;

    //  The last uncaught error, "" when there was none since clearError().
    const char *error() const { return error_; }
    void clearError() { error_[0] = 0; }
    int errorCount() const { return errors_; }
    size_t heapBytes() const { return heap_.reserved(); }
    bool timedOut() const { return timedOut_; }

    //  For embedders' natives: a task run from inside one (an event handler
    //  calling back into the page) shares the outer task's time.
    bool inTask() const { return depth_ > 0; }
    //  Reports an exception as a task does: takes the pending one.
    void reportException();
    //  The engine an embedder's native was called in.
    static Engine *from(JSContext *ctx);

private:
    struct Timer;
    struct Frame;
    struct Request;
    friend struct Natives;

    Heap heap_;
    Limits limits_;
    JSRuntime *rt_ = nullptr;
    JSContext *ctx_ = nullptr;
    Transport *transport_ = nullptr;

    Timer *timers_ = nullptr;
    int nTimers_ = 0, capTimers_ = 0, nextTimerId_ = 1;
    Frame *frames_ = nullptr;
    int nFrames_ = 0, capFrames_ = 0, nextFrameId_ = 1;
    Request *requests_ = nullptr;
    int nRequests_ = 0, capRequests_ = 0, nextRequestId_ = 1;

    int depth_ = 0;
    uint64_t deadline_ = 0;
    bool timedOut_ = false;
    char error_[256] = {};
    int errors_ = 0;

    bool begin();
    bool end(bool ok);
    void microtasks();
    bool installApis();
    void setError(const char *s);
    static int interrupt(JSRuntime *rt, void *opaque);
    static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, int handled, void *opaque);

    void pollRequests();
    void runTimers();
};

//  The text of a JS value for messages: strings as they are, errors as
//  "Name: message", anything else as String() makes it.  Fills out (cap
//  bytes, always terminated).
void describe(JSContext *ctx, JSValueConst v, char *out, size_t cap);

} // namespace jsr2
