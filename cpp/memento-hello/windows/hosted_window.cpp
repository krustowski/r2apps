// Shared host for r2web.elf, jug.elf and telegram.elf: indexed frames, input
// and lifetime.
#include "../../r2web/host.h"
#include "../web/png.h"
#include "../web/wbase.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"
struct HostedSlot { r2web::HostBlock *block; uint8_t pid; bool orphan; const char *program; };
static HostedSlot g_hostedSlots[8] = {};
static bool hostedAlive(uint8_t pid, const char *program)
{
    auto tasks = r2::tasks();
    if (tasks.empty()) return true;
    for (const auto &t : tasks)
        if (t.id == pid) {
            size_t n = 0;
            while (program[n] && program[n] != '.' && n < 8) ++n;
            for (size_t i = 0; i < n; ++i)
                if ((t.name[i] | 32) != (program[i] | 32)) return false;
            return t.mode == 1 && t.status < 4 && (t.name[n] == ' ' || t.name[n] == '.');
        }
    return false;
}
static void hostedKeepAlive()
{
    for (auto &s : g_hostedSlots) {
        if (!s.block) continue;
        if (!s.orphan) r2web::store(&s.block->hostBeat, r2web::load(&s.block->hostBeat)+1);
        else if (r2web::load(&s.block->exited) || !hostedAlive(s.pid, s.program)) {
            r2::heap::kernel_deallocate(s.block); s = {};
        }
    }
}
//  The clipboard's picture as a PNG on the RAM disk (or the floppy), for a
//  program that takes pictures: where, and how long it is.  The file is
//  written over rather than replaced, so what lies past len is stale.
static bool clipboardPictureFile(char *path, size_t cap, uint32_t &len)
{
    static const char *const places[] = {"/mnt/tmp/CLIP.PNG", "/mnt/fat/CLIP.PNG"};
    web::Buf png{true};
    if (!clipboardHasImage() ||
        !web::encodePng(g_clipImage, g_clipImageW, g_clipImageH, g_clipPalette, g_clipColours, png))
        return false;
    for (const char *p : places)
        if (r2::fs::write_at(r2::string_view(p, strlen(p)), r2::const_byte_span(png.data, png.len), 0) ==
            (int64_t)png.len) {
            web::scopy(path, p, cap); len = (uint32_t)png.len; return true;
        }
    return false;
}
class HostedWindow {
public:
    //  pictures: a Ctrl+V with a picture on the clipboard hands the program
    //  a PNG of it (r2web::PasteImage) rather than the clipboard's text.
    HostedWindow(const char *program, const char *title, uint32_t magic, uint32_t portBase, const char *url = nullptr,
                 bool pictures = false)
        : program_(program), title_(title), pictures_(pictures), magic_(magic)
    {
        hostedKeepAlive();
        for (int i=0; i<8; ++i) if (!g_hostedSlots[i].block) { slot=i; break; }
        if (slot<0) { strcpy(error,"Too many hosted processes."); return; }
        int w,h;
        MementoR2Impl::R2_Vga640x400::ScreenSize(w,h);
        if (w>int(r2web::MaxDimension)) w=r2web::MaxDimension;
        if (h>int(r2web::MaxDimension)) h=r2web::MaxDimension;
        if (w<=0 || h<=0) { strcpy(error,"Invalid screen size."); return; }
        block=(r2web::HostBlock *)r2::heap::kernel_allocate(r2web::blockSize(w*h));
        if (!block) { strcpy(error,"No memory for the window host block."); return; }
        portBase_=portBase ? portBase : 48000+slot*32; width_=w; height_=h;
        web::scopy(url_,url && *url ? url : "about:home",sizeof(url_));
        if (!launch()) { r2::heap::kernel_deallocate(block); block=nullptr; }
    }
    ~HostedWindow()
    {
        if (!block) return;
        r2web::store(&block->quit,1);
        for (int i=0; i<50 && !r2web::load(&block->exited) && hostedAlive(pid, program_); ++i) r2::sleep(20);
        // The child must stop using shared memory before its owner frees it.
        if (!r2web::load(&block->exited) && hostedAlive(pid, program_)) (void)r2::kill(pid);
        if (r2web::load(&block->exited) || !hostedAlive(pid, program_)) {
            r2::heap::kernel_deallocate(block); g_hostedSlots[slot]={};
        } else g_hostedSlots[slot].orphan=true;
    }
    bool failed() const { return !block; }
    const char *why() const { return error; }
    void SetWindow(PlatformWindow *w) { wnd=w; wnd->SetImmediateMode(true); }
    static void onEvent(void *p,PlatformWindowInterfaceInputEvent *e) { ((HostedWindow *)p)->event(e); }
private:
    const char *program_, *title_;
    bool pictures_;
    uint32_t magic_, portBase_=0, width_=0, height_=0; // the block's largest frame
    char url_[r2web::TextCapacity]={};
    PlatformWindow *wnd=nullptr;
    r2web::HostBlock *block=nullptr;
    int slot=-1;
    uint8_t pid=0;
    uint32_t shown=0,sentWidth=0,sentHeight=0,wantWidth=0,wantHeight=0;
    uint64_t started=0,lastCheck=0;
    bool ended=false;
    char error[128]={};
    uint8_t taskStatus=0;
    uint64_t taskRip=0;
    //  The block made ready for a child, and the child started on it: at
    //  first, and again when one that ended is restarted in its window.
    bool launch()
    {
        uint32_t w=width_,h=height_;
        memset(block,0,sizeof(*block));
        block->magic=magic_; block->version=r2web::Version;
        block->capacity=w*h; block->maxWidth=w; block->maxHeight=h;
        block->colours=MementoR2Impl::R2_Palette::Count();
        block->portBase=portBase_; block->front=r2web::None; block->hostBeat=1;
        web::scopy(block->initialUrl,url_,sizeof(block->initialUrl));
        char args[64]; web::scopy(args,program_,sizeof(args));
        web::scat(args," --host 0x",sizeof(args));
        size_t at=strlen(args);
        uintptr_t address=(uintptr_t)block;
        for (int shift=60; shift>=0; shift-=4) args[at++]="0123456789abcdef"[(address>>shift)&15];
        args[at]=0;
        auto id=r2::spawn(program_,args);
        if (!id) {
            web::scopy(error,"Cannot start ",sizeof(error)); web::scat(error,program_,sizeof(error));
            web::scat(error,"; install it in the bin directory.",sizeof(error));
            return false;
        }
        pid=*id; g_hostedSlots[slot]={block,pid,false,program_};
        started=r2::ticks(); lastCheck=0; ended=false; error[0]=0;
        shown=sentWidth=sentHeight=0;
        return true;
    }
    //  Enter on a window whose program has ended: it starts again there.
    //  The old one is gone (its task ended, or it said it had), so nothing
    //  else uses the block.
    void restart()
    {
        if (!launch()) { wnd->Repaint(); return; }
        wnd->SetImmediateMode(true); wnd->Repaint();
    }
    //  Whether the child is still there, noting how it is (and where it was,
    //  for a crash).  A task table that cannot be read says it is.
    bool alive()
    {
        auto tasks=r2::tasks();
        if (tasks.empty()) return true;
        for (const auto &t : tasks)
            if (t.id==pid) { taskStatus=t.status; taskRip=t.rip; break; }
        return hostedAlive(pid, program_);
    }
    //  Why the window has nothing to show: the program's own words, or a
    //  crash and where it happened, or just that it ended.
    void describeEnd()
    {
        if (block->error[0]) { web::scopy(error,block->error,sizeof(error)); return; }
        web::scopy(error,title_,sizeof(error));
        if (taskStatus==4) {
            web::scat(error," crashed at 0x",sizeof(error));
            char hex[17]; for (int i=0; i<16; ++i) hex[i]="0123456789abcdef"[(taskRip>>(60-4*i))&15];
            hex[16]=0; web::scat(error,hex,sizeof(error));
            web::scat(error,".",sizeof(error));
        } else web::scat(error," has ended.",sizeof(error));
    }
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
        // Ignored while the window has the focus: it is being looked at.
        if (r2web::load(&block->attentionPending)) {
            r2web::store(&block->attentionPending,0); wnd->SetAttention(true);
        }
        if (r2web::load(&block->frame)!=shown) wnd->Repaint();
        uint64_t now=r2::ticks();
        if (now-started<2000 || now-lastCheck<1000) return;
        lastCheck=now;
        if (r2web::load(&block->exited) || !alive()) {
            ended=true; describeEnd();
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
        char starting[64]="Starting "; web::scat(starting,title_,sizeof(starting));
        web::scat(starting,"...",sizeof(starting));
        target->DrawText(4,8,target->GetWidth()-Coord(8),8,error[0] ? error : starting,&o,false);
        if (ended && block)
            target->DrawText(4,20,target->GetWidth()-Coord(8),8,"Enter starts it again; Esc closes the window.",&o,false);
    }
    void event(PlatformWindowInterfaceInputEvent *e)
    {
        if (e->type==PlatformWindowInputEventType::OnImmediateModeIdleLoop) { idle(); return; }
        if (e->type==PlatformWindowInputEventType::OnPaint) { paint(e->Data.OnPaint.ctx,e->Data.OnPaint.target); return; }
        if (ended) {
            if (e->type==PlatformWindowInputEventType::OnKeyEvent && e->Data.OnKeyEvent.key->isKeyDown) {
                if (e->Data.OnKeyEvent.key->isEscape) wnd->Close();
                else if (e->Data.OnKeyEvent.key->isEnter && block) restart();
            }
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
            if (k->isChar && (k->theChar=='v' || k->theChar=='V') && (k->isLeftControl || k->isRightControl)) {
                uint32_t n=0;
                if (pictures_ && clipboardPictureFile(c.text,sizeof(c.text),n)) { c.x=r2web::PasteImage; c.y=(int32_t)n; }
                else web::scopy(c.text,clipboardGet(),sizeof(c.text));
            }
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
