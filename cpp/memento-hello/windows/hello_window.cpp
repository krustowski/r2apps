//
// Window 1 — Hello
//

class HelloWindow
{
public:
    bool wantsNext = false;

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<HelloWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *bg = nullptr;
    PlatformColor *fg = nullptr;
    PlatformFont *font = nullptr;

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
        }
        else if (data->type == PlatformWindowInputEventType::OnKeyEvent)
        {
            auto *key = data->Data.OnKeyEvent.key;
            if (key->isEscape)
                wnd->Close();
            if (key->isEnter)
            {
                wantsNext = true;
                wnd->Close();
            }
        }
        else if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
            {
                wantsNext = true;
                wnd->Close();
            }
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!bg)
            bg = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!fg)
            fg = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)
            // The landing screen keeps the large face; everything past it is
            // laid out around the small one.
            font = dc->CreateFont(16, nullptr, false, false, false, nullptr, nullptr);
        if (!bg || !fg || !font)
            return;

        Coord w = target->GetWidth();
        Coord h = target->GetHeight();

        target->FillRect(0, 0, w, h, bg, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = fg;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        target->DrawText(0, h / 4, w, 32, "Hello r2!", &opts, false);
        target->DrawText(0, h / 2 - 2, w, 18, "Enter  -  login", &opts, false);
        target->DrawText(0, h / 2 + 18, w, 18, "ESC  -  quit", &opts, false);
    }
};
