//
//  unit.cpp --- r2web's host checks: the HTML parser, the DOM and its
//  bridge to the browser (script.h), resource limits, and the shared frames
//  of host.h.  The network is not here (no ScriptNet on the host): libjsr2's
//  own tests cover fetch and EventSource with a scripted one.
//
#include "../script.h"
#include "../../memento-hello/web/boxpaint.h"
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

static void largeSnapshots()
{
    Page p;
    check(p.load("<body style='margin:0'><script></script></body>"), "large snapshot page starts");
    check(p.eval("document.body.innerHTML='<div id=box style=\"width:123px\">'+"
                 "('<p class=card data-info=\"'+'x'.repeat(150)+'\">hello</p>').repeat(4000)+'</div>';"),
          "large DOM fits the script heap", p.js.error());
    web::Buf html{true};
    char title[128];
    check(p.js.render(html, title, sizeof(title)), "large DOM renders within the script heap", p.js.error());
    check(html.len > 750000 && html.len < web::ScriptPage::PageLimit, "large snapshot has its complete markup");
    check(p.eval("document.title=[box.offsetWidth,box.offsetWidth,new URL('/next',document.URL).pathname].join('|')"),
          "large geometry snapshot and URL parsing fit the script heap", p.js.error());
    p.render();
    check(p.has("<title>123|123|/next</title>"), "geometry accepts markup expanded by node ids", p.out);
    check(p.eval("box.style.width='77px';document.title=box.offsetWidth"), "large geometry updates after mutation", p.js.error());
    p.render();
    check(p.has("<title>77</title>"), "large geometry refreshes after mutation", p.out);
    check(p.js.heapBytes() <= web::ScriptPage::HeapLimit, "large snapshots respect the heap limit");

    Page q;
    check(q.load("<button id=b onclick='window.clicked=true'>keep</button>"), "snapshot limit page starts");
    const std::string before = q.out;
    int button = q.node("b");
    check(q.eval("document.body.appendChild(document.createTextNode('x'.repeat(4*768*1024)))"), "oversized DOM text fits the heap");
    check(!q.js.render(q.html, title, sizeof(title)), "oversized snapshot is rejected");
    check(std::string((const char *)q.html.data, q.html.len) == before, "rejected snapshot preserves the shown document");
    char status[160];
    check(q.js.takeStatus(status, sizeof(status)) && std::strstr(status, "too big"), "snapshot limit has a useful status");
    q.js.click(button);
    check(q.eval("if (!window.clicked) throw Error('lost rendered button'); document.body.lastChild.remove(); document.title='recovered'"),
          "rejected snapshot preserves events and the page can shrink", q.js.error());
    q.render();
    check(q.has("<title>recovered</title>"), "rendering recovers after a rejected snapshot", q.out);
}

