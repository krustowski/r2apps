// r2web.elf owns the browser; Memento hosts its indexed frames and input.
#include "../../r2web/host.h"
#include "../web/wbase.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"
struct BrowserHostSlot { r2web::HostBlock *block; uint8_t pid; bool orphan; };
static BrowserHostSlot g_browserHosts[8] = {};
static bool browserAlive(uint8_t pid)
{
    auto tasks = r2::tasks();
    if (tasks.empty()) return true;
    for (const auto &t : tasks)
        if (t.id == pid && (t.name[0] | 32) == 'r' && t.name[1] == '2' && (t.name[2] | 32) == 'w')
            return t.status < 4;
    return false;
}
static void browserKeepAlive()
{
    for (auto &s : g_browserHosts) {
        if (!s.block) continue;
        if (!s.orphan) r2web::store(&s.block->hostBeat, r2web::load(&s.block->hostBeat)+1);
        else if (r2web::load(&s.block->exited) || !browserAlive(s.pid)) {
            r2::heap::kernel_deallocate(s.block); s = {};
        }
    }
}
class BrowserWindow {
public:
    explicit BrowserWindow(const char *url = nullptr)
    {
        browserKeepAlive();
        for (int i=0; i<8; ++i) if (!g_browserHosts[i].block) { slot=i; break; }
        if (slot<0) { strcpy(error,"Too many browser processes."); return; }
        int w,h;
        MementoR2Impl::R2_Vga640x400::ScreenSize(w,h);
        if (w>int(r2web::MaxDimension)) w=r2web::MaxDimension;
        if (h>int(r2web::MaxDimension)) h=r2web::MaxDimension;
        if (w<=0 || h<=0) { strcpy(error,"Invalid screen size."); return; }
        block=(r2web::HostBlock *)r2::heap::kernel_allocate(r2web::blockSize(w*h));
        if (!block) { strcpy(error,"No memory for the browser host block."); return; }
        memset(block,0,sizeof(*block));
        block->magic=r2web::Magic; block->version=r2web::Version;
        block->capacity=w*h; block->maxWidth=w; block->maxHeight=h;
        block->colours=MementoR2Impl::R2_Palette::Count();
        block->portBase=48000+slot*32; block->front=r2web::None; block->hostBeat=1;
        web::scopy(block->initialUrl,url && *url ? url : "about:home",sizeof(block->initialUrl));
        char args[64]="r2web.elf --host 0x";
        size_t at=strlen(args);
        uintptr_t address=(uintptr_t)block;
        for (int shift=60; shift>=0; shift-=4) args[at++]="0123456789abcdef"[(address>>shift)&15];
        args[at]=0;
        auto id=r2::spawn("r2web.elf",args);
        if (!id) {
            strcpy(error,"Install r2web.elf in /mnt/tar/bin or /mnt/iso/bin.");
            r2::heap::kernel_deallocate(block); block=nullptr; return;
        }
        pid=*id; g_browserHosts[slot]={block,pid,false}; started=r2::ticks();
    }
    ~BrowserWindow()
    {
        if (!block) return;
        r2web::store(&block->quit,1);
        for (int i=0; i<50 && !r2web::load(&block->exited) && browserAlive(pid); ++i) r2::sleep(20);
        // The child must stop using shared memory before its owner frees it.
        if (!r2web::load(&block->exited) && browserAlive(pid)) (void)r2::kill(pid);
        if (r2web::load(&block->exited) || !browserAlive(pid)) {
            r2::heap::kernel_deallocate(block); g_browserHosts[slot]={};
        } else g_browserHosts[slot].orphan=true;
    }
    bool failed() const { return !block; }
    const char *why() const { return error; }
    void SetWindow(PlatformWindow *w) { wnd=w; wnd->SetImmediateMode(true); }
    static void onEvent(void *p,PlatformWindowInterfaceInputEvent *e) { ((BrowserWindow *)p)->event(e); }
private:
    PlatformWindow *wnd=nullptr;
    r2web::HostBlock *block=nullptr;
    int slot=-1;
    uint8_t pid=0;
    uint32_t shown=0,sentWidth=0,sentHeight=0,wantWidth=0,wantHeight=0;
    uint64_t started=0,lastCheck=0;
    bool ended=false;
    char error[128]={};
    PlatformColor *background=nullptr,*foreground=nullptr;
    PlatformFont *font=nullptr;
    bool send(const r2web::Command &c)
    {
        if (!block || ended) return false;
        uint32_t h=r2web::load(&block->head),t=r2web::load(&block->tail);
        if (h-t>=r2web::Queue) return false;
        block->commands[h%r2web::Queue]=c;
        r2web::store(&block->head,h+1); return true;
    }
    void idle()
    {
        if (!block || ended) return;
        r2web::store(&block->hostBeat,r2web::load(&block->hostBeat)+1);
        resize();
        if (r2web::load(&block->exited) && !block->error[0]) {
            ended=true; wnd->Close(); return;
        }
        if (r2web::load(&block->copyPending)) {
            clipboardSet(block->copyText); r2web::store(&block->copyPending,0);
        }
        if (r2web::load(&block->openPending)) {
            char url[r2web::TextCapacity]; web::scopy(url,block->openUrl,sizeof(url));
            r2web::store(&block->openPending,0); openBrowserWindow(url);
        }
        if (r2web::load(&block->frame)!=shown) wnd->Repaint();
        uint64_t now=r2::ticks();
        if (now-started<2000 || now-lastCheck<1000) return;
        lastCheck=now;
        if (r2web::load(&block->exited) || !browserAlive(pid)) {
            ended=true;
            web::scopy(error,block->error[0] ? block->error : "Browser process ended. Reopen Web to restart.",sizeof(error));
            wnd->SetImmediateMode(false); wnd->Repaint();
        }
    }
    void paint(PlatformDrawingContext *dc,PlatformBitmap *target)
    {
        if (!target) return;
        auto *bm=static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
        // Bitmaps reserve extra pixels when resized. Only the active client
        // rectangle goes to the child; GetRealWidth is the backing stride.
        uint32_t stride=bm->GetRealWidth().intValue();
        Coord width=target->GetWidth(),height=target->GetHeight();
        uint32_t w=(uint32_t)ceil(COORD_VAL(width)*2),h=(uint32_t)ceil(COORD_VAL(height)*2);
        wantWidth=w; wantHeight=h; resize();
        if (block && !ended) {
            uint32_t front=r2web::load(&block->front);
            if (front<2 && r2web::claim(&block->frames[front].state,r2web::Reading)) {
                const auto &f=block->frames[front];
                if (r2web::dimensions(block,f.width,f.height)) {
                    const uint8_t *src=r2web::pixels(block,front);
                    uint8_t *dst=bm->GetPixels();
                    if (dst) {
                        if (w==f.width && h==f.height) {
                            for (uint32_t y=0; y<h; ++y) memcpy(dst+size_t(y)*stride,src+size_t(y)*w,w);
                        }
                        else for (uint32_t y=0; y<h; ++y)
                            for (uint32_t x=0; x<w; ++x)
                                dst[size_t(y)*stride+x]=src[size_t(y*f.height/h)*f.width+x*f.width/w];
                        // The current r2 backend has no SetTitle handler.
                        // Its public title is what the compositor draws.
                        auto *native=static_cast<MementoR2Impl::R2_WindowImpl *>(wnd);
                        char title[sizeof(native->title)]; web::scopy(title,f.title,sizeof(title));
                        if (strcmp(native->title,title)) { strcpy(native->title,title); wnd->Repaint(); }
                        shown=f.serial;
                    }
                }
                r2web::store(&block->frames[front].state,r2web::Ready);
                if (shown) return;
            } else if (shown) return;
        }
        if (!font) {
            background=dc->CreateColor(0xffeeeeee,nullptr,nullptr);
            foreground=dc->CreateColor(0xff000000,nullptr,nullptr);
            font=dc->CreateFont(6,nullptr,false,false,false,nullptr,nullptr);
        }
        if (!font || !background || !foreground) return;
        target->FillRect(0,0,target->GetWidth(),target->GetHeight(),background,false);
        PlatformDrawTextOptions o{};
        o.font=font; o.foreground=foreground;
        o.horizontalAlign=o.verticalAlign=PlatformAlign::Begin;
        target->DrawText(4,8,target->GetWidth()-Coord(8),32,error[0] ? error : "Starting r2web...",&o,false);
    }
    void event(PlatformWindowInterfaceInputEvent *e)
    {
        if (e->type==PlatformWindowInputEventType::OnImmediateModeIdleLoop) { idle(); return; }
        if (e->type==PlatformWindowInputEventType::OnPaint) { paint(e->Data.OnPaint.ctx,e->Data.OnPaint.target); return; }
        if (ended) {
            if (e->type==PlatformWindowInputEventType::OnKeyEvent && e->Data.OnKeyEvent.key->isKeyDown && e->Data.OnKeyEvent.key->isEscape) wnd->Close();
            return;
        }
        r2web::Command c{};
        switch (e->type) {
        case PlatformWindowInputEventType::OnKeyEvent: {
            auto *k=e->Data.OnKeyEvent.key;
            c.op=r2web::Key; c.value=(uint8_t)k->theChar; c.extra=(uint8_t)k->f;
#define ENCODE_FLAG(name,bit) if (k->name) c.flags|=1u<<bit;
            R2WEB_KEY_FLAGS(ENCODE_FLAG)
#undef ENCODE_FLAG
            if (k->isChar && (k->theChar=='v' || k->theChar=='V') && (k->isLeftControl || k->isRightControl))
                web::scopy(c.text,clipboardGet(),sizeof(c.text));
            break;
        }
        case PlatformWindowInputEventType::OnMouseMove:
            c.op=r2web::MouseMove; c.x=int(COORD_VAL(e->Data.OnMouseMove.mouseX)*2); c.y=int(COORD_VAL(e->Data.OnMouseMove.mouseY)*2); break;
        case PlatformWindowInputEventType::OnMouseClick:
            c.op=r2web::MouseButton; c.x=int(COORD_VAL(e->Data.OnMouseClick.mouseX)*2); c.y=int(COORD_VAL(e->Data.OnMouseClick.mouseY)*2);
            c.value=e->Data.OnMouseClick.button==PlatformWindowMouseButton::Right;
            c.extra=e->Data.OnMouseClick.state==PlatformWindowButtonState::Pressed; break;
        case PlatformWindowInputEventType::OnMouseWheel:
            c.op=r2web::Wheel; c.value=e->Data.OnMouseWheel.up; break;
        default: return;
        }
        (void)send(c);
    }
    void resize()
    {
        if (block && r2web::dimensions(block,wantWidth,wantHeight) &&
            (wantWidth!=sentWidth || wantHeight!=sentHeight)) {
            r2web::Command c{}; c.op=r2web::Resize; c.x=wantWidth; c.y=wantHeight;
            if (send(c)) { sentWidth=wantWidth; sentHeight=wantHeight; }
        }
    }
};
