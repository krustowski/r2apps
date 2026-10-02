//
// Window — Minesweeper
//
// The three boards everyone knows: 9x9 with 10 mines, 16x16 with 40 and 30x16
// with 99.  The first cell opened is never a mine, nor is any cell around it,
// so every game starts with an opening rather than a guess; the mines are laid
// only then.
//
//      left click, Space      open a cell
//      right click, F         flag it (again: take the flag off)
//      click an open number   when its flags are all there, open the rest
//                             of its neighbours (a "chord")
//      arrows                 move the keyboard cursor
//      N, or the face         a new game
//      1 2 3                  beginner, intermediate, expert
//      Esc                    close
//
// Best times, one per level, are kept in /mnt/fat/MINES.HSC.
//

class MinesWindow
{
public:
    static const int CS = 8;           // a cell, in window units (16 pixels)
    static const int MAX_W = 30, MAX_H = 16;
    static const int TOP = 16;         // the counter, the face and the clock
    static const int FOOT = 11;        // the key hints
    static const int W = MAX_W * CS + 6;
    static const int H = TOP + MAX_H * CS + 4 + FOOT;

    MinesWindow()
    {
        rng = r2::ticks() ^ 0x9E3779B97F4A7C15ull;
        loadBest();
        newGame(1);
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<MinesWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        // The idle loop runs the clock.
        wnd->SetImmediateMode(true);
    }

private:
    static constexpr const char *BEST_PATH = "/mnt/fat/MINES.HSC";

    struct Level
    {
        int w, h, mines;
        const char *name;
    };
    static const Level &level(int i)
    {
        static const Level levels[3] = {
            {9, 9, 10, "Beginner"}, {16, 16, 40, "Intermediate"}, {30, 16, 99, "Expert"}};
        return levels[i];
    }

    enum State : uint8_t
    {
        Ready,   // no cell open yet: the mines are not laid
        Playing,
        Won,
        Lost
    };

    struct Cell
    {
        bool mine;
        bool open;
        bool flag;
        uint8_t around; // mines in the eight neighbours
    };

    PlatformWindow *wnd = nullptr;
    Cell cells[MAX_W * MAX_H];
    int lvl = 1, gw = 16, gh = 16, mines = 40;
    State state = Ready;
    int opened = 0, flags = 0;
    int boom = -1;             // the mine that went off
    uint64_t startedAt = 0;    // ticks at the first open
    uint32_t seconds = 0;      // shown on the clock; frozen when it ends
    uint32_t best[3] = {0, 0, 0}; // seconds, 0 for none yet
    bool newBest = false;
    int cx = 0, cy = 0;        // the keyboard cursor
    bool showCursor = false;   // only once a key has moved it
    bool pressFace = false;
    uint64_t rng;

    PlatformColor *col[16] = {};
    PlatformFont *font = nullptr;

    //  Everything is laid out in the W x H units of the window as it opens,
    //  and drawn zoomed by z from (ox, oy): a larger (maximised) window shows
    //  the whole of it larger, centred.  Set by each paint, and used to map a
    //  click back.  The text comes in whole sizes only, so it has a font of
    //  its own for each.
    double z = 1, ox = 0, oy = 0;
    PlatformFont *fonts[8] = {};

    // ── Rules ────────────────────────────────────────────────────────────────

