//
//  js.elf --- JavaScript on r2, without a window.
//
//      js [-o FILE] [-t TASK_MS] script.js [more.js ...] [-- args]
//
//  Runs the scripts in one engine, in order, then keeps the event loop going
//  (timers, promises) until nothing is left to wait for, r2.exit() is
//  called, or a minute passes.  console output goes to the kernel console,
//  and with -o also into FILE, which is rewritten as it grows (so a test on
//  a floppy can be read from the host while it runs).  -t sets how long one
//  task may run before it is stopped (5000 ms).
//
//  Besides the Web APIs of libjsr2 a script has `r2`:
//      r2.args            the arguments after the script names' "--", if any
//      r2.exit(code)      stop after the current task
//      r2.readFile(path)  the file as a string, or null
//      r2.writeFile(path, text)   true when written
//      r2.lastError()     the engine's last uncaught error, "" for none
//      r2.log()           everything logged so far (the last 16 KiB)
//      r2.heap()          bytes the engine holds
//
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/io.hpp>
#include <r2/process.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>
#include <r2/types.hpp>
#include "jsr2.h"

R2_HEAP_ARENA_KERNEL(1024 * 1024)

namespace {

const size_t LOG_KEEP = 16 * 1024;
char g_log[LOG_KEEP + 1];
size_t g_logLen = 0;
char g_outPath[64] = {};
//  What is still to go into the -o file, which grows by appends (syscall
//  0x3a): a whole-file write is one 512-byte sector on r2.
char g_out[8192];
size_t g_outLen = 0;
uint64_t g_outOffset = 0, g_outWritten = 0;
bool g_exit = false;
int g_code = 0;
jsr2::Engine *g_engine = nullptr;

void flushOut(bool force)
{
    if (!g_outPath[0] || !g_outLen)
        return;
    uint64_t now = r2::ticks();
    if (!force && g_outLen < sizeof(g_out) / 2 && now - g_outWritten < 500)
        return;
    int64_t n = r2::fs::write_at(r2::string_view(g_outPath), r2::const_byte_span((const uint8_t *)g_out, g_outLen), g_outOffset);
    if (n > 0)
        g_outOffset += (uint64_t)n;
    g_outLen = 0;
    g_outWritten = now;
}

void logLine(int level, const char *text, size_t len)
{
    static const char *prefix[] = {"", "", "warning: ", "error: ", ""};
    const char *p = level >= 0 && level <= 4 ? prefix[level] : "";
    char line[600];
    size_t n = 0;
    for (const char *q = p; *q && n < sizeof(line) - 2; q++)
        line[n++] = *q;
    for (size_t i = 0; i < len && n < sizeof(line) - 2; i++)
        line[n++] = text[i];
    line[n++] = '\n';
    line[n] = 0;
    r2::print(r2::string_view(line, n));
    if (g_logLen + n > LOG_KEEP)
    {
        size_t drop = g_logLen + n - LOG_KEEP;
        if (drop > g_logLen)
            drop = g_logLen;
        memmove(g_log, g_log + drop, g_logLen - drop);
        g_logLen -= drop;
    }
    memcpy(g_log + g_logLen, line, n);
    g_logLen += n;
    g_log[g_logLen] = 0;
    if (g_outPath[0])
    {
        if (g_outLen + n > sizeof(g_out))
            flushOut(true);
        memcpy(g_out + g_outLen, line, n);
        g_outLen += n;
        flushOut(false);
    }
}

//  The RTC has whole seconds; Date.now() is the second it read at the start
//  plus the ticks since, so it moves in milliseconds and never backwards.
double epochMs()
{
    static double base = -1;
    static uint64_t baseTicks = 0;
    if (base < 0)
    {
        r2::RtcTime t;
        memset(&t, 0, sizeof(t));
        base = 0;
        baseTicks = r2::ticks();
        if (r2::raw_syscall(r2::Sys::Rtc, 0x01, (int64_t)&t) != 0)
            return 0;
        int y = t.year < 100 ? t.year + 2000 : t.year, m = t.month, d = t.day;
        if (y < 2024 || m < 1 || m > 12 || d < 1 || d > 31)
            return 0;
        y -= m <= 2;
        long era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = (unsigned)(y - era * 400);
        unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        long days = era * 146097 + (long)doe - 719468;
        base = ((double)days * 86400.0 + t.hours * 3600.0 + t.minutes * 60.0 + t.seconds) * 1000.0;
    }
    return base ? base + (double)(r2::ticks() - baseTicks) : 0;
}

//  RDRAND when the CPU has it, mixed with the tick count; the engine only
//  seeds Math.random and crypto.getRandomValues with it.
bool rdrand(uint64_t &v)
{
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    return ok;
}

void entropy(uint8_t *out, size_t n)
{
    static int have = -1;
    if (have < 0)
    {
        unsigned a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        have = (c >> 30) & 1;
    }
    static uint64_t x = 0;
    x ^= r2::ticks() * 0x9E3779B97F4A7C15ull + 1;
    for (size_t i = 0; i < n; i += 8)
    {
        uint64_t v = 0;
        if (!have || !rdrand(v))
        {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            v = x;
        }
        for (size_t k = 0; k < 8 && i + k < n; k++)
            out[i + k] = (uint8_t)(v >> (8 * k));
    }
}

//  Engine pages: the kernel's shared heap first, the program's arena when
//  that is full.  A header says which, for the free.
const uint64_t FROM_KERNEL = 0x4b45524e454cull, FROM_ARENA = 0x4152454e41ull;

void *pageAlloc(size_t n)
{
    uint64_t *p = (uint64_t *)r2::heap::kernel_allocate(n + 16);
    uint64_t tag = FROM_KERNEL;
    if (!p)
    {
        p = (uint64_t *)r2::heap::allocate(n + 16);
        tag = FROM_ARENA;
    }
    if (!p)
        return nullptr;
    p[0] = tag;
    p[1] = n;
    return p + 2;
}

void pageFree(void *q)
{
    if (!q)
        return;
    uint64_t *p = (uint64_t *)q - 2;
    if (p[0] == FROM_KERNEL)
        r2::heap::kernel_deallocate(p);
    else
        r2::heap::deallocate(p);
}

JSValue nExit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    g_code = 0;
    if (argc)
        JS_ToInt32(ctx, &g_code, argv[0]);
    g_exit = true;
    return JS_UNDEFINED;
}

