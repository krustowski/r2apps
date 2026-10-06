#pragma once
#include "host.h"
#include "ui/platform/PlatformUIRoot.h"
#include "ui/platform/PlatformWindow.h"
#include "ui/platform/impl/r2/R2_DrawingContextImpl.h"
#include "ui/platform/impl/r2/R2_BitmapImpl.h"

// Memento's software drawing context, with an offscreen window. This root
// never opens a video mode, reads an input pipe or presents to the display.
class BrowserSurface : public Memento::PlatformWindow {
public:
    bool dirty = true, immediate = false, closed = false;
    uint32_t width = 0, height = 0;
    char title[96] = "Web";
    BrowserSurface(Memento::PlatformWindowConstrData *data) : PlatformWindow(data) {}
    void input(Memento::PlatformWindowInterfaceInputEventLow &e) {
        e.instance = this;
        onEvent(&e);
    }
    void geometry(int w, int h, bool create) {
        width = w; height = h;
        using namespace Memento;
        PlatformWindowInterfaceInputEventLow e{};
        e.type = create ? PlatformWindowInputEventType::OnCreate : PlatformWindowInputEventType::OnResize;
        if (create) {
            e.Data.OnCreate.width = Dim(w); e.Data.OnCreate.height = Dim(h);
            e.Data.OnCreate.Screen.ScreenDPI = 192;
            e.Data.OnCreate.Screen.ScreenWidth = Dim(g_host->maxWidth);
            e.Data.OnCreate.Screen.ScreenHeight = Dim(g_host->maxHeight);
        } else {
            e.Data.OnResize.newWidth = Dim(w); e.Data.OnResize.newHeight = Dim(h);
            e.Data.OnResize.Screen.ScreenDPI = 192;
            e.Data.OnResize.Screen.ScreenWidth = Dim(g_host->maxWidth);
            e.Data.OnResize.Screen.ScreenHeight = Dim(g_host->maxHeight);
        }
        input(e); dirty = true;
    }
    Memento::MementoR2Impl::R2_BitmapImpl *paint() {
        Memento::PlatformWindowInterfaceInputEventLow e{};
        e.type = Memento::PlatformWindowInputEventType::OnPaint;
        dirty = false;
        input(e);
        return static_cast<Memento::MementoR2Impl::R2_BitmapImpl *>(e.Data.OnPaint.result);
    }
protected:
    bool doEvent(Memento::PlatformWindowInterface *e) override {
        using namespace Memento;
        switch (e->type) {
        case PlatformWindowInterfaceEventType::CreateDrawingContext: {
            MementoR2Impl::DrawingContextImplConstrDataExtra extra{};
            e->Data.CreateDrawingContext.result = new MementoR2Impl::R2_DrawingContextImpl(&e->Data.CreateDrawingContext, &extra);
            return e->Data.CreateDrawingContext.result != nullptr;
        }
        case PlatformWindowInterfaceEventType::Repaint:
        case PlatformWindowInterfaceEventType::SetVisible: dirty = true; return true;
        case PlatformWindowInterfaceEventType::SetImediateMode: immediate = e->Data.SetImediateMode.im; return true;
        case PlatformWindowInterfaceEventType::SetTitle:
            web::scopy(title, e->Data.SetTitle.title, sizeof(title));
            e->Data.SetTitle.hadError = false; dirty = true; return true;
        case PlatformWindowInterfaceEventType::Close: closed = true; return true;
        default: return true;
        }
    }
};
class BrowserRoot : public Memento::PlatformUIRoot {
protected:
    bool doEvent(Memento::PlatformUIRootInterface *e) override {
        using namespace Memento;
        if (e->type == PlatformUIRootInterfaceEventType::CreateWindow) {
            e->Data.CreateWindow.result = new BrowserSurface(&e->Data.CreateWindow);
            return e->Data.CreateWindow.result != nullptr;
        }
        return false;
    }
};
