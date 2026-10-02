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

    // Everything opens as a window over the desktop, which stays put --- the
    // shell too, since r2sh learnt --host (windows/shell_window.cpp).

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    int sel = 0; // 0=Clock 1=Shell 2=Net 3=Mount 4=Tasks 5=Chat 6=Calc 7=IRC 8=Music 9=Web 10=Editor 11=Snake 12=Mines 13=Telegram 14=Video

    // Everything here is sized around the 4x8 glyph: an icon is two text
    // lines tall, a label one, and the frame is what holds them.
    // Row 1: 5 icons at 30-px spacing; Row 2: 5 icons (Chat, Calc, IRC, Music, Web);
    // Row 3: the Editor (Turbo C++ in a window), Snake, Minesweeper, Telegram and Video.
    // Frame is FW=170 wide, centred on a 320px canvas: FX=(320-170)/2=75.
    static const int BSIZ = 20;
    static const int FW = 170; // dialog frame width
    //  Positions, laid out on the 320x200 canvas of the VGA's screen and
    //  moved by layOut() so the dialog stays in the middle of a larger one
    //  (the graphics kernel's framebuffer).  Members rather than constants:
    //  the clicks are tested against the same numbers the paint uses.
    int FX = 75;  // frame left edge (centered)
    int FY = 28;  // frame top
    static const int FH = 126; // frame height
    static const int TITLE_H = 11;
    int IX0 = 90, IX1 = 120, IX2 = 150, IX3 = 180, IX4 = 210;
    int IY = 46;   // row 1 icon top (below the title bar)
    int LY = 68;   // row 1 label top
    int IY2 = 80;  // row 2 icon top
    int LY2 = 102; // row 2 label top
    int IY3 = 114; // row 3 icon top
    int LY3 = 136; // row 3 label top
    int layoutDX = 0, layoutDY = 0;

    //  The dialog in the middle of a w x h window, moving every position by
    //  the change from the last time.
    void layOut(int w, int h)
    {
        int dx = w > 320 ? (w - 320) / 2 : 0, dy = h > 200 ? (h - 200) / 2 : 0;
        int mx = dx - layoutDX, my = dy - layoutDY;
        if (!mx && !my)
            return;
        FX += mx, IX0 += mx, IX1 += mx, IX2 += mx, IX3 += mx, IX4 += mx;
        FY += my, IY += my, LY += my, IY2 += my, LY2 += my, IY3 += my, LY3 += my;
        layoutDX = dx, layoutDY = dy;
    }
    static const int ICONS = 15;
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
    PlatformBitmap *bmpEditor = nullptr;
    PlatformBitmap *bmpSnake = nullptr;
    PlatformBitmap *bmpMines = nullptr;
    PlatformBitmap *bmpTelegram = nullptr;
    PlatformBitmap *bmpVideo = nullptr;

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
        // Net: three nodes, each linked to the other two
        if (!bmpNet)
        {
            bmpNet = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpNet)
            {
                bmpNet->FillRect(0, 0, 20, 20, dark, false);    // tile
                for (int i = 0; i < 4; i++)
                {
                    bmpNet->FillRect(7 - i, 8 + i, 2, 1, light, false);  // link, top to left
                    bmpNet->FillRect(11 + i, 8 + i, 2, 1, light, false); // link, top to right
                }
                bmpNet->FillRect(8, 14, 4, 2, light, false);    // link, left to right
                static const unsigned char nodes[][2] = {{7, 2}, {2, 12}, {12, 12}};
                for (auto &n : nodes)
                {
                    bmpNet->FillRect(n[0] + 1, n[1], 4, 6, light, false); // a node: 6x6
                    bmpNet->FillRect(n[0], n[1] + 1, 6, 4, light, false); //   less its corners
                }
            }
        }
        // Mount: three drives, each with a light on the left
        if (!bmpMount)
        {
            bmpMount = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
            if (bmpMount)
            {
                bmpMount->FillRect(0, 0, 20, 20, dark, false);  // tile
                bmpMount->FillRect(2, 2, 16, 4, light, false);  // drive 1
                bmpMount->FillRect(2, 8, 16, 4, light, false);  // drive 2
                bmpMount->FillRect(2, 14, 16, 4, light, false); // drive 3
                bmpMount->FillRect(4, 3, 2, 2, dark, false);    // light 1
                bmpMount->FillRect(4, 9, 2, 2, dark, false);    // light 2
                bmpMount->FillRect(4, 15, 2, 2, dark, false);   // light 3
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
                bmpCalc->FillRect(4, 4, 12, 3, dark, false);    // display
                for (int r = 0; r < 3; r++)                     // keys, flush with its edges
                    for (int c = 0; c < 3; c++)
                        bmpCalc->FillRect(4 + c * 5, 8 + r * 3, 2, 2, dark, false);
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

    void MakeEditorBitmap(PlatformDrawingContext *dc)
    {
        // Editor: a page of indented code under a title bar, and the cursor
        if (bmpEditor)
            return;
        bmpEditor = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpEditor)
            return;
        bmpEditor->FillRect(0, 0, 20, 20, dark, false);  // tile
        bmpEditor->FillRect(2, 2, 16, 16, light, false); // the page
        bmpEditor->FillRect(2, 2, 16, 2, dark, false);   // its menu bar
        bmpEditor->FillRect(4, 6, 6, 1, dark, false);    // int main() {
        bmpEditor->FillRect(6, 8, 8, 1, dark, false);    //   a line
        bmpEditor->FillRect(6, 10, 5, 1, dark, false);   //   another
        bmpEditor->FillRect(6, 12, 9, 1, dark, false);   //   a longer one
        bmpEditor->FillRect(4, 14, 2, 1, dark, false);   // }
        bmpEditor->FillRect(12, 10, 2, 3, dark, false);  // the cursor
    }

    void MakeSnakeBitmap(PlatformDrawingContext *dc)
    {
        // Snake: a board, a snake bent round a corner towards a square of food
        if (bmpSnake)
            return;
        bmpSnake = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpSnake)
            return;
        bmpSnake->FillRect(0, 0, 20, 20, dark, false);  // tile
        bmpSnake->FillRect(2, 2, 16, 16, light, false); // the board
        bmpSnake->FillRect(4, 14, 9, 2, dark, false);   // the tail, along the bottom
        bmpSnake->FillRect(11, 7, 2, 9, dark, false);   // round the corner and up
        bmpSnake->FillRect(10, 5, 4, 3, dark, false);   // the head
        bmpSnake->FillRect(11, 6, 1, 1, light, false);  //   an eye
        bmpSnake->FillRect(5, 5, 2, 2, dark, false);    // the food
    }

    void MakeMinesBitmap(PlatformDrawingContext *dc)
    {
        // Minesweeper: four tiles, a flag on one and a mine on another
        if (bmpMines)
            return;
        bmpMines = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpMines)
            return;
        bmpMines->FillRect(0, 0, 20, 20, dark, false);  // tile
        bmpMines->FillRect(2, 2, 7, 7, light, false);   // closed, flagged
        bmpMines->FillRect(11, 2, 7, 7, light, false);  // closed
        bmpMines->FillRect(2, 11, 7, 7, light, false);  // closed
        bmpMines->FillRect(11, 11, 7, 7, light, false); // open, the mine
        bmpMines->FillRect(5, 3, 1, 5, dark, false);    //   the pole
        bmpMines->FillRect(3, 3, 2, 2, dark, false);    //   the flag
        bmpMines->FillRect(4, 7, 3, 1, dark, false);    //   its foot
        bmpMines->FillRect(13, 13, 3, 3, dark, false);  //   the mine
        bmpMines->FillRect(14, 12, 1, 5, dark, false);  //   and its spikes
        bmpMines->FillRect(12, 14, 5, 1, dark, false);
    }

    void MakeVideoBitmap(PlatformDrawingContext *dc)
    {
        // Video: a strip of film, sprocket holes along both edges and a
        // picture in the middle frame
        if (bmpVideo)
            return;
        bmpVideo = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpVideo)
            return;
        bmpVideo->FillRect(0, 0, 20, 20, dark, false);  // tile
        bmpVideo->FillRect(2, 3, 16, 14, light, false); // the strip
        for (int x = 3; x < 18; x += 3)
        {
            bmpVideo->FillRect(x, 4, 2, 2, dark, false);  // holes along the top
            bmpVideo->FillRect(x, 14, 2, 2, dark, false); //   and the bottom
        }
        bmpVideo->FillRect(4, 7, 12, 6, dark, false);  // the frame
        bmpVideo->FillRect(8, 8, 1, 4, light, false);  // a play mark in it
        bmpVideo->FillRect(9, 8, 1, 4, light, false);
        bmpVideo->FillRect(10, 9, 1, 2, light, false);
        bmpVideo->FillRect(11, 9, 1, 2, light, false);
    }

    void MakeTelegramBitmap(PlatformDrawingContext *dc)
    {
        // Telegram: a paper plane flying up and to the right, drawn in rows
        // from its nose at the top right to its tail at the bottom left
        if (bmpTelegram)
            return;
        bmpTelegram = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bmpTelegram)
            return;
        bmpTelegram->FillRect(0, 0, 20, 20, dark, false); // tile
        //  The wing: each row starts further left and ends at or past the
        //  fold, which runs from the nose (15,6) down to the tail (9,12).
        static const unsigned char wing[][3] = {
            // y, x from, x to (exclusive)
            {3, 17, 18},  {4, 15, 18},  {5, 12, 18},  {6, 10, 17},  {7, 7, 17},
            {8, 4, 16},   {9, 2, 16},   {10, 4, 15},  {11, 6, 15},  {12, 8, 14},
            {13, 8, 14},  {14, 9, 13},  {15, 9, 13},  {16, 9, 12},  {17, 9, 11},
        };
        for (auto &r : wing)
            bmpTelegram->FillRect(r[1], r[0], r[2] - r[1], 1, light, false);
        //  The fold, in the tile's colour, from the nose to the tail.
        for (int y = 6; y < 13; y++)
            bmpTelegram->FillRect(21 - y, y, 1, 1, dark, false);
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
        openApp(s);
        wnd->Repaint();
    }

    // Up and Down: the same column a row away, round from the last row to the
    // first and back.  The last row is short; a column it has no icon in
    // lands on its last one.
    static int rowStep(int s, int dir)
    {
        const int perRow = 5;
        const int rows = (ICONS + perRow - 1) / perRow;
        int row = (s / perRow + dir + rows) % rows;
        int at = row * perRow + s % perRow;
        return at < ICONS ? at : ICONS - 1;
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
            if (my >= IY3 && my < IY3 + BSIZ && mx >= IX0 && mx < IX0 + BSIZ)
            {
                launchSel(10);
                return;
            }
            if (my >= IY3 && my < IY3 + BSIZ && mx >= IX1 && mx < IX1 + BSIZ)
            {
                launchSel(11);
                return;
            }
            if (my >= IY3 && my < IY3 + BSIZ && mx >= IX2 && mx < IX2 + BSIZ)
            {
                launchSel(12);
                return;
            }
            if (my >= IY3 && my < IY3 + BSIZ && mx >= IX3 && mx < IX3 + BSIZ)
            {
                launchSel(13);
                return;
            }
            if (my >= IY3 && my < IY3 + BSIZ && mx >= IX4 && mx < IX4 + BSIZ)
            {
                launchSel(14);
                return;
            }
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        //  Esc logs out: the windows close with the desktop, and main()
        //  shows the login dialog again.
        if (key->isEscape)
        {
            g_logout = true;
            static_cast<MementoR2Impl::R2_WindowImpl *>(wnd)->CloseOtherWindows();
            wnd->Close();
            return;
        }
        if (key->isArrowLeft)
        {
            sel = (sel + ICONS - 1) % ICONS;
            wnd->Repaint();
            return;
        }
        if (key->isArrowRight)
        {
            sel = (sel + 1) % ICONS;
            wnd->Repaint();
            return;
        }
        if (key->isArrowUp || key->isArrowDown)
        {
            sel = rowStep(sel, key->isArrowDown ? 1 : -1);
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
        MakeEditorBitmap(dc);
        MakeSnakeBitmap(dc);
        MakeMinesBitmap(dc);
        MakeTelegramBitmap(dc);
        MakeVideoBitmap(dc);

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();
        layOut((int)COORD_VAL(W), (int)COORD_VAL(H));

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
        // Row 3 icons (Editor)
        BlitIcon(target, bmpEditor, IX0, IY3, sel == 10);
        BlitIcon(target, bmpSnake, IX1, IY3, sel == 11);
        BlitIcon(target, bmpMines, IX2, IY3, sel == 12);
        BlitIcon(target, bmpTelegram, IX3, IY3, sel == 13);
        BlitIcon(target, bmpVideo, IX4, IY3, sel == 14);

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
        target->DrawText(IX0 - loff, LY3, LW, LH, "Editor", &lo, false);
        target->DrawText(IX1 - loff, LY3, LW, LH, "Snake", &lo, false);
        target->DrawText(IX2 - loff, LY3, LW, LH, "Mines", &lo, false);
        target->DrawText(IX3 - loff, LY3, LW, LH, "Telegram", &lo, false);
        target->DrawText(IX4 - loff, LY3, LW, LH, "Video", &lo, false);

        // A program that could not be started says so here, under the frame.
        if (g_launchError[0])
            target->DrawText(FX, FY + FH + 2, FW, LH, g_launchError, &lo, false);
    }
};
