//
// Window 6 — Desktop launcher
//

class DesktopWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<DesktopWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w) { wnd = w; }

    // The shell is the one launch that cannot share the screen: it wants text
    // mode, so the desktop closes for it and main() brings it back afterwards.
    // Everything else opens as a window over the desktop, which stays put.
    bool wantsShell = false;

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    int sel = 0; // 0=Clock 1=Shell 2=Net 3=Mount 4=Tasks 5=Chat 6=Calc 7=IRC 8=Music 9=Web

    // Everything here is sized around the 4x8 glyph: an icon is two text
    // lines tall, a label one, and the frame is what holds them.
    // Row 1: 5 icons at 30-px spacing; Row 2: 5 icons (Chat, Calc, IRC, Music, Web).
    // Frame is FW=170 wide, centred on a 320px canvas: FX=(320-170)/2=75.
    static const int BSIZ = 20;
    static const int FW = 170; // dialog frame width
    static const int FX = 75;  // frame left edge (centered)
    static const int FY = 28;  // frame top
    static const int FH = 92;  // frame height
    static const int TITLE_H = 11;
    static const int IX0 = 90, IX1 = 120, IX2 = 150, IX3 = 180, IX4 = 210;
    static const int IY = 46;   // row 1 icon top (below the title bar)
    static const int LY = 68;   // row 1 label top
    static const int IY2 = 80;  // row 2 icon top
    static const int LY2 = 102; // row 2 label top
    static const int LW = 30;
    static const int LH = 9;

    PlatformBitmap *bmpClock = nullptr;
    PlatformBitmap *bmpShell = nullptr;
    PlatformBitmap *bmpNet = nullptr;
    PlatformBitmap *bmpMount = nullptr;
    PlatformBitmap *bmpTasks = nullptr;
    PlatformBitmap *bmpChat = nullptr;
    PlatformBitmap *bmpCalc = nullptr;
    PlatformBitmap *bmpIRC = nullptr;
    PlatformBitmap *bmpMidi = nullptr;
    PlatformBitmap *bmpWeb = nullptr;

    //
    //  The icons are drawn for the size they are shown at: twenty logical
    //  pixels square, which is two lines of the 4x8 text face. Nothing here is
    //  a scaled-down version of a larger drawing — at this size a halved
    //  three-pixel detail lands on one and a half and comes out muddy, so each
    //  shape is placed on the 20x20 grid directly.
    //
    void MakeBitmaps(PlatformDrawingContext *dc)
    {
        // Clock: square face with rounded corners, four ticks, two hands
        if (!bmpClock)
        {
            bmpClock = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpClock)
            {
                bmpClock->FillRect(0, 0, 20, 20, dark, false);   // tile
                bmpClock->FillRect(3, 3, 14, 14, light, false);  // face
                bmpClock->FillRect(3, 3, 1, 1, dark, false);     // corner TL
                bmpClock->FillRect(16, 3, 1, 1, dark, false);    // corner TR
                bmpClock->FillRect(3, 16, 1, 1, dark, false);    // corner BL
                bmpClock->FillRect(16, 16, 1, 1, dark, false);   // corner BR
                bmpClock->FillRect(9, 4, 2, 1, dark, false);     // 12 tick
                bmpClock->FillRect(15, 9, 1, 2, dark, false);    // 3  tick
                bmpClock->FillRect(9, 15, 2, 1, dark, false);    // 6  tick
                bmpClock->FillRect(4, 9, 1, 2, dark, false);     // 9  tick
                bmpClock->FillRect(9, 6, 2, 5, dark, false);     // hour hand, at 12
                bmpClock->FillRect(10, 9, 5, 2, dark, false);    // minute hand, at 3
                bmpClock->FillRect(9, 9, 2, 2, dark, false);     // pivot
            }
        }
        // Shell: terminal window, title bar with three dots, ">_" on the body
        if (!bmpShell)
        {
            bmpShell = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpShell)
            {
                PlatformDrawTextOptions to{};
                to.font = font;
                to.foreground = light;
                to.horizontalAlign = PlatformAlign::Begin;
                to.verticalAlign = PlatformAlign::Begin;
                bmpShell->FillRect(0, 0, 20, 20, dark, false);   // tile
                bmpShell->FillRect(2, 2, 16, 16, light, false);  // window frame
                bmpShell->FillRect(3, 6, 14, 11, dark, false);   // body, leaving a title bar
                bmpShell->FillRect(4, 3, 2, 2, dark, false);     // dot 1
                bmpShell->FillRect(7, 3, 2, 2, dark, false);     // dot 2
                bmpShell->FillRect(10, 3, 2, 2, dark, false);    // dot 3
                bmpShell->DrawText(4, 8, 12, 8, ">_", &to, false);
            }
        }
        // Net: a globe, built as a disc of rows with the graticule cut out of it
        if (!bmpNet)
        {
            bmpNet = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpNet)
            {
                bmpNet->FillRect(0, 0, 20, 20, dark, false);    // tile
                bmpNet->FillRect(7, 2, 6, 1, light, false);     // disc, top row
                bmpNet->FillRect(5, 3, 10, 1, light, false);
                bmpNet->FillRect(4, 4, 12, 2, light, false);
                bmpNet->FillRect(3, 6, 14, 8, light, false);    // the wide middle
                bmpNet->FillRect(4, 14, 12, 2, light, false);
                bmpNet->FillRect(5, 16, 10, 1, light, false);
                bmpNet->FillRect(7, 17, 6, 1, light, false);    // disc, bottom row
                bmpNet->FillRect(4, 6, 12, 1, dark, false);     // upper latitude
                bmpNet->FillRect(3, 9, 14, 1, dark, false);     // equator
                bmpNet->FillRect(4, 13, 12, 1, dark, false);    // lower latitude
                bmpNet->FillRect(9, 2, 2, 16, dark, false);     // meridian
            }
        }
        // Mount: three drives, each with a light on the left
        if (!bmpMount)
        {
            bmpMount = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpMount)
            {
                bmpMount->FillRect(0, 0, 20, 20, dark, false);  // tile
                bmpMount->FillRect(2, 3, 16, 4, light, false);  // drive 1
                bmpMount->FillRect(2, 9, 16, 4, light, false);  // drive 2
                bmpMount->FillRect(2, 15, 16, 4, light, false); // drive 3
                bmpMount->FillRect(4, 4, 2, 2, dark, false);    // light 1
                bmpMount->FillRect(4, 10, 2, 2, dark, false);   // light 2
                bmpMount->FillRect(4, 16, 2, 2, dark, false);   // light 3
            }
        }
        // Tasks: four bars on a baseline
        if (!bmpTasks)
        {
            bmpTasks = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpTasks)
            {
                bmpTasks->FillRect(0, 0, 20, 20, dark, false);  // tile
                bmpTasks->FillRect(3, 11, 3, 6, light, false);  // bar 1
                bmpTasks->FillRect(7, 7, 3, 10, light, false);  // bar 2
                bmpTasks->FillRect(11, 9, 3, 8, light, false);  // bar 3
                bmpTasks->FillRect(15, 4, 3, 13, light, false); // bar 4
                bmpTasks->FillRect(2, 17, 16, 1, light, false); // baseline
            }
        }
        // Chat: speech bubble with a tail and three dots
        if (!bmpChat)
        {
            bmpChat = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpChat)
            {
                bmpChat->FillRect(0, 0, 20, 20, dark, false);   // tile
                bmpChat->FillRect(2, 3, 16, 10, light, false);  // bubble
                bmpChat->FillRect(4, 13, 4, 2, light, false);   // tail
                bmpChat->FillRect(4, 15, 2, 1, light, false);   // tail tip
                bmpChat->FillRect(5, 7, 2, 2, dark, false);     // dot 1
                bmpChat->FillRect(9, 7, 2, 2, dark, false);     // dot 2
                bmpChat->FillRect(13, 7, 2, 2, dark, false);    // dot 3
            }
        }
        // Calc: display over three rows of keys
        if (!bmpCalc)
        {
            bmpCalc = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpCalc)
            {
                bmpCalc->FillRect(0, 0, 20, 20, dark, false);   // tile
                bmpCalc->FillRect(2, 2, 16, 16, light, false);  // body
                bmpCalc->FillRect(4, 4, 12, 4, dark, false);    // display
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        bmpCalc->FillRect(4 + c * 5, 10 + r * 3, 3, 2, dark, false);
            }
        }
        // IRC: a hash
        if (!bmpIRC)
        {
            bmpIRC = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpIRC)
            {
                bmpIRC->FillRect(0, 0, 20, 20, dark, false);    // tile
                bmpIRC->FillRect(6, 3, 2, 14, light, false);    // left vertical
                bmpIRC->FillRect(12, 3, 2, 14, light, false);   // right vertical
                bmpIRC->FillRect(3, 6, 14, 2, light, false);    // top horizontal
                bmpIRC->FillRect(3, 12, 14, 2, light, false);   // bottom horizontal
            }
        }
        // Music: a speaker cone with two arcs coming off it
        if (!bmpMidi)
        {
            bmpMidi = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpMidi)
            {
                bmpMidi->FillRect(0, 0, 20, 20, dark, false);  // tile
                bmpMidi->FillRect(3, 8, 3, 4, light, false);   // the box
                bmpMidi->FillRect(6, 6, 2, 8, light, false);   // cone, near
                bmpMidi->FillRect(8, 4, 2, 12, light, false);  // cone, far
                bmpMidi->FillRect(12, 7, 2, 6, light, false);  // inner arc
                bmpMidi->FillRect(15, 5, 2, 10, light, false); // outer arc
            }
        }
    }

    void MakeWebBitmap(PlatformDrawingContext *dc)
    {
        // Web: a page with a link on it and the arrow pointing at the link
        if (bmpWeb)
            return;
        bmpWeb = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpWeb)
            return;
        bmpWeb->FillRect(0, 0, 20, 20, dark, false);   // tile
        bmpWeb->FillRect(2, 2, 16, 16, light, false);  // the page
        bmpWeb->FillRect(2, 2, 16, 3, dark, false);    // its address bar
        bmpWeb->FillRect(3, 3, 1, 1, light, false);    // back button
        bmpWeb->FillRect(6, 3, 11, 1, light, false);   // the address
        bmpWeb->FillRect(4, 7, 10, 1, dark, false);    // a line of text
        bmpWeb->FillRect(4, 9, 7, 1, dark, false);     // the link
        bmpWeb->FillRect(4, 10, 7, 1, dark, false);    //   and its underline
        bmpWeb->FillRect(4, 12, 6, 1, dark, false);    // more text
        bmpWeb->FillRect(9, 11, 1, 6, dark, false);    // the pointer: a stem
        bmpWeb->FillRect(10, 12, 1, 4, dark, false);   //   widening
        bmpWeb->FillRect(11, 13, 1, 2, dark, false);   //   to a tip
        bmpWeb->FillRect(12, 14, 1, 1, dark, false);
    }

    void BlitIcon(PlatformBitmap *t, PlatformBitmap *bm, int ix, int iy, bool s)
    {
        if (s)
            t->FillRect(ix - 2, iy - 2, BSIZ + 4, BSIZ + 4, dark, false);
        if (bm)
            t->CopyBitmap(ix, iy, BSIZ, BSIZ, bm, 0, 0, false, false, false, 255);
    }

    void launchSel(int s)
    {
        if (s == APP_SHELL)
        {
            wantsShell = true;
            wnd->Close();
            return;
        }
        openApp(s);
        wnd->Repaint();
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            // Row 1: Clock..Tasks at IX0..IX4; Row 2: Chat at IX0, Calc at IX1
            const int IXs[5] = {IX0, IX1, IX2, IX3, IX4};
            if (my >= IY && my < IY + BSIZ)
            {
                for (int i = 0; i < 5; i++)
                {
                    if (mx >= IXs[i] && mx < IXs[i] + BSIZ)
                    {
                        launchSel(i);
                        return;
                    }
                }
            }
            if (my >= IY2 && my < IY2 + BSIZ)
            {
                if (mx >= IX0 && mx < IX0 + BSIZ)
                {
                    launchSel(5);
                    return;
                }
                if (mx >= IX1 && mx < IX1 + BSIZ)
                {
                    launchSel(6);
                    return;
                }
                if (mx >= IX2 && mx < IX2 + BSIZ)
                {
                    launchSel(7);
                    return;
                }
                if (mx >= IX3 && mx < IX3 + BSIZ)
                {
                    launchSel(8);
                    return;
                }
                if (mx >= IX4 && mx < IX4 + BSIZ)
                {
                    launchSel(9);
                    return;
                }
            }
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        if (key->isEscape)
        {
            wnd->Close();
            return;
        }
        if (key->isArrowLeft || key->isArrowUp)
        {
            sel = (sel + 9) % 10;
            wnd->Repaint();
            return;
        }
        if (key->isArrowRight || key->isArrowDown)
        {
            sel = (sel + 1) % 10;
            wnd->Repaint();
            return;
        }
        if (key->isEnter)
        {
            launchSel(sel);
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!dark)
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light)
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font)
            return;
        MakeBitmaps(dc);
        MakeWebBitmap(dc);

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, dark, false);
        drawWallpaper(dc, target);

        // Dialog frame — centered, width FW, left edge at FX
        target->FillRect(FX, FY, FW, FH, dark, false);
        target->FillRect(FX + 2, FY + 2, FW - 4, FH - 4, light, false);
        target->FillRect(FX + 2, FY + 2 + TITLE_H, FW - 4, 1, dark, false); // title separator

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        target->DrawText(FX + 2, FY + 2, FW - 4, TITLE_H, "Desktop", &opts, false);

        // Row 1 icons (Clock, Shell, Net, Mount, Tasks)
        BlitIcon(target, bmpClock, IX0, IY, sel == 0);
        BlitIcon(target, bmpShell, IX1, IY, sel == 1);
        BlitIcon(target, bmpNet, IX2, IY, sel == 2);
        BlitIcon(target, bmpMount, IX3, IY, sel == 3);
        BlitIcon(target, bmpTasks, IX4, IY, sel == 4);
        // Row 2 icons (Chat, Calc, IRC)
        BlitIcon(target, bmpChat, IX0, IY2, sel == 5);
        BlitIcon(target, bmpCalc, IX1, IY2, sel == 6);
        BlitIcon(target, bmpIRC, IX2, IY2, sel == 7);
        BlitIcon(target, bmpMidi, IX3, IY2, sel == 8);
        BlitIcon(target, bmpWeb, IX4, IY2, sel == 9);

        // Labels
        PlatformDrawTextOptions lo{};
        lo.font = font;
        lo.foreground = dark;
        lo.horizontalAlign = PlatformAlign::Middle;
        lo.verticalAlign = PlatformAlign::Middle;
        const int loff = (LW - BSIZ) / 2; // 7 px: centres LW box on BSIZ icon
        target->DrawText(IX0 - loff, LY, LW, LH, "Clock", &lo, false);
        target->DrawText(IX1 - loff, LY, LW, LH, "Shell", &lo, false);
        target->DrawText(IX2 - loff, LY, LW, LH, "Net", &lo, false);
        target->DrawText(IX3 - loff, LY, LW, LH, "Mount", &lo, false);
        target->DrawText(IX4 - loff, LY, LW, LH, "Tasks", &lo, false);
        target->DrawText(IX0 - loff, LY2, LW, LH, "Chat", &lo, false);
        target->DrawText(IX1 - loff, LY2, LW, LH, "Calc", &lo, false);
        target->DrawText(IX2 - loff, LY2, LW, LH, "IRC", &lo, false);
        target->DrawText(IX3 - loff, LY2, LW, LH, "Music", &lo, false);
        target->DrawText(IX4 - loff, LY2, LW, LH, "Web", &lo, false);
    }
};