static void pixels()
{
    web::Document d;
    auto load=[&](const char *html,int width=320) {d.loadMessage(html);d.layoutPixels(width,8,16);};
    auto b=[&](const char *id)->const web::Box & {const web::Box *v=d.boxForId(id);check(v!=nullptr,"layout box exists",id);return v?*v:d.box(0);};
    load("<body style='margin:0'><div id=box style='width:100px;height:40px;margin:5px 7px;padding:3px 4px;border:2px solid red;background:blue'>"
         "<a id=link href='/next'>link</a></div></body>");
    check(b("box").x==7 && b("box").y==5 && b("box").w==112 && b("box").h==50,"pixel margins, padding, border and content dimensions");
    check(b("box").clientW==108 && b("box").clientH==46 && b("box").border[3]==2,"client size excludes borders");
    check(b("box").style.bg && b("box").style.box.borderColor,"box paint colours survive cascade");
    check(b("link").x==13 && b("link").y==10 && b("link").w==32 && d.pixelLinkAt(14,11)==0,"links use pixel positions");
    int x=0,y=0;check(d.pixelLinkPoint(0,x,y) && x==13 && y==10,"link focus position in pixels");
    load("<body style='margin:0'><div id=a style='box-sizing:border-box;width:100px;height:40px;padding:4px;border:2px solid'>x</div></body>");
    check(b("a").w==100 && b("a").h==40 && b("a").contentW==88 && b("a").contentH==28,"border-box dimensions");
    load("<body style='margin:0'><div id=a style='width:80px'>one two three four</div><div id=z style='height:7px'></div></body>");
    check(b("a").h==32 && b("z").y==32,"word wrapping controls following block position");
    load("<body style='margin:0'><div id=a style='width:40px'>abcdefghijk</div><pre id=p style='margin:0'>a\nb</pre></body>");
    check(b("a").h==48 && b("p").h==32,"long words and preformatted newlines");
    load("<body style='margin:0'><div id=f style='display:flex;width:300px;gap:10px;padding:4px'>"
         "<div id=a style='flex:1;height:20px'>a</div><div id=z style='flex:2;height:30px'>b</div></div></body>");
    check(b("a").w==96 && b("z").w==194 && b("z").x==110,"flex growth and gap");
    check(b("f").h==38,"flex cross height plus padding");
    load("<body style='margin:0'><div style='display:flex;width:8px'>"
         "<div id=a style='flex:1;height:2px'></div><div id=m style='flex:1;height:2px'></div>"
         "<div id=z style='flex:1;height:2px'></div></div></body>");
    check(b("a").w==2 && b("m").w==3 && b("z").w==3 && b("z").x+b("z").w==8,
          "fractional flex growth fills the complete chart width");
    load("<body style='margin:0'><div style='display:flex;width:8px'>"
         "<div id=a style='width:4px;height:2px'></div><div id=m style='width:4px;height:2px'></div>"
         "<div id=z style='width:4px;height:2px'></div></div></body>");
    check(b("a").w==3 && b("m").w==3 && b("z").w==2 && b("z").x+b("z").w==8,
          "fractional flex shrink keeps the row within its available width");
    load("<style>.card { flex: 1 1 100px; } .fixed { flex: 0 0 auto; }</style><body style='margin:0'>"
         "<div style='display:flex;width:240px;gap:8px'><div id=a class=card>a</div><div id=z class=card>b</div></div>"
         "<div id=f class=fixed>fixed</div></body>");
    check(b("a").w==116 && b("z").x==124 && b("f").style.box.grow==0 && b("f").style.box.shrink==0,
          "flex shorthands accept normal stylesheet whitespace and an explicit auto basis");
    load("<body style='margin:0'><div id=f style='display:flex;width:100px;gap:4px;flex-wrap:wrap'>"
         "<div id=a style='width:60px;height:10px'></div><div id=z style='width:60px;height:20px'></div></div></body>");
    check(b("z").x==0 && b("z").y==14 && b("f").h==34,"flex wrapping and row gap");
    load("<body style='margin:0'><div style='display:flex;width:100px'>"
         "<div id=a style='width:80px;height:10px'></div><div id=z style='width:80px;height:20px'></div></div></body>");
    check(b("a").w==50 && b("z").x==50 && b("a").h==10,"flex shrink preserves explicit cross size");
    load("<body style='margin:0'><div style='display:flex;width:100px;height:60px;align-items:center;justify-content:space-between'>"
         "<div id=a style='width:20px;height:10px'></div><div id=z style='width:20px;height:20px'></div></div></body>");
    check(b("a").y==25 && b("z").x==80 && b("z").y==20,"flex alignment and justification");
    load("<body style='margin:0'><div id=f style='display:flex;flex-direction:column;width:100px;height:100px;gap:10px'>"
         "<div id=a style='flex:1'>x</div><div id=z style='flex:1'>y</div></div></body>");
    check(b("a").h==45 && b("z").y==55 && b("z").w==100,"column flex growth and stretch");
    load("<style>.s{width:50%;padding:2px}#a{width:80px!important}</style><body style='margin:0'>"
         "<div id=a class=s style='width:10px'></div><div id=z class=s style='margin:0 auto'></div></body>",200);
    check(b("a").w==84 && b("z").w==104 && b("z").x==48,"selector cascade, important, percentages and auto margins");
    d.layoutPixels(400,8,16);check(b("z").w==204 && b("z").x==98,"pixel relayout on resize");
    load("<body style='margin:0'><input id=a style='width:100px;padding:4px' value=hello>"
         "<img id=z width=40 height=30 src='pic'></body>");
    d.setImageSize(0,80,60);d.layoutPixels(320,8,16);
    check(b("a").w==108 && b("z").w==40 && b("z").h==30,"controls and image dimensions");
    check(d.pixelLinkAt(2,2)>=0,"control box is clickable beyond its text");
    d.layout(40);check(!d.pixelLayout() && d.lineCount()>0,"legacy text layout remains available");
    d.layoutPixels(320,8,16);check(d.pixelLayout() && b("z").w==40,"switch back to pixel layout");
    load("<body style='margin:0'><div id=a style='width:40px;height:5px'><div style='height:80px;width:100px'></div></div>"
         "<div id=z style='display:none'>hidden</div></body>");
    check(b("a").scrollH==80 && b("a").scrollW==100 && !d.boxForId("z"),"overflow extents and hidden elements");
    load("<body style='margin:0'><div aria-hidden=true><div id=bar style='height:30px;background:#78e8ce'></div></div></body>");
    check(d.boxForId("bar") && b("bar").h==30 && b("bar").style.bgRgb==0xFF78E8CE,
          "aria-hidden decorative content remains visible in pixel layout");
    load("<body style='margin:0'><button id=a>go</button><span id=z>end</span></body>");
    std::string labels;
    for(size_t i=0;i<d.lineCount();++i)for(uint32_t r=0;r<d.line(i).nRuns;++r) {
        const auto &run=d.run(d.line(i).firstRun+r);labels.append(d.text(run.off),run.len);
    }
    check(labels=="goend" && b("z").x==34,"pixel buttons use padded boxes without synthetic brackets",labels);
    d.layout(40);
    labels.clear();
    for(size_t i=0;i<d.lineCount();++i)for(uint32_t r=0;r<d.line(i).nRuns;++r) {
        const auto &run=d.run(d.line(i).firstRun+r);labels.append(d.text(run.off),run.len);
    }
    check(labels.find("[go]")!=std::string::npos,"text layout retains its button delimiters",labels);
    load("<body style='margin:0'><button id=a style='width:50px'>abc def</button></body>");
    check(b("a").h==40,"button text wraps inside its assigned content width");
    load("<body style='margin:0;background:#1e2221;color:#c9c6c1'><div id=a style='background:#181b1a;border:1px solid #3a4140;border-radius:9px'>"
         "<a id=z href=/next style='color:#78e8ce'>accent</a></div></body>");
    check(b("a").style.bgRgb==0xFF181B1A && b("a").style.box.borderRgb==0xFF3A4140 && b("a").style.box.radius==9,
          "pixel colours and rounded borders retain the authored values");
    const auto &accent=d.run(d.line(0).firstRun);
    check(accent.fgRgb==0xFF78E8CE && accent.bgRgb==0xFF181B1A,"link RGB and inherited surface reach the painter");
    load("<body style='margin:0'><button id=a>[go]</button><input id=z type=submit value='[send]'></body>");
    labels.clear();
    for(size_t i=0;i<d.lineCount();++i)for(uint32_t r=0;r<d.line(i).nRuns;++r) {
        const auto &run=d.run(d.line(i).firstRun+r);labels.append(d.text(run.off),run.len);
    }
    check(labels=="[go][send]","authored brackets survive on button and submit labels",labels);
    check(web::cssDarkRgb(0xFF00AA00)==0xFF55FF55 && web::cssDarkRgb(0xFF55FF55)==0xFF00AA00 &&
          web::cssDarkRgb(0xFFFFFFFF)==0xFF000000,"RGB dark mode preserves accent hues and reverses brightness");
    web::Box rounded;rounded.w=40;rounded.h=30;rounded.style.box.radius=9;
    for(int k=0;k<4;++k)rounded.border[k]=1;
    unsigned char canvas[30][40]={};int fills=0;
    web::paintBox(rounded,[&](int x,int y,int w,int h,bool border) {
        ++fills;
        check(x>=0 && y>=0 && x+w<=40 && y+h<=30,"rounded paint remains in its box");
        for(int row=y;row<y+h;++row)for(int col=x;col<x+w;++col) {
            check(!canvas[row][col],"rounded background and borders do not overlap");
            canvas[row][col]=border?2:1;
        }
    });
    check(!canvas[0][0] && canvas[0][20]==2 && canvas[15][0]==2 && canvas[15][20]==1,
          "rounded paint leaves corner cutouts, a solid border and filled interior");
    check(canvas[29][0]==canvas[0][0] && canvas[29][20]==canvas[0][20] && fills<80,
          "rounded corners are symmetric and straight spans are batched");
    load("<body style='margin:0'><div style='display:flex;justify-content:space-between;width:320px'>"
         "<span>brand</span><nav id=a style='display:flex;flex-wrap:wrap;gap:10px'><a>one</a><a>two</a><a id=z>three</a></nav></div></body>");
    check(b("a").w==108 && b("a").h==16 && b("z").y==b("a").y,"intrinsic flex width includes gaps and keeps navigation on one row");
    load("<style>body{font-size:15px}.value{font-size:26px}h2{font-size:18px}</style><body style='margin:0'>"
         "<p id=a class=value style='margin:0'>25%</p><h2 id=z style='margin:0'>Machine</h2></body>");
    check(b("a").big && b("a").h==32 && !b("z").big && b("z").h==16,"CSS sizes distinguish summary values from panel headings");
    load("<body style='margin:0'><div style='display:flex;width:160px;gap:8px'>hello<span id=z>there</span></div></body>");
    check(b("z").x==48 && d.lineCount()==2,"direct text forms an anonymous flex item");
    load("<body style='margin:0'>a<br><span id=z>b</span></body>");
    check(b("z").x==0 && b("z").y==16,"br advances the enclosing inline flow");
    load("<body style='margin:0'>a<input id=a type=hidden style='width:100px;height:100px'><span id=z>b</span></body>");
    check(b("a").w==0 && b("a").h==0 && b("z").x==8,"hidden inputs neither draw nor advance the cursor");
    load("<body style='margin:0'><span id=a style='padding:2px 4px;border:1px solid red'>abc</span><span id=z>x</span></body>");
    check(b("a").x==0 && b("a").y==-3 && b("a").w==34 && b("a").h==22 && b("z").x==34,"inline padding and border boxes");
    load("<body style='margin:0'><div id=a style='width:80px;display:flex;flex-direction:column;gap:4px'>"
         "<div>one two three four</div><div id=z>end</div></div></body>");
    check(b("z").y==36 && b("a").h==52,"auto-height flex columns keep wrapped content");
    load("<body style='margin:0'><div style='height:100px'><div id=a style='height:50%'></div></div>"
         "<div id=z style='height:50%'>x</div></body>");
    check(b("a").h==50 && b("z").h==16,"percentage heights require a definite containing height");
    load("<html style='height:100%'><body style='margin:0;height:100%'><div id=a style='height:50%'></div></body></html>");
    d.layoutPixels(320,8,16,200);check(b("a").h==100,"viewport-rooted percentage heights");
    load("<body style='margin:0'><div id=a style='box-sizing:border-box;min-height:50px;padding:4px;border:2px solid'></div></body>");
    check(b("a").h==50,"border-box min-height includes edges");
    load("<body style='margin:0'><div id=a style='text-align:center;width:100px'><span id=z>abc</span></div></body>");
    check(b("z").x==38 && d.run(d.line(0).firstRun).x==38,"text alignment moves inline geometry with its glyphs");
    load("<body style='margin:0'><div style='text-align:center;width:100px'><div><span id=z>abc</span></div></div></body>");
    check(b("z").x==38 && d.run(d.line(0).firstRun).x==38,"nested blocks align their own text once");
    load("<body style='margin:0'><div id=a>late CSS</div><style>#a{width:120px}</style></body>");
    check(b("a").w==120,"style blocks apply to earlier elements");
    load("<body style='margin:0'><div id=a>safe</div><script>let s='<style>#a{width:1px}</style>';</script>"
         "<textarea><style>#a{width:2px}</style></textarea></body>");
    check(b("a").w==320,"raw-text strings do not become stylesheets");
    load("<p><a href=/next><h2 id=a>heading</h2></a><p>end");
    check(d.linkRow(0)>=0 && b("a").h>0,"reconstructed formatting retains a valid box tree");
    std::string deep="<body>";
    for(int i=0;i<300;++i)deep+="<div>";
    deep+="deep";for(int i=0;i<300;++i)deep+="</div>";
    deep+="</body>";load(deep.c_str());
    check(d.rows()>0 && d.boxCount()<140,"deep markup remains bounded");

}

