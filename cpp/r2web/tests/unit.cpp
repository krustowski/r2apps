//
//  unit.cpp --- r2web's host checks: the HTML parser, the DOM and its
//  bridge to the browser (script.h), resource limits, and the shared frames
//  of host.h.  The network is not here (no ScriptNet on the host): libjsr2's
//  own tests cover fetch and EventSource with a scripted one.
//
#include "../script.h"
#include "../htmlparse.h"
#include "../host.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace web {
void *alloc(size_t n) { return std::malloc(n); }
void *realloc(void *p, size_t n) { return std::realloc(p, n); }
void free(void *p) { std::free(p); }
void *big_alloc(size_t n) { return std::malloc(n); }
void *big_realloc(void *p, size_t n) { return std::realloc(p, n); }
void big_free(void *p) { std::free(p); }
uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace web

static int failures = 0;
static void check(bool value, const char *what, const std::string &detail = "")
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        if (!detail.empty())
            std::fprintf(stderr, "      %s\n", detail.substr(0, 600).c_str());
        failures++;
    }
}

//  A page with scripts, run the way the browser runs it.
struct Page
{
    web::ScriptPage js;
    web::Buf html{true};
    std::string out;

    bool load(const char *text, const char *url = "http://example.test/dir/index.html")
    {
        web::Buf b{true};
        b.appendStr(text);
        if (!js.start(b, "utf-8", url))
            return false;
        for (int i = 0; i < js.count(); i++)
            if (!js.src(i)[0])
            {
                const char *code = js.source(i);
                js.run(i, code, std::strlen(code));
            }
        js.parsed();
        settle();
        return true;
    }
    void settle(int ms = 30)
    {
        auto until = web::now_ms() + (uint64_t)ms;
        do
        {
            js.tick();
            if (js.wantsFrame())
                js.frame((double)web::now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (web::now_ms() < until);
        render();
    }
    const std::string &render()
    {
        char title[128];
        if (js.render(html, title, sizeof(title)))
            out.assign((const char *)html.data, html.len);
        return out;
    }
    bool has(const char *s) const { return out.find(s) != std::string::npos; }
    //  The r2:N handle the render gave the element with this id.
    int node(const char *id) const
    {
        std::string key = std::string("id=\"") + id + "\"";
        size_t at = out.find(key);
        if (at == std::string::npos)
            return -1;
        size_t open = out.rfind('<', at), close = out.find('>', at);
        size_t r = out.find("r2:", open);
        if (r == std::string::npos || r > close)
        {
            //  A listener's element is wrapped: its link is just inside.
            r = out.find("r2:", close);
            if (r == std::string::npos || r > out.find('>', close + 1))
                return -1;
        }
        return web::ScriptPage::nodeOf(out.c_str() + r);
    }
    bool eval(const char *code) { return js.eval(code); }
};

static void parser()
{
    Page p;
    check(p.load("<!doctype html><title>T &amp; t</title><p>one<p>two<ul><li>a<li>b</ul>"
                 "<table><tr><td>c<td>d</table><div id=x data-a='1&amp;2' class=\"k\">e&lt;f&#x41;&eacute;</div>"
                 "<textarea id=t>&lt;b&gt; raw</textarea><svg viewBox='0 0 1 1'><circle r=1 /></svg><b><i>z</b>q"
                 "<script>window.done = 1</script>"),
          "a page with a script starts an engine");
    p.eval("window.r = [document.querySelectorAll('p').length, document.querySelectorAll('li').length,"
           "document.querySelector('table > tbody > tr > td + td').textContent,"
           "document.getElementById('x').getAttribute('data-a'), document.getElementById('x').textContent,"
           "document.getElementById('t').value, document.querySelector('svg').getAttribute('viewBox'),"
           "document.querySelector('circle').parentNode.tagName, document.title, document.doctype.name,"
           "document.head.childNodes.length > 0, document.body.lastChild.nodeName].join('|');"
           "document.title = r;");
    p.render();
    check(p.has("<title>2|2|d|1&amp;2|e&lt;fA\xc3\xa9|&lt;b&gt; raw|0 0 1 1|svg|T &amp; t|html|true|SCRIPT</title>"),
          "implied tags, tables, entities, raw text and SVG", p.out);
    p.eval("document.title = document.body.innerHTML.replace(/<script>.*<\\/script>/, '').replace(/<svg.*<\\/svg>/, 'S')");
    p.render();
    check(p.has("&lt;p&gt;one&lt;/p&gt;&lt;p&gt;two&lt;/p&gt;&lt;ul&gt;&lt;li&gt;a&lt;/li&gt;&lt;li&gt;b&lt;/li&gt;&lt;/ul&gt;"
                "&lt;table&gt;&lt;tbody&gt;&lt;tr&gt;&lt;td&gt;c&lt;/td&gt;&lt;td&gt;d&lt;/td&gt;&lt;/tr&gt;&lt;/tbody&gt;&lt;/table&gt;"),
          "serialised structure", p.out);
    check(p.has("&lt;b&gt;&lt;i&gt;z&lt;/i&gt;&lt;/b&gt;q"), "misnested formatting", p.out);

    Page q;
    q.load("<script>document.write('<p id=w>written</p>'); var x = document.currentScript.nextSibling;"
           "document.title = x ? x.id : 'none';</script><p>after</p>");
    check(q.has("<title>w</title>") && q.has("<p id=\"w\">written</p>"), "document.write lands after its script", q.out);
}

static void dom()
{
    Page p;
    p.load("<body><ul id=list></ul><p id=msg class='a b'>hi</p><script>"
           "const ul = document.getElementById('list');"
           "for (const t of ['x', 'y', 'z']) { const li = document.createElement('li'); li.textContent = t; li.dataset.v = t; ul.append(li); }"
           "ul.children[1].remove();"
           "const m = document.querySelector('#msg'); m.classList.add('c'); m.classList.toggle('a'); m.style.color = 'red';"
           "m.insertAdjacentHTML('beforeend', ' <b>there</b>');"
           "document.body.appendChild(Object.assign(document.createElement('span'), {id: 's', textContent: [...ul.querySelectorAll('li')].map(l => l.dataset.v).join()}));"
           "const frag = document.createDocumentFragment(); frag.append('t1', document.createElement('hr')); document.body.append(frag);"
           "document.title = [ul.childElementCount, m.className, m.matches('p.b.c'), m.closest('body').tagName, getComputedStyle(m).color].join();"
           "</script></body>");
    check(p.has("<title>2,b c,true,BODY,red</title>"), "creating, removing and classes", p.out);
    check(p.has("<li data-v=\"x\">x</li><li data-v=\"z\">z</li>"), "children in order", p.out);
    check(p.has("style=\"color: red;\"") && p.has("hi <b>there</b>"), "style and insertAdjacentHTML", p.out);
    check(p.has("<span id=\"s\">x,z</span>") && p.has("t1<hr>"), "fragments and text", p.out);

    //  Observers, custom elements and template content.
    Page q;
    q.load("<template id=tp><i>from template</i></template><div id=host></div><x-greet name=World></x-greet><script>"
           "const seen = []; new MutationObserver(rs => { for (const r of rs) seen.push(r.type); document.title = seen.join(); })"
           ".observe(document.body, {childList: true, subtree: true, attributes: true});"
           "class G extends HTMLElement { static observedAttributes = ['name']; connectedCallback() { this.textContent = 'Hello ' + this.getAttribute('name'); }"
           "  attributeChangedCallback(n, o, v) { if (this.isConnected) this.textContent = 'Hello ' + v; } }"
           "customElements.define('x-greet', G);"
           "document.getElementById('host').append(document.getElementById('tp').content.cloneNode(true));"
           "document.querySelector('x-greet').setAttribute('name', 'r2');"
           "</script>");
    check(q.has("<x-greet name=\"r2\">Hello r2</x-greet>"), "custom elements upgrade and react", q.out);
    check(q.has("<div id=\"host\"><i>from template</i></div>"), "template content clones", q.out);
    check(q.has("<title>childList"), "mutation observer", q.out);
}

static void events()
{
    Page p;
    p.load("<body><a id=a href='/next'>go</a> <a id=b href='/other' onclick=\"document.title='inline:'+this.id; return false\">stay</a>"
           "<div id=card>card</div><button id=btn>press</button><input id=name value=initial><input id=cb type=checkbox>"
           "<form id=f action='/send'><input name=q id=q value=v><button id=sub>send</button></form><script>"
           "document.getElementById('card').addEventListener('click', e => { document.title = 'card ' + e.bubbles; });"
           "document.getElementById('btn').onclick = () => { document.title = 'button'; };"
           "document.body.addEventListener('click', e => { window.lastTarget = e.target.id; });"
           "document.getElementById('name').addEventListener('input', e => { document.title = 'typed ' + e.target.value; });"
           "document.getElementById('cb').addEventListener('change', e => { document.title = 'checked ' + e.target.checked; });"
           "document.getElementById('f').addEventListener('submit', e => { if (q.value === 'stop') { e.preventDefault(); document.title = 'stopped'; } });"
           "</script></body>");
    check(p.js.click(p.node("a")), "a plain link goes on to the browser");
    check(!p.js.click(p.node("b")), "return false in an inline handler cancels the link");
    p.render();
    check(p.has("<title>inline:b</title>"), "inline handler with this", p.out);
    check(p.node("card") > 0, "an element with a click listener is clickable", p.out);
    p.js.click(p.node("card"));
    p.render();
    check(p.has("<title>card true</title>"), "listener on a div", p.out);
    p.js.click(p.node("btn"));
    p.render();
    check(p.has("<title>button</title>"), "onclick property", p.out);
    p.eval("document.title = lastTarget");
    p.render();
    check(p.has("<title>btn</title>"), "clicks bubble", p.out);
    p.js.input(p.node("name"), "new text");
    p.render();
    check(p.has("<title>typed new text</title>") && p.has("value=\"new text\""), "typing reaches the DOM", p.out);
    p.js.state(p.node("cb"), true, -1);
    p.render();
    check(p.has("<title>checked true</title>") && p.has(" checked"), "checkbox state", p.out);

    web::ScriptPage::Submit s;
    check(!p.js.click(p.node("sub")), "a submit button is the DOM's to send");
    check(p.js.takeSubmit(s) && !std::strcmp(s.action, "http://example.test/send") && !s.post &&
              std::string((const char *)s.data.data, s.data.len) == "q=v",
          "form submission with the DOM's values");
    p.js.input(p.node("q"), "stop");
    check(!p.js.submitFrom(p.node("q")), "Enter in a field submits through the DOM");
    p.render();
    check(!p.js.takeSubmit(s) && p.has("<title>stopped</title>"), "submit event can cancel", p.out);

    p.eval("location.href = 'page2.html'");
    check(!std::strcmp(p.js.navigation(), "http://example.test/dir/page2.html"), "location resolves against the page",
          p.js.navigation());
    p.js.clearNavigation();
    char url[256];
    p.eval("history.pushState({a: 1}, '', '/dir/x?y=1'); document.title = location.pathname + location.search + history.state.a");
    check(p.js.takeUrl(url, sizeof(url)) && !std::strcmp(url, "http://example.test/dir/x?y=1"), "pushState moves the address");
    p.render();
    check(p.has("<title>/dir/x?y=11</title>"), "location after pushState", p.out);
    p.eval("alert('hello\\nthere')");
    char status[64];
    check(p.js.takeStatus(status, sizeof(status)) && !std::strcmp(status, "hello there"), "alert goes to the status line");
}

static void loop()
{
    Page p;
    p.load("<p id=out>0</p><script>let n = 0; const t = setInterval(() => { document.getElementById('out').textContent = ++n;"
           "if (n === 3) clearInterval(t); }, 5);"
           "requestAnimationFrame(ts => { document.title = typeof ts; });"
           "Promise.resolve().then(() => { document.body.dataset.micro = 'yes'; });"
           "document.addEventListener('DOMContentLoaded', () => { document.body.dataset.ready = document.readyState; });"
           "window.addEventListener('load', () => { document.body.dataset.loaded = document.readyState; });"
           "localStorage.setItem('k', 'v1'); document.cookie = 'c=1; path=/';"
           "</script>",
           "http://store.test/a.html");
    p.settle(80);
    check(p.has("<p id=\"out\">3</p>"), "intervals run from tick()", p.out);
    check(p.has("<title>number</title>"), "animation frames", p.out);
    check(p.has("data-micro=\"yes\"") && p.has("data-ready=\"interactive\"") && p.has("data-loaded=\"complete\""),
          "microtasks and load events", p.out);
    check(!p.js.busy(), "nothing left to wait for");
    //  Storage and cookies outlive the page, per origin.
    Page q;
    q.load("<script>document.title = localStorage.getItem('k') + document.cookie + localStorage.length</script>",
           "http://store.test/b.html");
    check(q.has("<title>v1c=11</title>"), "localStorage and cookies persist per origin", q.out);
    Page r;
    r.load("<script>document.title = String(localStorage.getItem('k'))</script>", "http://other.test/");
    check(r.has("<title>null</title>"), "another origin has its own", r.out);
}

static void limits()
{
    Page p;
    check(p.load("<p>x</p><script>for(;;){}</script><script>document.title='after'</script>"), "page with a busy loop");
    check(p.has("<title>after</title>"), "the next script still runs", p.out);
    p.eval("try { let s = 'x'; for (;;) s += s; } catch (e) { document.title = 'oom ' + (e instanceof RangeError || e instanceof InternalError); }");
    p.render();
    check(p.has("<title>oom true</title>"), "running out of memory is an exception", p.out);
    check(p.js.heapBytes() <= web::ScriptPage::HeapLimit, "heap stays within its limit");
    Page q;
    check(!q.load("<p>no scripts here</p>"), "pages without scripts start no engine");
    check(q.load("<button onclick='x()'>b</button>"), "inline handlers alone start one");
    q.js.click(q.node("b") >= 0 ? q.node("b") : 0);
    p.js.clear();
    check(p.js.heapBytes() == 0, "clear releases the engine");
    Page t;
    t.load("<script>setTimeout(() => { for(;;){} }, 1); setTimeout(() => { document.title = 'still here'; }, 5);</script>");
    t.settle(1500);
    check(std::strstr(t.js.error(), "execution limit") != nullptr, "an endless timer is stopped", t.js.error());
    check(t.has("<title>still here</title>"), "and the page goes on", t.out);
}

static void frames()
{
    using namespace r2web;
    auto *b = (HostBlock *)std::calloc(1, blockSize(64));
    b->capacity = 64;
    b->maxWidth = 8;
    b->maxHeight = 8;
    b->front = None;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t serial = 1; serial <= 100000; ++serial)
        {
            uint32_t back = load(&b->front) == 0 ? 1 : 0;
            if (!claim(&b->frames[back].state, Writing))
            {
                --serial;
                std::this_thread::yield();
                continue;
            }
            std::memset(pixels(b, back), serial & 255, 64);
            b->frames[back].serial = serial;
            b->frames[back].width = 8;
            b->frames[back].height = 8;
            store(&b->frames[back].state, Ready);
            store(&b->front, back);
            store(&b->frame, serial);
        }
        done = true;
    });
    unsigned reads = 0;
    do
    {
        uint32_t front = load(&b->front);
        if (front < 2 && claim(&b->frames[front].state, Reading))
        {
            uint8_t expected = b->frames[front].serial & 255;
            for (unsigned i = 0; i < 64; ++i)
                check(pixels(b, front)[i] == expected, "frame cannot tear while pinned");
            store(&b->frames[front].state, Ready);
            ++reads;
        }
    } while (!done);
    producer.join();
    check(reads > 0, "frame reader ran");
    std::free(b);
}

int main()
{
    parser();
    dom();
    events();
    loop();
    limits();
    frames();
    if (failures)
    {
        std::printf("r2web: %d checks failed\n", failures);
        return 1;
    }
    std::puts("r2web: parser, DOM, events, loop, limits and shared-frame checks passed");
}
