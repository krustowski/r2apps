//
//  run.cpp --- libjsr2's host tests.
//
//      run harness.js a.test.js b.test.js ...
//
//  Each test file runs in an engine of its own, after harness.js, with a
//  scripted network (Fake below) on http://test/.  The harness reports each
//  test through __report(name, failure) and the end through __finish(); a
//  file that does not finish in ten seconds fails.
//
#include "jsr2.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

uint64_t monotonic()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
double epoch()
{
    using namespace std::chrono;
    return (double)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
void entropy(uint8_t *out, size_t n)
{
    static uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; i++)
    {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        out[i] = (uint8_t)x;
    }
}
std::string g_log;
bool g_verbose = false;
void logLine(int level, const char *text, size_t len)
{
    g_log.append(text, len);
    g_log += '\n';
    if (g_verbose || level == jsr2::Error)
        std::fprintf(stderr, "    [%d] %.*s\n", level, (int)len, text);
}

//  A scripted network.  Every address on http://test/ answers with a list
//  of events, each held back until its time has come.
struct Fake : jsr2::Transport
{
    struct Step
    {
        jsr2::Transport::Event::Kind kind;
        uint64_t after; // ms after open()
        int status;
        std::string text, headers, url;
    };
    struct Conn
    {
        bool open = false;
        uint64_t opened = 0;
        size_t next = 0;
        std::vector<Step> steps;
        std::string data; // the current DATA
    };
    std::vector<Conn> conns;
    int sseConnections = 0;
    std::string lastSseHeaders;

    static Step H(int status, const char *type, uint64_t after = 0, std::string url = "")
    {
        return {jsr2::Transport::Event::HEADERS, after, status, "", std::string("content-type: ") + type + "\n", url};
    }
    static Step D(std::string s, uint64_t after = 0) { return {jsr2::Transport::Event::DATA, after, 0, s, "", ""}; }
    static Step E(uint64_t after = 0) { return {jsr2::Transport::Event::END, after, 0, "", "", ""}; }
    static Step F(const char *why, uint64_t after = 0) { return {jsr2::Transport::Event::FAIL, after, 0, why, "", ""}; }

    int open(const Request &r, const char **error) override
    {
        std::string url = r.url, method = r.method;
        std::string body((const char *)r.body, r.body ? r.bodyLen : 0);
        if (url.rfind("http://test/", 0) != 0)
        {
            *error = "only http://test/ exists here";
            return -1;
        }
        std::string path = url.substr(11);
        Conn c;
        c.open = true;
        c.opened = monotonic();
        auto &s = c.steps;
        if (path == "/hello.txt")
            s = {H(200, "text/plain"), D("hello "), D("world", 5), E(5)};
        else if (path == "/data.json")
            s = {H(200, "application/json"), D("{\"a\":1,\"list\":[1,2,"), D("3],\"text\":\"\xc5\xbelu\xc5\xa5"), D("ou\xc4\x8dk\xc3\xbd\"}"), E()};
        else if (path == "/echo")
        {
            std::string j = "{\"method\":\"" + method + "\",\"headers\":" + quote(r.headers) + ",\"body\":" + quote(body) + "}";
            s = {H(200, "application/json"), D(j), E()};
        }
        else if (path == "/redirect")
            s = {H(200, "text/plain", 0, "http://test/hello.txt"), D("hello world"), E()};
        else if (path == "/missing")
            s = {H(404, "text/plain"), D("not here"), E()};
        else if (path == "/broken")
            s = {F("connection refused")};
        else if (path == "/slow")
            s = {H(200, "text/plain"), D("first", 10), E(60000)};
        else if (path == "/sse")
        {
            sseConnections++;
            lastSseHeaders = r.headers;
            if (sseConnections == 1)
                //  Split inside lines, inside a CRLF and inside a two-byte
                //  character; then the server drops the connection.
                s = {H(200, "text/event-stream"), D(": comment\n\nretry: 50\ndata: one\n\n"),
                     D("event: tick\ndata: t\xc3", 10), D("\xa9\ndata: second line\r", 20), D("\n\r\nid: 7\ndata: three\n\n", 30),
                     D("data: unterminated", 40), E(50)};
            else
                s = {H(200, "text/event-stream"), D("data: again\n\n"), E(60000)};
        }
        else if (path == "/not-sse")
            s = {H(200, "text/html"), D("<p>"), E()};
        else
            s = {H(404, "text/plain"), E()};
        conns.push_back(c);
        return (int)conns.size() - 1;
    }

    bool next(int h, Event &e) override
    {
        if (h < 0 || (size_t)h >= conns.size() || !conns[h].open)
            return false;
        Conn &c = conns[h];
        if (c.next >= c.steps.size())
            return false;
        Step &s = c.steps[c.next];
        if (monotonic() - c.opened < s.after)
            return false;
        c.next++;
        e.kind = s.kind;
        e.status = s.status;
        e.statusText = s.status == 200 ? "OK" : "Not Found";
        e.headers = s.headers.c_str();
        e.url = s.url.c_str();
        c.data = s.text;
        e.data = (const uint8_t *)c.data.data();
        e.len = c.data.size();
        e.error = s.text.c_str();
        return true;
    }