JSValue nReadFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    const char *path = argc ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (!path)
        return JS_EXCEPTION;
    auto data = r2::fs::read(r2::string_view(path), 4u << 20);
    JS_FreeCString(ctx, path);
    if (!data)
        return JS_NULL;
    return JS_NewStringLen(ctx, (const char *)data->data(), data->size());
}

JSValue nWriteFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_FALSE;
    const char *path = JS_ToCString(ctx, argv[0]);
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, argv[1]);
    bool ok = false;
    if (path && text)
    {
        r2::fs::remove(r2::string_view(path));
        ok = !len || r2::fs::write_at(r2::string_view(path), r2::const_byte_span((const uint8_t *)text, len), 0) == (int64_t)len;
    }
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

JSValue nLastError(JSContext *ctx, JSValueConst, int, JSValueConst *) { return JS_NewString(ctx, g_engine->error()); }
JSValue nLog(JSContext *ctx, JSValueConst, int, JSValueConst *) { return JS_NewStringLen(ctx, g_log, g_logLen); }
JSValue nHeap(JSContext *ctx, JSValueConst, int, JSValueConst *) { return JS_NewInt64(ctx, (int64_t)g_engine->heapBytes()); }

const JSCFunctionListEntry R2_FUNCS[] = {
    JS_CFUNC_DEF("exit", 1, nExit),
    JS_CFUNC_DEF("readFile", 1, nReadFile),
    JS_CFUNC_DEF("writeFile", 2, nWriteFile),
    JS_CFUNC_DEF("lastError", 0, nLastError),
    JS_CFUNC_DEF("log", 0, nLog),
    JS_CFUNC_DEF("heap", 0, nHeap),
};

