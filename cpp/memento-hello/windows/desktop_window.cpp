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
    int sel = 0;

    // Draw artwork on its native 15px grid in a six-column layout.
    // Painting, hit testing and keyboard navigation share these dimensions.
    static const int BSIZ = 15;
    static const int COLS = 6;
    static const int ICONS = 16;
    static const int CELL_W = 24, CELL_H = 29;
    static const int LW = CELL_W, LH = 9;
    static const int FW = COLS * CELL_W + 20;
    static const int FH = 19 + ((ICONS + COLS - 1) / COLS) * CELL_H + 4;
    static const int TITLE_H = 11;
    int FX = (320 - FW) / 2, FY = (200 - FH) / 2;

    void layOut(int w, int h)
    {
        FX = (w - FW) / 2;
        FY = (h - FH) / 2;
    }

    int iconX(int i) const { return FX + 10 + (CELL_W - BSIZ) / 2 + (i % COLS) * CELL_W; }
    int iconY(int i) const { return FY + 19 + (i / COLS) * CELL_H; }

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
    PlatformBitmap *bmpSpotify = nullptr;

    // Hand-tuned at the displayed size: no resampling of one-pixel strokes.
    void MakeIcon(PlatformDrawingContext *dc, PlatformBitmap *&bitmap,
                  const char *const (&rows)[BSIZ])
    {
        if (bitmap) return;
        bitmap = dc->CreateBitmap(Coord(BSIZ), Coord(BSIZ), nullptr, nullptr);
        if (!bitmap) return;
        bitmap->FillRect(0, 0, BSIZ, BSIZ, dark, false);
        for (int y = 0; y < BSIZ; ++y)
            for (int x = 0; x < BSIZ; ++x)
                if (rows[y][x] == '#')
                    bitmap->FillRect(x, y, 1, 1, light, false);
    }

    void MakeBitmaps(PlatformDrawingContext *dc)
    {
        static const char *const clock[BSIZ] = {
            "...............",
            ".....#####.....",
            "...####.####...",
            "..###########..",
            "..#####.#####..",
            ".######.######.",
            ".######.######.",
            ".#.####....#.#.",
            ".#############.",
            ".#############.",
            "..###########..",
            "..###########..",
            "...####.####...",
            ".....#####.....",
            "...............",
        };
        MakeIcon(dc, bmpClock, clock);

        static const char *const shell[BSIZ] = {
            "...............",
            ".#############.",
            ".#.#.#.......#.",
            ".#############.",
            ".#...........#.",
            ".#...........#.",
            ".#..#........#.",
            ".#...#.......#.",
            ".#....#......#.",
            ".#...#.......#.",
            ".#..#..####..#.",
            ".#...........#.",
            ".#...........#.",
            ".#############.",
            "...............",
        };
        MakeIcon(dc, bmpShell, shell);

        static const char *const net[BSIZ] = {
            ".....#####.....",
            ".....#...#.....",
            ".....#...#.....",
            ".....#####.....",
            ".......#.......",
            ".......#.......",
            "...#########...",
            "...#.......#...",
            ".#####...#####.",
            ".#...#...#...#.",
            ".#...#...#...#.",
            ".#####...#####.",
            "...#.......#...",
            "..###.....###..",
            "...............",
        };
        MakeIcon(dc, bmpNet, net);

        static const char *const mount[BSIZ] = {
            "...............",
            ".##..###.......",
            ".##..###.......",
            ".##...##..##...",
            ".#############.",
            ".#############.",
            ".#.....###..###",
            ".#.....###..###",
            ".#############.",
            "........######.",
            "..##########...",
            ".############..",
            ".#.......##.#..",
            ".############..",
            "...............",
        };
        MakeIcon(dc, bmpMount, mount);

        static const char *const tasks[BSIZ] = {
            "...............",
            "...........##..",
            "...........##..",
            ".....##....##..",
            ".....##....##..",
            ".....##....##..",
            ".....##.##.##..",
            ".....##.##.##..",
            "..##.##.##.##..",
            "..##.##.##.##..",
            "..##.##.##.##..",
            "..##.##.##.##..",
            ".#############.",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpTasks, tasks);

        static const char *const chat[BSIZ] = {
            "...............",
            "...#########...",
            "..###########..",
            ".#############.",
            ".#############.",
            ".###.##.##.###.",
            ".#############.",
            ".#############.",
            "..###########..",
            "...#########...",
            "...###.........",
            "...##..........",
            "...#...........",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpChat, chat);

        static const char *const calc[BSIZ] = {
            "...............",
            "..###########..",
            "..###########..",
            "..##.......##..",
            "..##.......##..",
            "..##.......##..",
            "..###########..",
            "..###########..",
            "..##.##.##.##..",
            "..###########..",
            "..##.##.##.##..",
            "..###########..",
            "..##.##.##.##..",
            "..###########..",
            "...............",
        };
        MakeIcon(dc, bmpCalc, calc);

        static const char *const irc[BSIZ] = {
            "...............",
            "...............",
            "....##...##....",
            "....##...##....",
            "....##...##....",
            "..###########..",
            "..###########..",
            "....##...##....",
            "....##...##....",
            "..###########..",
            "..###########..",
            "....##...##....",
            "....##...##....",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpIRC, irc);

        static const char *const midi[BSIZ] = {
            "...............",
            "...............",
            "......#........",
            ".....##....#...",
            "....###.....#..",
            "..#####..#...#.",
            "..#####...#..#.",
            "..#####...#..#.",
            "..#####...#..#.",
            "..#####..#...#.",
            "....###.....#..",
            ".....##....#...",
            "......#........",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpMidi, midi);

        static const char *const web[BSIZ] = {
            ".....#####.....",
            "...##..#..##...",
            "..#.#..#..#.#..",
            ".##.#..#..#.##.",
            ".#...#.#.#...#.",
            ".#############.",
            ".#...#.#.#...#.",
            ".#...#.#.#...#.",
            ".#...#.#.#...#.",
            ".#############.",
            ".##.#..#..#.##.",
            "..#.#..#..#.#..",
            "...##..#..##...",
            ".....#####.....",
            "...............",
        };
        MakeIcon(dc, bmpWeb, web);

        static const char *const editor[BSIZ] = {
            "...............",
            "...#######.....",
            "...#.....##....",
            "...#.....#.#...",
            "...#.....####..",
            "...#........#..",
            "...#.#####..#..",
            "...#........#..",
            "...#...####.#..",
            "...#........#..",
            "...#.#####..#..",
            "...#........#..",
            "...##########..",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpEditor, editor);

        static const char *const snake[BSIZ] = {
            "...............",
            ".#############.",
            ".#...........#.",
            ".#.##........#.",
            ".#.##...####.#.",
            ".#......##.#.#.",
            ".#......####.#.",
            ".#.......##..#.",
            ".#.......##..#.",
            ".#.......##..#.",
            ".#..#######..#.",
            ".#..#######..#.",
            ".#...........#.",
            ".#############.",
            "...............",
        };
        MakeIcon(dc, bmpSnake, snake);

        static const char *const mines[BSIZ] = {
            "...............",
            ".######.######.",
            ".#...##.######.",
            ".#...##.######.",
            ".###.##.######.",
            ".##...#.######.",
            ".######.######.",
            "...............",
            ".######.######.",
            ".######.##.###.",
            ".######.#....#.",
            ".######.##..##.",
            ".######.##.###.",
            ".######.######.",
            "...............",
        };
        MakeIcon(dc, bmpMines, mines);

        static const char *const telegram[BSIZ] = {
            "...............",
            "............##.",
            "..........####.",
            ".......#######.",
            ".....######.##.",
            "..#######..##..",
            ".######...###..",
            "..####..#####..",
            "....#..#####...",
            ".....#######...",
            ".....######....",
            ".....##.###....",
            ".....#...##....",
            "...............",
            "...............",
        };
        MakeIcon(dc, bmpTelegram, telegram);

        static const char *const video[BSIZ] = {
            "...............",
            ".#############.",
            ".#.#.#.#.#.#.#.",
            ".#############.",
            ".#...........#.",
            ".#...#.......#.",
            ".#...###.....#.",
            ".#...#####...#.",
            ".#...###.....#.",
            ".#...#.......#.",
            ".#...........#.",
            ".#############.",
            ".#.#.#.#.#.#.#.",
            ".#############.",
            "...............",
        };
        MakeIcon(dc, bmpVideo, video);

        static const char *const spotify[BSIZ] = {
            ".....#####.....",
            "...#########...",
            "..###########..",
            "..###########..",
            ".###.....#####.",
            ".##..####...##.",
            ".###########.#.",
            ".###....######.",
            ".##.####..####.",
            ".#########.###.",
            "..##....#####..",
            "..######.####..",
            "...#########...",
            ".....#####.....",
            "...............",
        };
        MakeIcon(dc, bmpSpotify, spotify);

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
        const int perRow = COLS;
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
            for (int i = 0; i < ICONS; i++)
            {
                int x = iconX(i), y = iconY(i);
                // Labels launch too; their wider hit area stays in this cell.
                if (mx >= x - (LW - BSIZ) / 2 && mx < x - (LW - BSIZ) / 2 + LW &&
                    my >= y && my < y + BSIZ + 2 + LH)
                {
                    launchSel(i);
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

        PlatformBitmap *icons[ICONS] = {
            bmpClock, bmpShell, bmpNet, bmpMount, bmpTasks, bmpChat,
            bmpCalc, bmpIRC, bmpMidi, bmpWeb, bmpEditor, bmpSnake,
            bmpMines, bmpTelegram, bmpVideo, bmpSpotify,
        };
        static const char *const labels[ICONS] = {
            "Clock", "Shell", "Net", "Mount", "Tasks", "Chat",
            "Calc", "IRC", "Music", "Web", "Editor", "Snake",
            "Mines", "Telegram", "Video", "Spotify",
        };
        for (int i = 0; i < ICONS; i++)
        {
            int x = iconX(i), y = iconY(i);
            BlitIcon(target, icons[i], x, y, sel == i);
            target->DrawText(x - (LW - BSIZ) / 2, y + BSIZ + 2, LW, LH, labels[i], &opts, false);
        }

        // A program that could not be started says so here, under the frame.
        if (g_launchError[0])
            target->DrawText(FX, FY + FH + 2, FW, LH, g_launchError, &opts, false);
    }
};
