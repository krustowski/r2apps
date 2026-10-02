//
// Window — Editor: Turbo C++ 23 in a window
//
// The editor is not in this process.  It is ~/vxn/tcpp/r2 (tcpp.elf, which
// r2_main's build_iso puts in /mnt/iso/bin), started with --host and the
// address of a block of memory this window allocates: it draws its 80x25
// screen into the block and takes its keys out of it, and this window draws
// the one and fills the other.  So the editor keeps its own 2 MiB, its dialogs
// keep their own loops, and Memento goes on with its other windows while it
// runs --- Alt+Tab, or the taskbar, to go between them.
//
// The block is on r2's user heap (0xC00_000 up), which every process's page
// tables map the same way; see host.hpp in the tcpp sources, which this layout
// has to match.
//
// Closing: the close box asks the editor to leave the way Alt+X does, so a
// file that has not been saved gets its question; the window goes when the
// editor has gone.  If Memento stops beating (it died, or it is taking the
// window down without asking), the editor notices within ten seconds and
// leaves on its own.
//

// ── The shared block (tcpp/r2/src/host.hpp) ─────────────────────────────────

struct EdHostKey
{
    uint8_t key;
    char ch;
    uint8_t mods;
    uint8_t pad;
};

struct EdHostBlock
{
    static const uint32_t Magic = 0x42484354; // "TCHB"
    static const uint32_t Version = 1;
    static const int Cols = 80;
    static const int Rows = 25;
    static const int KeySlots = 64;

    uint32_t magic;
    uint32_t version;
    volatile uint32_t frame;
    volatile uint32_t editorBeat;
    volatile uint32_t keyTail;
    volatile uint8_t exited;
    uint8_t pad0[3];
    uint8_t cells[Cols * Rows];
    uint8_t attrs[Cols * Rows];
    volatile uint32_t hostBeat;
    volatile uint32_t keyHead;
    volatile uint8_t quit;
    uint8_t pad1[3];
    EdHostKey keys[KeySlots];
};

//  tc::Key, by number.
enum EdKey : uint8_t
{
    ED_NONE = 0,
    ED_CHAR,
    ED_ENTER,
    ED_ESCAPE,
    ED_BACKSPACE,
    ED_TAB,
    ED_UP,
    ED_DOWN,
    ED_LEFT,
    ED_RIGHT,
    ED_HOME,
    ED_END,
    ED_PAGEUP,
    ED_PAGEDOWN,
    ED_INSERT,
    ED_DELETE,
    ED_F1, // F2..F12 follow
};

static const uint8_t ED_CTRL = 1, ED_ALT = 2, ED_SHIFT = 4;

//  Every live block, so that main() can keep them beating while it has handed
//  the screen to a full-screen program and this window's loop is not running.
static const int ED_MAX = 8;
static EdHostBlock *g_edBlocks[ED_MAX];

static void editorKeepAlive()
{
    for (EdHostBlock *b : g_edBlocks)
        if (b)
            b->hostBeat = b->hostBeat + 1;
}

// ── The frames tcpp draws with ──────────────────────────────────────────────
//
// Code page 437's line characters, as four arms (up, down, left, right), each
// none, one line or two: the table tcpp builds its own glyphs from
// (Screen::drawLineGlyphs).  The kernel font this window draws text with has
// none of them, so they are drawn here as lines.

struct EdLine
{
    uint8_t code, up, down, left, right;
};