void say(const char *s) { r2::print(r2::string_view(s)); }

} // namespace

int main()
{
    jsr2::setPlatform({pageAlloc, pageFree, [] { return (uint64_t)r2::ticks(); }, epochMs, entropy, logLine});
    int argc = r2::arg_count(), first = 1;
    uint32_t taskMs = 5000;
    for (;;)
    {
        if (argc > first + 1 && r2::arg(first) == r2::string_view("-o"))
        {
            r2::string_view o = r2::arg(first + 1);
            size_t n = o.size() < sizeof(g_outPath) - 1 ? o.size() : sizeof(g_outPath) - 1;
            memcpy(g_outPath, o.data(), n);
            g_outPath[n] = 0;
            first += 2;
        }
        else if (argc > first + 1 && r2::arg(first) == r2::string_view("-t"))
        {
            taskMs = 0;
            for (char c : r2::arg(first + 1))
                if (c >= '0' && c <= '9')
                    taskMs = taskMs * 10 + (uint32_t)(c - '0');
            first += 2;
        }
        else
            break;
    }
    if (first >= argc)
    {
        say("usage: js [-o FILE] [-t TASK_MS] script.js [more.js ...] [-- args]\n");
        return 2;
    }
    if (g_outPath[0])
        r2::fs::remove(r2::string_view(g_outPath));
    jsr2::Engine engine;
    g_engine = &engine;
    jsr2::Limits limits;
    limits.heapBytes = 16u << 20;
    limits.taskMs = taskMs ? taskMs : 5000;
    if (!engine.start(limits))
    {
        say("js: cannot start the engine: ");
        say(engine.error());
        say("\n");
        return 1;
    }
    JSContext *ctx = engine.context();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue r2obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, r2obj, R2_FUNCS, (int)(sizeof(R2_FUNCS) / sizeof(R2_FUNCS[0])));
    JSValue args = JS_NewArray(ctx);
    int scriptsEnd = argc;
    for (int i = first; i < argc; i++)
        if (r2::arg(i) == r2::string_view("--"))
        {
            scriptsEnd = i;
            for (int k = i + 1; k < argc; k++)
                JS_SetPropertyUint32(ctx, args, (uint32_t)(k - i - 1), JS_NewStringLen(ctx, r2::arg(k).data(), r2::arg(k).size()));
            break;
        }
    JS_SetPropertyStr(ctx, r2obj, "args", args);
    JS_SetPropertyStr(ctx, global, "r2", r2obj);
    JS_SetPropertyStr(ctx, global, "print", JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "console"), "log"));
    JS_FreeValue(ctx, global);

    int status = 0;
    for (int i = first; i < scriptsEnd && !g_exit; i++)
    {
        char path[96];
        r2::string_view a = r2::arg(i);
        size_t n = a.size() < sizeof(path) - 1 ? a.size() : sizeof(path) - 1;
        memcpy(path, a.data(), n);
        path[n] = 0;
        auto src = r2::fs::read(r2::string_view(path), 4u << 20);
        if (!src)
        {
            say("js: cannot read ");
            say(path);
            say("\n");
            status = 1;
            break;
        }
        if (!src->push_back(0)) // QuickJS reads up to a terminator
        {
            say("js: no memory for ");
            say(path);
            say("\n");
            status = 1;
            break;
        }
        if (!engine.eval((const char *)src->data(), src->size() - 1, path))
            status = 1;
    }
    uint64_t until = r2::ticks() + 60000;
    while (!g_exit && engine.busy() && r2::ticks() < until)
    {
        engine.tick();
        if (engine.wantsFrame())
            engine.frame((double)r2::ticks());
        flushOut(false);
        r2::sleep(2);
    }
    engine.stop();
    flushOut(true);
    return g_exit ? g_code : status;
}
