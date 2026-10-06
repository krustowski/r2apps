#include "../script.h"
#include "../host.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static size_t live = 0;
namespace web {
void *alloc(size_t n) { void *p=std::malloc(n); if(p) ++live; return p; }
void *realloc(void *p,size_t n) { if (!p) return alloc(n); return std::realloc(p,n); }
void free(void *p) { if(p) { --live; std::free(p); } }
void *big_alloc(size_t n) { return alloc(n); }
void *big_realloc(void *p,size_t n) { return realloc(p,n); }
void big_free(void *p) { free(p); }
uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
static void check(bool value,const char *message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static void load(web::Buf &html,web::Document &doc,web::ScriptPage &js,const char *text) {
    html.clear(); html.appendStr(text);
    doc.loadHtml(html.data,html.len,"utf-8");
    check(js.start(html,doc,"https://example.test/index.html"),"script preparation");
}
static void scripts()
{
    web::Buf html{true}; web::Document doc; web::ScriptPage js;
    load(html,doc,js,"<body><p id='answer'>old</p><input id='name' value='initial'>"
        "<button id='button' type='button' onclick=\"this.textContent='clicked'; document.title='handler';\">Go</button>"
        "<script>var result = 6*7; document.getElementById('answer').textContent = 'answer ' + result;"
        "document.title='computed'; document.write('<p>written</p>');</script>"
        "<script type='application/json'>{broken}</script><script src='two.js'></script>"
        "<script>document.title=String(result+1);</script></body>");
    check(js.count()==3,"classic scripts only, with external scripts in order");
    check(!std::strcmp(js.src(1),"two.js"),"external source recorded");
    check(js.eval(js.source(0)),js.error());
    check(std::strstr(html.cstr(),"answer 42"),"textContent arithmetic");
    check(std::strstr(html.cstr(),"<title>computed</title>"),"title insertion");
    check(std::strstr(html.cstr(),"<p>written</p></body>"),"document.write output");
    check(js.eval("result += 1;"),"external evaluation");
    check(js.eval(js.source(2)),"script globals persist");
    check(std::strstr(html.cstr(),"<title>44</title>"),"ordered script result");
    check(js.eval("document.getElementById('answer').textContent='<b>&safe';"),"escaped text mutation");
    check(std::strstr(html.cstr(),"&lt;b&gt;&amp;safe"),"textContent escaping");
    check(js.eval("document.getElementById('answer').innerHTML='<strong>markup</strong>';"),"innerHTML mutation");
    check(std::strstr(html.cstr(),"<strong>markup</strong>"),"innerHTML preserves markup");
    check(js.eval("if(document.getElementById('missing') !== null) throw Error('not null');"),"missing element");
    doc.controlSetText(0,"typed");
    check(js.eval("if(document.getElementById('name').value !== 'typed') throw Error('stale');"
        "document.getElementById('name').value='new';"),"live form values");
    check(!std::strcmp(doc.controlText(0),"new"),"form setter updates control");
    check(js.click("this.textContent='clicked'; document.title='handler'; return false;","button"),"onclick handler");
    check(std::strstr(html.cstr(),">clicked</button>"),"onclick receiver");
    check(js.eval("location.href='/next';"),"location binding");
    check(!std::strcmp(js.navigation(),"/next"),"navigation request");
    check(js.eval("var a=[1,2,3]; var f=function(x){return x*2;};"
        "if(JSON.parse('{\"x\":7}').x!==7 || f(a[1])!==4 || /abc/.test('abc')!==true) throw Error('ES5');"),"ES5 builtins");
    check(js.eval("if((1.25).toFixed(2)!=='1.25' || Math.pow(2,10)!==1024) throw Error('format/math');"),"freestanding number formatting and math");
    check(!js.eval("var = broken"),"syntax error caught");
    check(js.eval("document.title='still alive';"),"recover after syntax error");
    check(js.heapBytes()<=web::ScriptPage::HeapLimit,"heap bound");
    js.clear(); check(js.heapBytes()==0,"release runtime");
}
static void limits()
{
    web::Buf html{true}; web::Document doc; web::ScriptPage js;
    load(html,doc,js,"<body id='body'>unchanged</body>");
    uint64_t before=web::now_ms();
    check(!js.eval("while(true) { try { while(true) {} } catch(e) {} }"),"uncatchable execution limit");
    check(web::now_ms()-before<2000,"infinite script stopped promptly");
    check(std::strstr(js.error(),"execution limit"),"timeout diagnostic");
    check(js.heapBytes()==0,"timed out state fully released");
    check(js.eval("document.title='recovered';"),"new state after timeout");
    check(!js.eval("var s='x'; for(var i=0;i<24;i++) s=s+s;"),"allocation limit");
    check(js.heapBytes()<=web::ScriptPage::HeapLimit,"OOM kept within cap");
    js.clear();
    load(html,doc,js,"<body><script>throw Error('test');</script><script>document.title='next';</script></body>");
    check(!js.eval(js.source(0)),"runtime errors caught");
    check(js.eval(js.source(1)),"later script after error");
    check(std::strstr(html.cstr(),"<title>next</title>"),"later script output");
    check(!js.eval("document.body.innerHTML = Array(800000).join('x');"),"array size bound");
}
static void htmlParsing()
{
    web::Buf html{true}; web::Document doc; web::ScriptPage js;
    load(html,doc,js,"<!-- <script>bad</script> --><textarea><script>bad</script></textarea>"
        "<div id='outside' data-id='wrong'><div id='inside'>x</div></div><script>"
        "document.getElementById('outside').innerHTML='<span>nested replaced</span>';</script>");
    check(js.count()==1,"raw text and comments do not become scripts");
    check(js.eval(js.source(0)),"nested content mutation");
    check(std::strstr(html.cstr(),"<span>nested replaced</span></div>"),"correct closing tag range");
    load(html,doc,js,"<a id='a' href='/old' onclick=\"document.title='a';\">link</a>");
    check(!std::strcmp(doc.linkId(0),"a") && std::strstr(doc.linkHandler(0),"document.title"),"link handler metadata");
    check(js.eval("document.getElementById('a').href='/new?a=1&b=2';"),"attribute mutation");
    check(std::strstr(html.cstr(),"href=\"/new?a=1&amp;b=2\""),"attribute escaping");
    check(js.eval("if(document.getElementById('a').href!=='/new?a=1&b=2') throw Error('decode');"),"attribute decoding");
}
static void frames()
{
    using namespace r2web;
    auto *b=(HostBlock *)std::calloc(1,blockSize(64));
    b->capacity=64; b->maxWidth=8; b->maxHeight=8; b->front=None;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t serial=1;serial<=100000;++serial) {
            uint32_t back=load(&b->front)==0 ? 1:0;
            if (!claim(&b->frames[back].state,Writing)) { --serial; std::this_thread::yield(); continue; }
            std::memset(pixels(b,back),serial&255,64);
            b->frames[back].serial=serial; b->frames[back].width=8; b->frames[back].height=8;
            store(&b->frames[back].state,Ready); store(&b->front,back); store(&b->frame,serial);
        }
        done=true;
    });
    unsigned reads=0;
    do {
        uint32_t front=load(&b->front);
        if (front<2 && claim(&b->frames[front].state,Reading)) {
            uint8_t expected=b->frames[front].serial&255;
            for (unsigned i=0;i<64;++i) check(pixels(b,front)[i]==expected,"frame cannot tear while pinned");
            store(&b->frames[front].state,Ready); ++reads;
        }
    } while (!done);
    producer.join(); check(reads>0,"frame reader ran"); std::free(b);
}
int main()
{
    scripts(); check(live==0,"script allocations reclaimed");
    limits(); check(live==0,"error and timeout allocations reclaimed");
    htmlParsing(); check(live==0,"DOM allocations reclaimed");
    frames(); std::puts("r2web: script, DOM, resource-limit and shared-frame checks passed");
}