static void geometry()
{
    Page p;
    p.js.setPixelViewport(320,200,8,16,0,true,true);
    p.load("<style>#box{width:100px;height:40px;padding:3px 4px;border:2px solid red}</style><body style='margin:0'>"
           "<div id=box><span id=child>hello</span></div><script>"
           "const el=document.getElementById('box'),r=el.getBoundingClientRect();"
           "document.title=[r.x,r.y,r.width,r.height,el.clientWidth,el.clientHeight,el.clientLeft,el.clientTop,el.offsetWidth,el.scrollHeight].join('|');"
           "</script></body>");
    check(p.has("<title>0|0|112|50|108|46|2|2|112|46</title>"),"scripts get real boxes during initial execution",p.out);
    p.eval("box.style.width='120px';document.title=box.getBoundingClientRect().width");p.render();
    check(p.has("<title>132</title>"),"geometry flushes style changes synchronously",p.out);
    p.eval("child.textContent='abcdefghijklmno';box.style.width='40px';box.style.height='auto';"
           "document.title=[box.offsetHeight,child.offsetLeft,child.offsetTop,child.offsetParent===box].join('|')");p.render();
    check(p.has("<title>58|4|3|true</title>"),"text mutation, wrapping and offset geometry",p.out);
    p.js.setPixelViewport(160,200,8,16,20,true,true);
    p.eval("document.title=[innerWidth,innerHeight,box.getBoundingClientRect().top,scrollY].join('|')");p.render();
    check(p.has("<title>160|200|-20|20</title>"),"viewport and scroll-adjusted bounding rect",p.out);
    p.eval("el.remove();document.title=[el.offsetWidth,el.getBoundingClientRect().height].join('|')");p.render();
    check(p.has("<title>0|0</title>"),"detached elements have zero geometry",p.out);
    Page q;
    q.js.setPixelViewport(320,200,8,16,0,false,true);
    q.load("<body><div id=a style='width:100px'>hello</div><script>document.title=a.offsetWidth</script></body>");
    check(q.has("<title>40</title>"),"text mode retains approximate geometry",q.out);
    q.js.setPixelViewport(320,200,8,16,0,true,true);
    q.eval("document.title=a.offsetWidth");q.render();
    check(q.has("<title>100</title>"),"geometry switches from text to pixels",q.out);
    Page r;
    r.js.setPixelViewport(320,200,8,16,0,true,true);
    r.load("<body style='margin:0'><div id=a style='width:50%'>x</div><script>document.title=getComputedStyle(a).width</script></body>");
    check(r.has("<title>160px</title>"),"computed width resolves percentages",r.out);
    r.js.setPixelViewport(400,200,8,16,0,true,true);
    r.eval("document.title=getComputedStyle(a).width");r.render();
    check(r.has("<title>200px</title>"),"geometry invalidates on resize",r.out);
    const char *css="#a{width:80px!important}";
    web::StyleSheetText sheet={(const uint8_t *)css,strlen(css)};
    r.js.setLayoutSheets(&sheet,1);
    r.eval("document.title=a.offsetWidth");r.render();
    check(r.has("<title>80</title>"),"external CSS affects geometry",r.out);
    r.js.setPixelViewport(400,200,8,16,0,true,false);
    r.eval("document.title=a.offsetWidth");r.render();
    check(r.has("<title>384</title>"),"CSS-off geometry follows the layout switch",r.out);

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
    largeSnapshots();
    pixels();
    geometry();
    frames();
    if (failures)
    {
        std::printf("r2web: %d checks failed\n", failures);
        return 1;
    }
    std::puts("r2web: parser, DOM, events, loop, limits, pixel layout, geometry and shared-frame checks passed");
}