static const EdLine kEdLines[] = {
    {0xB3, 1, 1, 0, 0}, {0xC4, 0, 0, 1, 1}, {0xDA, 0, 1, 0, 1}, {0xBF, 0, 1, 1, 0}, {0xC0, 1, 0, 0, 1},
    {0xD9, 1, 0, 1, 0}, {0xC3, 1, 1, 0, 1}, {0xB4, 1, 1, 1, 0}, {0xC2, 0, 1, 1, 1}, {0xC1, 1, 0, 1, 1},
    {0xC5, 1, 1, 1, 1}, {0xBA, 2, 2, 0, 0}, {0xCD, 0, 0, 2, 2}, {0xC9, 0, 2, 0, 2}, {0xBB, 0, 2, 2, 0},
    {0xC8, 2, 0, 0, 2}, {0xBC, 2, 0, 2, 0}, {0xCC, 2, 2, 0, 2}, {0xB9, 2, 2, 2, 0}, {0xCB, 0, 2, 2, 2},
    {0xCA, 2, 0, 2, 2}, {0xCE, 2, 2, 2, 2}, {0xD5, 0, 1, 0, 2}, {0xB8, 0, 1, 2, 0}, {0xD4, 1, 0, 0, 2},
    {0xBE, 1, 0, 2, 0}, {0xD6, 0, 2, 0, 1}, {0xB7, 0, 2, 1, 0}, {0xD3, 2, 0, 0, 1}, {0xBD, 2, 0, 1, 0},
    {0xC6, 1, 1, 0, 2}, {0xB5, 1, 1, 2, 0}, {0xC7, 2, 2, 0, 1}, {0xB6, 2, 2, 1, 0}, {0xD1, 0, 1, 2, 2},
    {0xCF, 1, 0, 2, 2}, {0xD2, 0, 2, 1, 1}, {0xD0, 2, 0, 1, 1}, {0xD8, 1, 1, 2, 2}, {0xD7, 2, 2, 1, 1},
};

class EditorWindow
{
public:
    //  path: the file to open, or null for an empty one.
    explicit EditorWindow(const char *path)
    {
        error[0] = 0;
        int slot = -1;
        for (int i = 0; i < ED_MAX && slot < 0; i++)
            if (!g_edBlocks[i])
                slot = i;
        if (slot < 0)
        {
            strcpy(error, "Too many editors open.");
            return;
        }
        //  The kernel heap hands blocks back zeroed.
        blk = (EdHostBlock *)r2::heap::kernel_allocate(sizeof(EdHostBlock));
        if (!blk)
        {
            strcpy(error, "No memory for the editor.");
            return;
        }
        blk->magic = EdHostBlock::Magic;
        blk->version = EdHostBlock::Version;
        blk->hostBeat = 1;
        g_edBlocks[slot] = blk;

        //  "tcpp.elf --host 0xC1A2B0 /mnt/fat/FILE.CPP"
        char args[128] = "tcpp.elf --host 0x";
        size_t at = strlen(args);
        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at] = 0;
        if (path && path[0])
        {
            if (at + 1 + strlen(path) + 1 > sizeof(args) || strchr(path, ' '))
            {
                strcpy(error, "That path cannot be given to the editor.");
                release();
                return;
            }
            args[at++] = ' ';
            strcpy(args + at, path);
        }
        r2::optional<uint8_t> id = r2::spawn("tcpp.elf", args);
        if (!id)
        {
            strcpy(error, "Could not start tcpp.elf (is it in /mnt/iso/bin?)");
            release();
            return;
        }
        pid = *id;
        startedAt = r2::ticks();
    }

    ~EditorWindow()
    {
        if (!blk)
            return;
        if (!blk->exited && editorAlive())
        {
            //  Taken down without asking (the session is ending): the editor
            //  leaves at once, and the block stays until it has.
            blk->quit = 1;
            for (int i = 0; i < 100 && !blk->exited && editorAlive(); i++)
            {
                blk->hostBeat = blk->hostBeat + 1;
                r2::sleep(20);
            }
        }
        if (blk->exited || !editorAlive())
            release();
        else
            forget(); // still running: leave the memory to it rather than pull it away
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<EditorWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        //  The loop turns while the window is open: that is what watches for
        //  new frames and keeps the editor's host beating.
        wnd->SetImmediateMode(true);
        //  Not maximizable, so Alt+F goes on to Turbo C++, whose File menu
        //  it opens, rather than to the window manager.
        wnd->SetCapabilities(true, false, false, true, true);
    }

    bool failed() const { return !blk; }
    const char *why() const { return error; }

    //  The size the window asks for: the grid in cells of the small font.
    static const int W = 80 * 3 + 4;
    static const int H = 25 * 6 + 4;