    uint32_t random(uint32_t bound)
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (uint32_t)(rng % bound);
    }

    Cell &at(int x, int y) { return cells[y * gw + x]; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < gw && y < gh; }

    void newGame(int l)
    {
        lvl = l;
        gw = level(l).w;
        gh = level(l).h;
        mines = level(l).mines;
        memset(cells, 0, sizeof(cells));
        state = Ready;
        opened = flags = 0;
        boom = -1;
        seconds = 0;
        newBest = false;
        if (cx >= gw)
            cx = gw - 1;
        if (cy >= gh)
            cy = gh - 1;
        if (wnd)
            wnd->Repaint();
    }

    //  Lays the mines anywhere but the 3x3 around the first cell opened.
    void layMines(int fx, int fy)
    {
        int free_ = 0;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                if (x < fx - 1 || x > fx + 1 || y < fy - 1 || y > fy + 1)
                    free_++;
        int toLay = mines < free_ ? mines : free_;
        while (toLay > 0)
        {
            // The n-th free cell, counted afresh: no retry loop to run long
            // on a crowded board.
            int n = (int)random((uint32_t)free_);
            for (int i = 0; i < gw * gh; i++)
            {
                int x = i % gw, y = i / gw;
                if (cells[i].mine || (x >= fx - 1 && x <= fx + 1 && y >= fy - 1 && y <= fy + 1))
                    continue;
                if (n-- == 0)
                {
                    cells[i].mine = true;
                    break;
                }
            }
            free_--;
            toLay--;
        }
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
            {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if ((dx || dy) && inside(x + dx, y + dy) && at(x + dx, y + dy).mine)
                            n++;
                at(x, y).around = (uint8_t)n;
            }
    }

    //  Opens a cell, and every cell around a zero with it.
    void open(int x, int y)
    {
        if (state == Won || state == Lost || !inside(x, y))
            return;
        if (state == Ready)
        {
            layMines(x, y);
            state = Playing;
            startedAt = r2::ticks();
        }
        Cell &c = at(x, y);
        if (c.open || c.flag)
            return;
        if (c.mine)
        {
            c.open = true;
            boom = y * gw + x;
            lose();
            return;
        }
        //  Flood through the zeros with an explicit stack: the board is at most
        //  480 cells, and each goes on it once.
        static int stack[MAX_W * MAX_H];
        int top = 0;
        c.open = true;
        opened++;
        stack[top++] = y * gw + x;
        while (top)
        {
            int i = stack[--top];
            if (cells[i].around)
                continue;
            int ix = i % gw, iy = i / gw;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = ix + dx, ny = iy + dy;
                    if (!inside(nx, ny))
                        continue;
                    Cell &n = at(nx, ny);
                    if (n.open || n.flag || n.mine)
                        continue;
                    n.open = true;
                    opened++;
                    stack[top++] = ny * gw + nx;
                }
        }
        if (opened == gw * gh - mines)
            win();
    }

    //  On an open number whose flags are all placed: open the rest round it.
    void chord(int x, int y)
    {
        Cell &c = at(x, y);
        if (!c.open || !c.around || state != Playing)
            return;
        int f = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (inside(x + dx, y + dy) && at(x + dx, y + dy).flag)
                    f++;
        if (f != c.around)
            return;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if ((dx || dy) && inside(x + dx, y + dy))
                    open(x + dx, y + dy);
    }

    void toggleFlag(int x, int y)
    {
        if (state == Won || state == Lost || !inside(x, y))
            return;
        Cell &c = at(x, y);
        if (c.open)
            return;
        c.flag = !c.flag;
        flags += c.flag ? 1 : -1;
    }

    //  What a click or Space does: open a closed cell, chord an open one.
    void act(int x, int y)
    {
        if (at(x, y).open)
            chord(x, y);
        else
            open(x, y);
    }

    void lose()
    {
        state = Lost;
        tick();
        r2::audio::beep(r2::audio::NOTE_C4, 80);
    }

    void win()
    {
        state = Won;
        tick();
        // Every mine left is flagged for the player: nothing else it could be.
        for (int i = 0; i < gw * gh; i++)
            if (cells[i].mine && !cells[i].flag)
            {
                cells[i].flag = true;
                flags++;
            }
        if (!best[lvl] || seconds < best[lvl])
        {
            best[lvl] = seconds;
            newBest = true;
            saveBest();
        }
        r2::audio::beep(r2::audio::NOTE_C5, 40);
        r2::audio::beep(r2::audio::NOTE_G4 + 200, 60);
    }

    //  The clock reads whole seconds, up to 999 as it always has.
    void tick()
    {
        if (state != Playing && state != Won && state != Lost)
            return;
        uint32_t s = (uint32_t)((r2::ticks() - startedAt) / 1000);
        if (s > 999)
            s = 999;
        seconds = s;
    }

    // ── Best times ───────────────────────────────────────────────────────────

    //  "12 85 0\n": one number of seconds per level, 0 for none.
    void loadBest()
    {
        auto text = r2::fs::read_text(BEST_PATH, 512);
        if (!text)
            return;
        const char *p = text->c_str();
        for (int i = 0; i < 3; i++)
        {
            while (*p == ' ')
                p++;
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (uint32_t)(*p++ - '0');
            best[i] = v;
        }
    }

    void saveBest()
    {
        char buf[48] = "";
        for (int i = 0; i < 3; i++)
        {
            if (i)
                append(buf, " ");
            appendU(buf, best[i]);
        }
        append(buf, "\n");
        (void)r2::fs::write_text(BEST_PATH, r2::string_view(buf, strlen(buf)));
    }

    // ── Events ───────────────────────────────────────────────────────────────

    //  Where the board is drawn: centred, whatever its size.
    int boardX() const { return (W - gw * CS) / 2; }
    int boardY() const { return TOP + 2 + (MAX_H - gh) * CS / 2; }
    int faceX() const { return W / 2 - 8; }

    void onClick(int mx, int my, bool right, bool pressed)
    {
        //  The face: pressed in on the way down, a new game on the way up.
        bool onFace = my >= 2 && my < 14 && mx >= faceX() && mx < faceX() + 16;
        if (!right && onFace)
        {
            pressFace = pressed;
            if (!pressed)
                newGame(lvl);
            wnd->Repaint();
            return;
        }
        if (pressFace && !pressed)
        {
            pressFace = false;
            wnd->Repaint();
        }
        if (!pressed)
            return;
        int x = (mx - boardX()) / CS, y = (my - boardY()) / CS; // in units of the layout
        if (mx < boardX() || my < boardY() || !inside(x, y))
            return;
        showCursor = false;
        cx = x;
        cy = y;
        if (right)
            toggleFlag(x, y);
        else
            act(x, y);
        wnd->Repaint();
    }

    void onKey(PlatformKey *key)
    {
        if (!key->isKeyDown)
            return;
        char c = key->isChar ? (char)key->theChar : 0;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (key->isEscape)
        {
            wnd->Close();
            return;
        }
        if (c >= '1' && c <= '3')
            return newGame(c - '1');
        if (c == 'n')
            return newGame(lvl);
        int dx = 0, dy = 0;
        if (key->isArrowLeft)
            dx = -1;
        else if (key->isArrowRight)
            dx = 1;
        else if (key->isArrowUp)
            dy = -1;
        else if (key->isArrowDown)
            dy = 1;
        if (dx || dy)
        {
            if (showCursor)
            {
                cx = (cx + dx + gw) % gw;
                cy = (cy + dy + gh) % gh;
            }
            showCursor = true;
        }
        else if (c == ' ' || key->isEnter)
        {
            showCursor = true;
            act(cx, cy);
        }
        else if (c == 'f')
        {
            showCursor = true;
            toggleFlag(cx, cy);
        }
        else
            return;
        wnd->Repaint();
    }

    void onIdle()
    {
        if (state == Playing)
        {
            uint32_t was = seconds;
            tick();
            if (seconds != was)
            {
                wnd->Repaint();
                return;
            }
        }
        r2::sleep(20); // nothing to do until the next second
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        switch (data->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop:
            onIdle();
            return;
        case PlatformWindowInputEventType::OnPaint:
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            onKey(data->Data.OnKeyEvent.key);
            return;
        case PlatformWindowInputEventType::OnMouseClick:
        {
            auto &m = data->Data.OnMouseClick;
            onClick((int)((COORD_VAL(m.mouseX) - ox) / z), (int)((COORD_VAL(m.mouseY) - oy) / z), m.button == PlatformWindowMouseButton::Right,
                    m.state == PlatformWindowButtonState::Pressed);
            return;
        }
        default:
            return;
        }
    }

    // ── Drawing ──────────────────────────────────────────────────────────────

    enum
    {
        BLACK = 0,
        BLUE = 1,
        GREEN = 2,
        CYAN = 3,
        RED = 4,
        MAGENTA = 5,
        BROWN = 6,
        LIGHTGREY = 7,
        DARKGREY = 8,
        LIGHTBLUE = 9,
        LIGHTRED = 12,
        YELLOW = 14,
        WHITE = 15
    };

    void makeResources(PlatformDrawingContext *dc)
    {
        if (font)
            return;
        static const uint32_t rgb[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                         0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                         0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
        for (int i = 0; i < 16; i++)
            col[i] = dc->CreateColor(0xFF000000u | rgb[i], nullptr, nullptr);
        font = fonts[1] = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
    }

    //  The font for the zoom: glyphs at the whole multiple of their size
    //  nearest to it (every box the text goes in has room for half a step
    //  more).  CreateFont takes pixels that it turns into points
    //  by the DPI (px * 100 / 75 * dpi / 96), and the glyph is 12 points
    //  tall a step, so ask for the middle of the step wanted.
    void pickFont(PlatformDrawingContext *dc)
    {
        int s = (int)(z + 0.5);
        if (s < 1)
            s = 1;
        if (s > 7)
            s = 7;
        if (!fonts[s])
        {
            int dpi = wnd->GetEffectiveDPI() > 0 ? wnd->GetEffectiveDPI() : 192;
            fonts[s] = dc->CreateFont(Coord((12.0 * s + 6) * 72 / dpi), nullptr, false, false, false, nullptr, nullptr);
        }
        font = fonts[s] ? fonts[s] : fonts[1];
    }

    //  The largest zoom the window takes, in steps of whole pixels so that
    //  every edge of the layout lands on one.
    void zoomTo(double winW, double winH)
    {
        int px = wnd->GetEffectiveDPI() > 0 ? (wnd->GetEffectiveDPI() + 48) / 96 : 2;
        if (px < 1)
            px = 1;
        double fit = winW / W < winH / H ? winW / W : winH / H;
        z = (int)(fit * px) / (double)px;
        if (z < 1)
            z = 1;
        ox = (int)((winW - W * z) / 2 * px) / (double)px;
        oy = (int)((winH - H * z) / 2 * px) / (double)px;
        if (ox < 0)
            ox = 0;
        if (oy < 0)
            oy = 0;
    }

    static void append(char *out, const char *s)
    {
        size_t at = strlen(out);
        while (*s)
            out[at++] = *s++;
        out[at] = 0;
    }

    static void appendU(char *out, uint32_t v)
    {
        char tmp[12];
        int k = 0;
        do
        {
            tmp[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        size_t at = strlen(out);
        while (k)
            out[at++] = tmp[--k];
        out[at] = 0;
    }

    void rect(PlatformBitmap *t, int x, int y, int w, int h, int c)
    {
        t->FillRect(Coord(ox + x * z), Coord(oy + y * z), Coord(w * z), Coord(h * z), col[c], false);
    }

    void text(PlatformBitmap *t, int x, int y, int w, int h, const char *s, int c, PlatformAlign align)
    {
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = col[c];
        o.horizontalAlign = align;
        o.verticalAlign = PlatformAlign::Middle;
        t->DrawText(Coord(ox + x * z), Coord(oy + y * z), Coord(w * z), Coord(h * z), (const mchar *)s, &o, false);
    }

    //  A raised square: light on the top and left, dark on the bottom and
    //  right --- a key that has not been pressed.
    void raised(PlatformBitmap *t, int x, int y, int w, int h)
    {
        rect(t, x, y, w, h, LIGHTGREY);
        rect(t, x, y, w, 1, WHITE);
        rect(t, x, y, 1, h, WHITE);
        rect(t, x, y + h - 1, w, 1, DARKGREY);
        rect(t, x + w - 1, y, 1, h, DARKGREY);
    }

    void drawFlag(PlatformBitmap *t, int x, int y)
    {
        rect(t, x + 4, y + 2, 1, 5, BLACK); // the pole
        rect(t, x + 2, y + 2, 2, 1, RED);   // the flag, a small pennant
        rect(t, x + 1, y + 3, 3, 1, RED);
        rect(t, x + 2, y + 4, 2, 1, RED);
        rect(t, x + 2, y + 6, 5, 1, BLACK); // its foot
    }

    void drawMine(PlatformBitmap *t, int x, int y)
    {
        rect(t, x + 2, y + 2, 4, 4, BLACK);
        rect(t, x + 1, y + 3, 6, 2, BLACK);
        rect(t, x + 3, y + 1, 2, 6, BLACK);
        rect(t, x + 3, y + 3, 1, 1, WHITE); // the glint
    }

    void drawCell(PlatformBitmap *t, int i, int x, int y)
    {
        const Cell &c = cells[i];
        bool over = state == Won || state == Lost;
        if (!c.open && !(over && state == Lost && c.mine && !c.flag))
        {
            raised(t, x, y, CS, CS);
            if (c.flag)
            {
                drawFlag(t, x, y);
                // A flag on a cell that had no mine, once the game is lost.
                if (state == Lost && !c.mine)
                {
                    rect(t, x + 1, y + 1, CS - 2, 1, RED);
                    rect(t, x + 1, y + CS - 2, CS - 2, 1, RED);
                }
            }
            return;
        }
        // Open: flat, with a line on the top and left against its neighbours.
        rect(t, x, y, CS, CS, i == boom ? LIGHTRED : LIGHTGREY);
        rect(t, x, y, CS, 1, DARKGREY);
        rect(t, x, y, 1, CS, DARKGREY);
        if (c.mine)
        {
            drawMine(t, x, y);
            return;
        }
        if (c.around)
        {
            static const int ink[9] = {0, BLUE, GREEN, RED, LIGHTBLUE, BROWN, CYAN, BLACK, DARKGREY};
            char s[2] = {(char)('0' + c.around), 0};
            text(t, x, y, CS + 1, CS, s, ink[c.around], PlatformAlign::Middle);
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *t)
    {
        if (!t)
            return;
        makeResources(dc);
        if (!font || !col[WHITE])
            return;
        Coord Wc = t->GetWidth(), Hc = t->GetHeight();
        zoomTo(COORD_VAL(Wc), COORD_VAL(Hc));
        pickFont(dc);
        t->FillRect(0, 0, Wc, Hc, col[LIGHTGREY], false);

        // The top: mines left, the face, the clock.
        char left[12] = "";
        int remain = mines - flags;
        if (remain < 0)
        {
            append(left, "-");
            remain = -remain;
        }
        appendU(left, (uint32_t)remain);
        rect(t, 4, 2, 28, 12, BLACK);
        text(t, 4, 2, 28, 12, left, LIGHTRED, PlatformAlign::Middle);

        char clock[12] = "";
        appendU(clock, seconds);
        rect(t, W - 32, 2, 28, 12, BLACK);
        text(t, W - 32, 2, 28, 12, clock, LIGHTRED, PlatformAlign::Middle);

        const char *face = state == Lost ? "x(" : state == Won ? "B)" : ":)";
        int fx = faceX();
        if (pressFace)
        {
            rect(t, fx, 2, 16, 12, DARKGREY);
            rect(t, fx + 1, 3, 15, 11, LIGHTGREY);
        }
        else
            raised(t, fx, 2, 16, 12);
        rect(t, fx + 3, 4, 10, 8, YELLOW);
        text(t, fx + 3, 4, 11, 8, face, BLACK, PlatformAlign::Middle);

        // The level and its best time, either side of the face.
        text(t, 36, 2, fx - 40, 12, level(lvl).name, BLACK, PlatformAlign::Begin);
        char b[24] = "best ";
        if (best[lvl])
            appendU(b, best[lvl]);
        else
            append(b, "-");
        text(t, fx + 20, 2, W - 36 - fx - 24, 12, b, newBest ? RED : BLACK, PlatformAlign::End);

        // The board, sunk into the window.
        int bx = boardX(), by = boardY();
        rect(t, bx - 1, by - 1, gw * CS + 2, gh * CS + 2, DARKGREY);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                drawCell(t, y * gw + x, bx + x * CS, by + y * CS);

        if (showCursor && state != Won && state != Lost)
        {
            int x = bx + cx * CS, y = by + cy * CS;
            rect(t, x, y, CS, 1, BLUE);
            rect(t, x, y + CS - 1, CS, 1, BLUE);
            rect(t, x, y, 1, CS, BLUE);
            rect(t, x + CS - 1, y, 1, CS, BLUE);
        }

        // What the keys do, or how it ended.
        const char *foot = "right click/F flag   N new   1 2 3 level   Esc close";
        char msg[64] = "";
        if (state == Won)
        {
            append(msg, "Cleared in ");
            appendU(msg, seconds);
            append(msg, newBest ? " s - a new best!   N plays again" : " s   N plays again");
            foot = msg;
        }
        else if (state == Lost)
            foot = "Boom.   N or the face plays again";
        text(t, 4, H - FOOT, W - 8, FOOT - 1, foot, state == Lost ? RED : BLACK, PlatformAlign::Middle);
    }
};