    void close(int h) override
    {
        if (h >= 0 && (size_t)h < conns.size())
            conns[h].open = false;
    }

    int openCount() const
    {
        int n = 0;
        for (auto &c : conns)
            n += c.open;
        return n;
    }

    static std::string quote(const std::string &s)
    {
        std::string o = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\')
                o += '\\', o += c;
            else if (c == '\n')
                o += "\\n";
            else if (c == '\r')
                o += "\\r";
            else
                o += c;
        }
        return o + "\"";
    }
};

std::string readFile(const char *path)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
    {
        std::perror(path);
        std::exit(2);
    }
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    std::fclose(f);
    return s;
}

int g_pass, g_fail;
bool g_finished;
Fake *g_fake;

JSValue report(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *why = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
    if (why && *why)
    {
        g_fail++;
        std::printf("  FAIL %s\n       %s\n", name, why);
    }
    else
    {
        g_pass++;
        if (g_verbose)
            std::printf("  ok   %s\n", name);
    }
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, why);
    return JS_UNDEFINED;
}

JSValue finish(JSContext *, JSValueConst, int, JSValueConst *)
{
    g_finished = true;
    return JS_UNDEFINED;
}

//  For the tests: what the fake network saw, and the engine's own state.
JSValue probe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    const char *what = argc ? JS_ToCString(ctx, argv[0]) : nullptr;
    JSValue r = JS_UNDEFINED;
    jsr2::Engine *e = jsr2::Engine::from(ctx);
    if (what && !strcmp(what, "sseConnections"))
        r = JS_NewInt32(ctx, g_fake->sseConnections);
    else if (what && !strcmp(what, "lastSseHeaders"))
        r = JS_NewString(ctx, g_fake->lastSseHeaders.c_str());
    else if (what && !strcmp(what, "openRequests"))
        r = JS_NewInt32(ctx, g_fake->openCount());
    else if (what && !strcmp(what, "log"))
        r = JS_NewString(ctx, g_log.c_str());
    else if (what && !strcmp(what, "error"))
        r = JS_NewString(ctx, e->error());
    else if (what && !strcmp(what, "heap"))
        r = JS_NewInt64(ctx, (int64_t)e->heapBytes());
    JS_FreeCString(ctx, what);
    return r;
}

bool runFile(const std::string &harness, const char *path)
{
    std::printf("%s\n", path);
    jsr2::Engine e;
    Fake fake;
    g_fake = &fake;
    g_log.clear();
    g_finished = false;
    int failBefore = g_fail;
    jsr2::Limits limits;
    limits.taskMs = 500;
    if (!e.start(limits))
    {
        std::printf("  FAIL cannot start the engine: %s\n", e.error());
        g_fail++;
        return false;
    }
    e.setTransport(&fake);
    e.setBaseUrl("http://test/dir/page.html");
    JSContext *ctx = e.context();
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__report", JS_NewCFunction(ctx, report, "__report", 2));
    JS_SetPropertyStr(ctx, global, "__finish", JS_NewCFunction(ctx, finish, "__finish", 0));
    JS_SetPropertyStr(ctx, global, "__probe", JS_NewCFunction(ctx, probe, "__probe", 1));
    JS_FreeValue(ctx, global);
    std::string src = readFile(path);
    if (!e.eval(harness.c_str(), harness.size(), "harness.js") || !e.eval(src.c_str(), src.size(), path))
    {
        std::printf("  FAIL %s\n", e.error());
        g_fail++;
        return false;
    }
    e.eval("__run()", 7, "run");
    uint64_t until = monotonic() + 10000;
    while (!g_finished && monotonic() < until)
    {
        e.tick();
        if (e.wantsFrame())
            e.frame((double)monotonic());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!g_finished)
    {
        std::printf("  FAIL did not finish (last error: %s)\n", e.error());
        g_fail++;
    }
    e.stop();
    if (e.heapBytes())
    {
        std::printf("  FAIL heap not released\n");
        g_fail++;
    }
    return g_fail == failBefore;
}

} // namespace

int main(int argc, char **argv)
{
    jsr2::setPlatform({[](size_t n) { return std::malloc(n); }, [](void *p) { std::free(p); }, monotonic, epoch, entropy,
                       logLine});
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-v"))
    {
        g_verbose = true;
        first = 2;
    }
    if (argc <= first)
    {
        std::fprintf(stderr, "usage: run [-v] harness.js test.js...\n");
        return 2;
    }
    std::string harness = readFile(argv[first]);
    for (int i = first + 1; i < argc; i++)
        runFile(harness, argv[i]);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