private:
    PlatformWindow *wnd = nullptr;
    EdHostBlock *blk = nullptr;
    uint8_t pid = 0;
    uint64_t startedAt = 0;
    uint64_t lastCheck = 0;
    uint32_t shownFrame = 0;
    bool leaving = false; // the editor has gone; the next close is allowed
    char error[64];

    PlatformColor *pal[16] = {};
    PlatformColor *shade[3][16] = {}; // a colour over another, 25/50/75 %
    PlatformFont *font = nullptr;
    double cw = 3, ch = 6;

    void forget()
    {
        for (EdHostBlock *&b : g_edBlocks)
            if (b == blk)
                b = nullptr;
        blk = nullptr;
    }

    void release()
    {
        EdHostBlock *b = blk;
        forget();
        r2::heap::kernel_deallocate(b);
    }

    //  The editor's task is still in the table.  A table that cannot be read
    //  just now counts as alive.
    bool editorAlive()
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (tasks.empty())
            return true;
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid && tasks[i].status < 4 && tasks[i].name[0] == 'T' && tasks[i].name[1] == 'C')
                return true;
        return false;
    }

    void send(uint8_t key, char c, uint8_t mods)
    {
        if (!blk || blk->exited)
            return;
        uint32_t head = blk->keyHead;
        if (head - blk->keyTail >= (uint32_t)EdHostBlock::KeySlots)
            return; // the editor is not keeping up; drop rather than overwrite
        EdHostKey &k = blk->keys[head % EdHostBlock::KeySlots];
        k.key = key;
        k.ch = c;
        k.mods = mods;
        asm volatile("" ::: "memory");
        blk->keyHead = head + 1;
    }

    void onKey(PlatformKey *key)
    {
        if (!key->isKeyDown)
            return;
        uint8_t mods = 0;
        if (key->isLeftControl || key->isRightControl)
            mods |= ED_CTRL;
        if (key->isLeftAlt || key->isRightAlt)
            mods |= ED_ALT;
        if (key->isLeftShift || key->isRightShift)
            mods |= ED_SHIFT;
        uint8_t k = ED_NONE;
        char c = 0;
        if (key->isEscape)
            k = ED_ESCAPE;
        else if (key->isEnter)
            k = ED_ENTER;
        else if (key->isBackspace)
            k = ED_BACKSPACE;
        else if (key->isTab)
            k = ED_TAB;
        else if (key->isArrowUp)
            k = ED_UP;
        else if (key->isArrowDown)
            k = ED_DOWN;
        else if (key->isArrowLeft)
            k = ED_LEFT;
        else if (key->isArrowRight)
            k = ED_RIGHT;
        else if (key->isHome)
            k = ED_HOME;
        else if (key->isEnd)
            k = ED_END;
        else if (key->isPageUp)
            k = ED_PAGEUP;
        else if (key->isPageDown)
            k = ED_PAGEDOWN;
        else if (key->isInsert)
            k = ED_INSERT;
        else if (key->isDelete)
            k = ED_DELETE;
        else if (key->isF && key->f >= 1 && key->f <= 12)
            k = (uint8_t)(ED_F1 + key->f - 1);
        else if (key->isChar)
        {
            k = ED_CHAR;
            c = (char)key->theChar;
        }
        if (k != ED_NONE) // the modifiers on their own are not keys
            send(k, c, mods);
    }

    void onIdle()
    {
        if (!blk)
            return;
        blk->hostBeat = blk->hostBeat + 1;
        if (blk->frame != shownFrame)
        {
            shownFrame = blk->frame;
            wnd->Repaint();
        }
        //  Once a second: has the editor gone?  It says so when it leaves
        //  properly; a crash only shows in the task table.
        uint64_t now = r2::ticks();
        if (now - lastCheck < 1000 || now - startedAt < 2000)
            return;
        lastCheck = now;
        if (blk->exited || !editorAlive())
        {
            leaving = true;
            wnd->SetImmediateMode(false);
            wnd->Close();
        }
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
        case PlatformWindowInputEventType::OnClose:
        {
            //  The close box: the editor is asked to leave, as Alt+X would, so
            //  an unsaved file gets its question.  The window follows it.
            PlatformWindowInterfaceInputEventBase *base = data;
            if (blk && !leaving && !blk->exited && base->Data.OnClose.canBePrevented)
            {
                base->Data.OnClose.preventClosing = true;
                send(ED_CHAR, 'x', ED_ALT);
            }
            return;
        }
        default:
            return;
        }
    }

    // ── Drawing ───────────────────────────────────────────────────────────────

    void makeResources(PlatformDrawingContext *dc)
    {
        if (font)
            return;
        static const uint8_t rgb[16][3] = {
            {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
            {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
            {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
            {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
        };
        static const uint32_t alpha[3] = {0x30, 0x80, 0xC0};
        for (int i = 0; i < 16; i++)
        {
            uint32_t c = ((uint32_t)rgb[i][0] << 16) | ((uint32_t)rgb[i][1] << 8) | rgb[i][2];
            pal[i] = dc->CreateColor(0xFF000000u | c, nullptr, nullptr);
            for (int s = 0; s < 3; s++)
                shade[s][i] = dc->CreateColor((alpha[s] << 24) | c, nullptr, nullptr);
        }
        font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (font)
        {
            Coord w, h;
            if (font->GetDrawnTextSize("MMMMMMMMMM", w, h))
            {
                cw = COORD_VAL(w) / 10;
                ch = COORD_VAL(h);
            }
        }
        //  The r2 font can answer a measurement with nothing; its cell is 6x12
        //  pixels, which at this window's DPI is 3x6.
        if (!(cw > 0))
            cw = 3;
        if (!(ch > 0))
            ch = 6;
    }

    static const EdLine *lineFor(uint8_t c)
    {
        if (c < 0xB3 || c > 0xDA)
            return nullptr;
        for (const EdLine &l : kEdLines)
            if (l.code == c)
                return &l;
        return nullptr;
    }

    //  A character this window draws itself rather than with the font.
    static bool drawn(uint8_t c)
    {
        return lineFor(c) || (c >= 0xB0 && c <= 0xB2) || (c >= 0xDB && c <= 0xDF) || c == 0xFE || c == 0x07 ||
               c == 0x10 || c == 0x11;
    }

    void rect(PlatformBitmap *t, double x, double y, double w, double h, PlatformColor *c)
    {
        t->FillRect(Coord(x), Coord(y), Coord(w), Coord(h), c, false);
    }

    void drawSpecial(PlatformBitmap *t, double x, double y, uint8_t c, PlatformColor *fg)
    {
        if (const EdLine *l = lineFor(c))
        {
            //  One line runs through the middle of the cell; two run a pixel
            //  either side of it.  A unit here is two pixels.
            const double th = 0.5;
            double mx = x + cw / 2 - th / 2, my = y + ch / 2 - th / 2;
            int vw = l->up > l->down ? l->up : l->down;
            int hw = l->left > l->right ? l->left : l->right;
            double xs[2] = {vw == 2 ? mx - 1 : mx, mx + 1};
            double ys[2] = {hw == 2 ? my - 1 : my, my + 1};
            for (int k = 0; k < vw; k++)
            {
                if (l->up)
                    rect(t, xs[k], y, th, (hw ? ys[hw - 1] : my) - y + th, fg);
                if (l->down)
                {
                    double from = hw ? ys[0] : my;
                    rect(t, xs[k], from, th, y + ch - from, fg);
                }
            }
            for (int k = 0; k < hw; k++)
            {
                if (l->left)
                    rect(t, x, ys[k], (vw ? xs[vw - 1] : mx) - x + th, th, fg);
                if (l->right)
                {
                    double from = vw ? xs[0] : mx;
                    rect(t, from, ys[k], x + cw - from, th, fg);
                }
            }
            return;
        }
        switch (c)
        {
        case 0xDB: rect(t, x, y, cw, ch, fg); return;
        case 0xDC: rect(t, x, y + ch / 2, cw, ch / 2, fg); return;
        case 0xDD: rect(t, x, y, cw / 2, ch, fg); return;
        case 0xDE: rect(t, x + cw / 2, y, cw / 2, ch, fg); return;
        case 0xDF: rect(t, x, y, cw, ch / 2, fg); return;
        case 0xFE: rect(t, x + 0.5, y + ch / 2 - 1.5, cw - 1, 3, fg); return;
        case 0x07: rect(t, x + cw / 2 - 0.5, y + ch / 2 - 0.5, 1, 1, fg); return;
        default: return;
        }
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        makeResources(dc);
        if (!font)
            return;
        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        double W = COORD_VAL(Wc), H = COORD_VAL(Hc);
        rect(target, 0, 0, W, H, pal[0]);

        PlatformDrawTextOptions o{};
        o.font = font;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Begin;

        if (!blk)
        {
            o.foreground = pal[15];
            target->DrawText(4, 4, Coord(W - 8), 10, (const mchar *)error, &o, false);
            return;
        }

        const double ox = (W - 80 * cw) / 2, oy = (H - 25 * ch) / 2;
        char run[81];
        for (int r = 0; r < EdHostBlock::Rows; r++)
        {
            const uint8_t *cells = blk->cells + r * EdHostBlock::Cols;
            const uint8_t *attrs = blk->attrs + r * EdHostBlock::Cols;
            double y = oy + r * ch;
            int c = 0;
            while (c < EdHostBlock::Cols)
            {
                //  A run of cells with one colour, drawn as one background
                //  and one line of text; frames and shades on top, one by one.
                uint8_t a = attrs[c];
                int e = c;
                while (e < EdHostBlock::Cols && attrs[e] == a)
                    e++;
                PlatformColor *fg = pal[a & 15], *bg = pal[(a >> 4) & 15];
                double x = ox + c * cw;
                rect(target, x, y, (e - c) * cw, ch, bg);

                int n = 0;
                bool any = false;
                for (int k = c; k < e; k++)
                {
                    uint8_t g = cells[k];
                    bool special = drawn(g);
                    run[n++] = special || g < 0x20 && g != 0x18 && g != 0x19 && g != 0x1A && g != 0x1B ? ' ' : (char)g;
                    any = any || (!special && g > ' ');
                }
                run[n] = 0;
                if (any)
                {
                    o.foreground = fg;
                    target->DrawText(Coord(x), Coord(y), Coord(n * cw + cw), Coord(ch), (const mchar *)run, &o, false);
                }
                for (int k = c; k < e; k++)
                {
                    uint8_t g = cells[k];
                    double cx = ox + k * cw;
                    if (g >= 0xB0 && g <= 0xB2)
                        rect(target, cx, y, cw, ch, shade[g - 0xB0][a & 15]);
                    else if (g == 0x10 || g == 0x11)
                    {
                        o.foreground = fg;
                        target->DrawText(Coord(cx), Coord(y), Coord(cw * 2), Coord(ch), g == 0x10 ? ">" : "<", &o,
                                         false);
                    }
                    else if (drawn(g))
                        drawSpecial(target, cx, y, g, fg);
                }
                c = e;
            }
        }
    }
};
