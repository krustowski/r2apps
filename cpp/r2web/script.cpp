#include "script.h"
#ifndef WEB_HOST
#include "../memento-hello/web/net_r2.h"
#endif

namespace web {
namespace {
ScriptPage *active = nullptr;
struct Tag { size_t start, end; char name[24]; bool closing, empty; };
bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_'; }
bool voidTag(const char *s) {
    return !strcmp(s,"input") || !strcmp(s,"img") || !strcmp(s,"br") || !strcmp(s,"hr") || !strcmp(s,"meta") || !strcmp(s,"link") || !strcmp(s,"area") || !strcmp(s,"source") || !strcmp(s,"wbr") || !strcmp(s,"base") || !strcmp(s,"embed") || !strcmp(s,"param") || !strcmp(s,"col");
}
bool nextTag(const Buf &b, size_t &at, Tag &t)
{
    while (at < b.len) {
        if (b.data[at++] != '<') continue;
        t = {}; t.start = at-1;
        if (at+3 <= b.len && !memcmp(b.data+at,"!--",3)) {
            const char *end = ifind((const char *)b.data+at+3, b.len-at-3,"-->");
            at = end ? size_t(end-(const char *)b.data)+3 : b.len; continue;
        }
        if (at < b.len && b.data[at] == '/') { t.closing = true; ++at; }
        size_t first = at;
        while (at < b.len && nameChar((char)b.data[at])) ++at;
        if (first == at) continue;
        scopyn(t.name,(const char *)b.data+first,at-first,sizeof(t.name));
        for (char *p=t.name; *p; ++p) *p=lower(*p);
        char quote=0;
        while (at < b.len) {
            char c=(char)b.data[at++];
            if (quote) { if (c==quote) quote=0; }
            else if (c=='\'' || c=='"') quote=c;
            else if (c=='>') { t.end=at; t.empty=(at>1 && b.data[at-2]=='/') || voidTag(t.name); return true; }
        }
    }
    return false;
}
// Locate attributes by token boundaries, respecting quotes and '=' in values.
bool attrRange(const Buf &b,const Tag &t,const char *name,size_t &first,size_t &last,size_t &vs,size_t &ve)
{
    size_t p=t.start+1;
    while (p<t.end && nameChar((char)b.data[p])) ++p;
    while (p+1<t.end) {
        while (p+1<t.end && isSpace((char)b.data[p])) ++p;
        first=p;
        while (p+1<t.end && nameChar((char)b.data[p])) ++p;
        size_t nameEnd=p;
        if (first==p) { ++p; continue; }
        while (p+1<t.end && isSpace((char)b.data[p])) ++p;
        vs=ve=p;
        if (b.data[p]=='=') {
            ++p; while (p+1<t.end && isSpace((char)b.data[p])) ++p;
            char q=(b.data[p]=='\'' || b.data[p]=='"') ? (char)b.data[p++] : 0;
            vs=p;
            while (p+1<t.end && (q ? b.data[p]!=q : !isSpace((char)b.data[p]) && b.data[p]!='>')) ++p;
            ve=p;
            if (q && p<t.end && b.data[p]==q) ++p;
        }
        last=p;
        if (strlen(name)==nameEnd-first && ieqn((const char *)b.data+first,name,nameEnd-first)) return true;
    }
    return false;
}
void decode(Buf &out,const char *s,size_t n)
{
    for (size_t i=0;i<n;++i) {
        if (s[i]=='&') {
            struct Entity { const char *name; char value; };
            static const Entity entities[]={{"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&apos;",'\''}};
            bool found=false;
            for (auto &e:entities) { size_t l=strlen(e.name); if (l<=n-i && !memcmp(s+i,e.name,l)) { out.push(e.value); i+=l-1; found=true; break; } }
            if (found) continue;
        }
        out.push((uint8_t)s[i]);
    }
}
void escape(Buf &out,const char *s) {
    for (;*s;++s) {
        if (*s=='&') out.appendStr("&amp;");
        else if (*s=='<') out.appendStr("&lt;");
        else if (*s=='>') out.appendStr("&gt;");
        else if (*s=='"') out.appendStr("&quot;");
        else out.push((uint8_t)*s);
    }
}
size_t rawEnd(const Buf &b,const Tag &t,size_t &closing)
{
    char needle[28]="</"; scat(needle,t.name,sizeof(needle));
    size_t p=t.end;
    while (p<b.len) {
        const char *s=ifind((const char *)b.data+p,b.len-p,needle);
        if (!s) break;
        p=size_t(s-(const char *)b.data);
        size_t after=p; Tag end;
        if (nextTag(b,after,end) && end.closing && !strcmp(end.name,t.name)) { closing=p; return after; }
        ++p;
    }
    closing=b.len; return b.len;
}
}
void ScriptPage::fail(const char *message) { scopy(diagnostic,message,sizeof(diagnostic)); }
void *ScriptPage::allocate(void *ctx,void *ptr,int size)
{
    auto *p=(ScriptPage *)ctx;
    auto *old=ptr ? ((Allocation *)ptr)-1 : nullptr;
    if (size<=0) {
        if (old) {
            if (old->prev) old->prev->next=old->next; else p->allocations=old->next;
            if (old->next) old->next->prev=old->prev;
            p->used-=old->size+sizeof(Allocation); big_free(old);
        }
        return nullptr;
    }
    size_t previous=old ? old->size : 0;
    size_t previousCost=old ? previous+sizeof(Allocation) : 0;
    if (size_t(size)+sizeof(Allocation)>HeapLimit-(p->used-previousCost)) return nullptr;
    auto *fresh=(Allocation *)big_alloc(sizeof(Allocation)+size);
    if (!fresh) return nullptr;
    fresh->size=size; fresh->prev=nullptr; fresh->next=p->allocations;
    if (fresh->next) fresh->next->prev=fresh;
    p->allocations=fresh; p->used+=size+sizeof(Allocation);
    if (old) { memcpy(fresh+1,ptr,previous<size_t(size) ? previous:size_t(size)); allocate(ctx,ptr,0); }
    return fresh+1;
}
void ScriptPage::destroyVM()
{
    running=false;
    if (active==this) active=nullptr;
    if (J) { js_freestate(J); J=nullptr; }
    // Also reclaim parser temporaries abandoned by a fatal execution timeout.
    while (allocations) { auto *next=allocations->next; big_free(allocations); allocations=next; }
    used=0;
}
void ScriptPage::clear()
{
    destroyVM(); code.release(); scratch.release(); html=nullptr; document=nullptr;
    nScripts=0; dirty=false; diagnostic[0]=nextUrl[0]=0;
}
void ScriptPage::poll()
{
    if (!running) return;
    if (budget) --budget;
    if (!budget || ((budget & 255)==0 && now_ms()>deadline)) {
        fail("JavaScript stopped: execution limit exceeded.");
        longjmp(abortPoint,1);
    }
}
bool ScriptPage::find(const char *id,Element &e) const
{
    if (!html) return false;
    size_t at=0; Tag t;
    while (nextTag(*html,at,t)) {
        if (t.closing) continue;
        bool match=(id[0]=='@' && !strcmp(t.name,id+1));
        if (id[0]!='@') {
            size_t a,z,vs,ve;
            match=attrRange(*html,t,"id",a,z,vs,ve) && ve-vs==strlen(id) && !memcmp(html->data+vs,id,ve-vs);
        }
        size_t rawClose=at;
        bool raw=!strcmp(t.name,"script") || !strcmp(t.name,"style") || !strcmp(t.name,"textarea") || !strcmp(t.name,"title");
        if (match) {
            e={t.start,t.end,t.end,t.end,{},t.empty}; scopy(e.tag,t.name,sizeof(e.tag));
            if (t.empty) return true;
            if (raw) { e.end=rawEnd(*html,t,e.innerEnd); return true; }
            size_t p=at; int depth=1; Tag close;
            while (nextTag(*html,p,close)) {
                if (!strcmp(close.name,t.name)) {
                    if (close.closing) --depth; else if (!close.empty) ++depth;
                    if (!depth) { e.innerEnd=close.start; e.end=close.end; return true; }
                }
                if (!close.closing && (!strcmp(close.name,"script") || !strcmp(close.name,"style"))) p=rawEnd(*html,close,rawClose);
            }
            e.innerEnd=e.end=html->len; return true;
        }
        if (raw) at=rawEnd(*html,t,rawClose);
    }
    if (!strcmp(id,"@body")) { e={0,0,html->len,html->len,{},false}; scopy(e.tag,"body",sizeof(e.tag)); return true; }
    return false;
}
bool ScriptPage::attribute(const Element &e,const char *name,Buf &out) const
{
    out.clear(); if (!html || e.tagEnd==0) return false;
    Tag t{}; t.start=e.start; t.end=e.tagEnd;
    size_t a,z,vs,ve;
    if (!attrRange(*html,t,name,a,z,vs,ve)) return false;
    decode(out,(const char *)html->data+vs,ve-vs); return !out.failed;
}
bool ScriptPage::replace(size_t first,size_t last,const char *s,size_t len)
{
    if (!html || first>last || last>html->len || len>PageLimit || html->len-(last-first)>PageLimit-len) {
        fail("JavaScript DOM output exceeds the page limit."); return false;
    }
    size_t fresh=html->len-(last-first)+len;
    if (!html->reserve(fresh+1)) { fail("No memory for JavaScript DOM output."); return false; }
    memmove(html->data+first+len,html->data+last,html->len-last);
    if (len) memcpy(html->data+first,s,len);
    html->len=fresh; html->data[fresh]=0; dirty=true; return true;
}
bool ScriptPage::setAttribute(const Element &e,const char *name,const char *value)
{
    Tag t{}; t.start=e.start; t.end=e.tagEnd;
    size_t a,z,vs,ve;
    bool exists=attrRange(*html,t,name,a,z,vs,ve);
    if (!exists) { a=z=e.tagEnd-1; if (a && html->data[a-1]=='/') a=z=a-1; }
    scratch.clear(); scratch.push(' '); scratch.appendStr(name); scratch.appendStr("=\""); escape(scratch,value); scratch.push('"');
    return !scratch.failed && replace(a,z,scratch.cstr(),scratch.len);
}
void ScriptPage::content(const Element &e,bool text)
{
    scratch.clear();
    if (!text) { scratch.append(html->data+e.tagEnd,e.innerEnd-e.tagEnd); return; }
    size_t at=e.tagEnd,first=at; Tag t;
    while (at<e.innerEnd && nextTag(*html,at,t) && t.start<e.innerEnd) {
        decode(scratch,(const char *)html->data+first,t.start-first);
        if (!t.closing && (!strcmp(t.name,"script") || !strcmp(t.name,"style"))) { size_t close; at=rawEnd(*html,t,close); }
        first=at;
    }
    if (first<e.innerEnd) decode(scratch,(const char *)html->data+first,e.innerEnd-first);
}
void ScriptPage::freeNode(js_State *J,void *data) { allocate(self(J),data,0); }
void ScriptPage::node(const char *id)
{
    auto *n=(Node *)allocate(this,nullptr,sizeof(Node));
    if (!n) js_error(J,"JavaScript heap limit exceeded");
    n->page=this; scopy(n->id,id,sizeof(n->id));
    js_pushnull(J);
    js_newuserdatax(J,"r2web.node",n,getNode,putNode,nullptr,freeNode);
}
int ScriptPage::getNode(js_State *J,void *data,const char *name)
{
    auto *n=(Node *)data; auto *p=n->page; Element e;
    if (!strcmp(name,"id")) { js_pushstring(J,n->id[0]=='@' ? "":n->id); return 1; }
    if (!p->find(n->id,e)) { js_pushundefined(J); return 1; }
    if (!strcmp(name,"innerHTML") || !strcmp(name,"textContent") || !strcmp(name,"innerText")) {
        p->content(e,strcmp(name,"innerHTML")!=0); js_pushstring(J,p->scratch.cstr()); return 1;
    }
    if (!strcmp(name,"value") || !strcmp(name,"className") || !strcmp(name,"href") || !strcmp(name,"src")) {
        if (!strcmp(name,"value") && p->document)
            for (int i=0;i<p->document->controlCount();++i) {
                auto &c=p->document->control(i);
                if (!strcmp(p->document->str(c.id),n->id) && c.isText()) { js_pushlstring(J,c.edit ? c.edit:"",c.editLen); return 1; }
            }
        p->attribute(e,!strcmp(name,"className") ? "class":name,p->scratch);
        js_pushstring(J,p->scratch.cstr()); return 1;
    }
    return 0;
}
int ScriptPage::putNode(js_State *J,void *data,const char *name)
{
    auto *n=(Node *)data; auto *p=n->page; Element e;
    if (!p->find(n->id,e)) return 0;
    if (!strcmp(name,"innerHTML") || !strcmp(name,"textContent") || !strcmp(name,"innerText")) {
        const char *value=js_tostring(J,-1);
        if (e.empty) return 1;
        if (!strcmp(name,"innerHTML")) p->replace(e.tagEnd,e.innerEnd,value,strlen(value));
        else { p->scratch.clear(); escape(p->scratch,value); if (!p->scratch.failed) p->replace(e.tagEnd,e.innerEnd,p->scratch.cstr(),p->scratch.len); }
        return 1;
    }
    if (!strcmp(name,"value") || !strcmp(name,"className") || !strcmp(name,"href") || !strcmp(name,"src")) {
        const char *value=js_tostring(J,-1);
        if (!strcmp(name,"value") && p->document)
            for (int i=0;i<p->document->controlCount();++i) {
                auto &c=p->document->control(i);
                if (c.isText() && !strcmp(p->document->str(c.id),n->id)) {
                    p->document->controlSetText(i,value);
                }
            }
        p->setAttribute(e,!strcmp(name,"className") ? "class":name,value);
        return 1;
    }
    return 0;
}
void ScriptPage::getById(js_State *J)
{
    auto *p=self(J); const char *id=js_tostring(J,1); Element e;
    if (!*id || strlen(id)>=sizeof(Node::id) || !p->find(id,e)) js_pushnull(J);
    else p->node(id);
}
void ScriptPage::write(js_State *J)
{
    auto *p=self(J); Element e;
    if (!p->find("@body",e)) { js_pushundefined(J); return; }
    for (int i=1;i<js_gettop(J);++i) {
        const char *s=js_tostring(J,i);
        size_t n=strlen(s);
        if (!p->replace(e.innerEnd,e.innerEnd,s,n)) break;
        e.innerEnd+=n;
    }
    js_pushundefined(J);
}
void ScriptPage::log(js_State *J)
{
    auto *p=self(J);
    if (js_gettop(J)>1) p->fail(js_tostring(J,1));
    js_pushundefined(J);
}
void ScriptPage::getTitle(js_State *J)
{
    auto *p=self(J); Element e;
    if (p->find("@title",e)) { p->content(e,true); js_pushstring(J,p->scratch.cstr()); }
    else js_pushstring(J,"");
}
void ScriptPage::putTitle(js_State *J)
{
    auto *p=self(J); const char *s=js_tostring(J,1); Element e;
    bool found=p->find("@title",e);
    p->scratch.clear();
    if (!found) p->scratch.appendStr("<title>");
    escape(p->scratch,s);
    if (!found) p->scratch.appendStr("</title>");
    if (!p->scratch.failed) p->replace(found ? e.tagEnd:0,found ? e.innerEnd:0,p->scratch.cstr(),p->scratch.len);
    js_pushundefined(J);
}
void ScriptPage::getHref(js_State *J) { js_pushstring(J,self(J)->pageUrl); }
void ScriptPage::putHref(js_State *J) { scopy(self(J)->nextUrl,js_tostring(J,1),sizeof(nextUrl)); js_pushundefined(J); }
void ScriptPage::report(js_State *J,const char *message) { self(J)->fail(message); }
void ScriptPage::bind()
{
    js_setcontext(J,this); js_setreport(J,report);
    js_newobject(J);
    js_newcfunction(J,getById,"getElementById",1); js_setproperty(J,-2,"getElementById");
    js_newcfunction(J,write,"write",1); js_setproperty(J,-2,"write");
    js_newcfunction(J,getTitle,"title",0); js_newcfunction(J,putTitle,"title",1); js_defaccessor(J,-3,"title",0);
    node("@body"); js_setproperty(J,-2,"body");
    js_setglobal(J,"document");
    js_newobject(J);
    js_newcfunction(J,getHref,"href",0); js_newcfunction(J,putHref,"href",1); js_defaccessor(J,-3,"href",0);
    js_newcfunction(J,putHref,"assign",1); js_setproperty(J,-2,"assign");
    js_setglobal(J,"location");
    js_newobject(J); js_newcfunction(J,log,"log",1); js_setproperty(J,-2,"log"); js_setglobal(J,"console");
    js_newcfunction(J,log,"alert",1); js_setglobal(J,"alert");
    js_pushglobal(J); js_setglobal(J,"window");
}
bool ScriptPage::createVM()
{
    if (J) return true;
    J=js_newstate(allocate,this,0);
    if (!J) { fail("No memory for JavaScript."); destroyVM(); return false; }
    if (js_try(J)) { fail("Cannot initialize JavaScript browser bindings."); js_pop(J,1); destroyVM(); return false; }
    bind(); js_endtry(J); return true;
}
bool ScriptPage::start(Buf &body,Document &doc,const char *url)
{
    clear(); html=&body; document=&doc; scopy(pageUrl,url,sizeof(pageUrl));
    size_t at=0; Tag t;
    while (nextTag(body,at,t)) {
        if (t.closing) continue;
        if (!strcmp(t.name,"style") || !strcmp(t.name,"textarea") || !strcmp(t.name,"title")) { size_t close; at=rawEnd(body,t,close); continue; }
        if (strcmp(t.name,"script")) continue;
        size_t close; at=rawEnd(body,t,close);
        Element e{t.start,t.end,close,at,{},false};
        attribute(e,"type",scratch);
        bool classic=!scratch.len || ieq(scratch.cstr(),"text/javascript") || ieq(scratch.cstr(),"application/javascript");
        if (!classic) continue;
        if (nScripts==MaxScripts) { fail("Only the first 16 page scripts are supported."); break; }
        auto &s=scripts[nScripts++]; s={};
        attribute(e,"src",scratch); scopy(s.url,scratch.cstr(),sizeof(s.url));
        s.source=(uint32_t)code.len;
        if (!s.url[0]) code.append(body.data+t.end,close-t.end);
        code.push(0);
    }
    if (code.failed) { fail("No memory for page scripts."); nScripts=0; return false; }
    return true;
}
bool ScriptPage::eval(const char *source,const char *thisId)
{
    if (!createVM()) return false;
    budget=1000000; deadline=now_ms()+250; running=true; active=this;
    if (setjmp(abortPoint)) { destroyVM(); return false; }
    if (js_try(J)) {
        fail(js_trystring(J,-1,"JavaScript error")); js_pop(J,1);
        running=false; active=nullptr; return false;
    }
    int error=js_ploadstring(J,"page script",source);
    if (!error) {
        if (thisId) node(thisId); else js_pushglobal(J);
        error=js_pcall(J,0);
    }
    if (error) fail(js_trystring(J,-1,"JavaScript error"));
    js_pop(J,1); js_endtry(J); running=false; active=nullptr; return error==0;
}
bool ScriptPage::click(const char *handler,const char *id)
{
    if (!handler || !*handler) return false;
    if (!createVM()) return true;
    budget=1000000; deadline=now_ms()+250; running=true; active=this;
    if (setjmp(abortPoint)) { destroyVM(); return true; }
    if (js_try(J)) {
        fail(js_trystring(J,-1,"JavaScript handler error")); js_pop(J,1);
        running=false; active=nullptr; return true;
    }
    // Function compiles the handler as a body, so return and this work.
    js_getglobal(J,"Function"); js_pushstring(J,handler);
    int error=js_pconstruct(J,1);
    if (!error) { node(id && *id ? id:"@body"); error=js_pcall(J,0); }
    if (error) fail(js_trystring(J,-1,"JavaScript handler error"));
    js_pop(J,1); js_endtry(J); running=false; active=nullptr;
    return true;
}
} // namespace web
extern "C" void web_js_poll(void) { if (web::active) web::active->poll(); }
extern "C" double web_js_epoch_ms(void)
{
#ifdef WEB_HOST
    return 0; // Deterministic host tests; Date parsing still uses upstream code.
#else
    unsigned long days, seconds;
    web::currentTime(&days,&seconds);
    return days ? (double(days)-719528)*86400000.0+seconds*1000.0:0;
#endif
}
