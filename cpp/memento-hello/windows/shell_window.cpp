//
// Window — Shell: r2sh in a window
//
// The shell is not in this process.  It is c/r2sh (sh.elf in /mnt/iso/bin),
// started with --host and the address of a block this window allocates on the
// user heap: it writes what it says into one ring of the block and reads what
// is typed out of another, and this window is the terminal between them ---
// 80 columns, the last 200 lines kept, PageUp and PageDown to look back.  The
// layout has to match c/r2sh/host.h.
//
// Programs the shell runs are processes of their own and print to the console,
// not here; the shell says so when it starts one.
//
// Closing the window, or `exit` in the shell, ends both.  If Memento stops
// beating, the shell notices within ten seconds and leaves on its own.
//

// ── The shared block (c/r2sh/host.h) ────────────────────────────────────────

struct ShHostBlock
{
    static const uint32_t Magic = 0x42535232; // "2RSB"
    static const uint32_t Version = 1;
    static const uint32_t OutSize = 8192;
    static const uint32_t InSize = 128;

    uint32_t magic;
    uint32_t version;
    volatile uint32_t outHead;
    volatile uint32_t inTail;
    volatile uint32_t shellBeat;
    volatile uint8_t exited;
    uint8_t pad0[3];
    uint8_t out[OutSize];
    volatile uint32_t outTail;
    volatile uint32_t inHead;
    volatile uint32_t hostBeat;
    volatile uint8_t quit;
    uint8_t pad1[3];
    uint8_t in[InSize];
};

//  Every live block, kept beating while main() has handed the screen to a
//  full-screen program and this window's loop is not running.
static const int SH_MAX = 4;
static ShHostBlock *g_shBlocks[SH_MAX];

static void shellKeepAlive()
{
    for (ShHostBlock *b : g_shBlocks)
        if (b)
            b->hostBeat = b->hostBeat + 1;
}

class ShellWindow
{
public:
    ShellWindow()
    {
        error[0] = 0;
        clearScreen();
        int slot = -1;
        for (int i = 0; i < SH_MAX && slot < 0; i++)
            if (!g_shBlocks[i])
                slot = i;
        if (slot < 0)
        {
            strcpy(error, "Too many shells open.");
            return;
        }
        blk = (ShHostBlock *)r2::heap::kernel_allocate(sizeof(ShHostBlock)); // zeroed
        if (!blk)
        {
            strcpy(error, "No memory for the shell.");
            return;
        }
        blk->magic = ShHostBlock::Magic;
        blk->version = ShHostBlock::Version;
        blk->hostBeat = 1;
        g_shBlocks[slot] = blk;

        char args[64] = "sh.elf --host 0x";
        size_t at = strlen(args);
        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at] = 0;
        r2::optional<uint8_t> id = r2::spawn("sh.elf", args);
        if (!id)
        {
            strcpy(error, "Could not start sh.elf (is it in /mnt/iso/bin?)");
            release();
            return;
        }
        pid = *id;
        startedAt = r2::ticks();
    }

    ~ShellWindow()
    {
        if (!blk)
            return;
        if (!blk->exited && shellAlive())
        {
            blk->quit = 1;
            for (int i = 0; i < 50 && !blk->exited && shellAlive(); i++)
            {
                blk->hostBeat = blk->hostBeat + 1;
                r2::sleep(20);
            }
        }
        if (blk->exited || !shellAlive())
            release();
        else
            forget(); // still running: leave the block to it
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<ShellWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        //  The loop turns while the window is open: that is what drains the
        //  shell's output and keeps it beating.
        wnd->SetImmediateMode(true);
    }

    bool failed() const { return !blk; }
    const char *why() const { return error; }

    static const int Cols = 80;
    static const int Rows = 25;
    static const int W = Cols * 3 + 4;
    static const int H = Rows * 6 + 4;

private:
    static const int Keep = 200; // lines of scrollback, the screen's included

    PlatformWindow *wnd = nullptr;
    ShHostBlock *blk = nullptr;
    uint8_t pid = 0;
    uint64_t startedAt = 0;
    uint64_t lastCheck = 0;
    char error[64];

    //  The terminal: line n lives at lines[n % Keep]; `last` is the line the
    //  cursor is on, `first` the oldest one still kept.
    char lines[Keep][Cols];
    long first = 0, last = 0;
    int col = 0;
    int back = 0; // lines scrolled back from the bottom

    PlatformColor *bg = nullptr, *fg = nullptr, *dim = nullptr, *cursor = nullptr;
    PlatformFont *font = nullptr;
    double cw = 3, ch = 6;

    void forget()
    {
        for (ShHostBlock *&b : g_shBlocks)
            if (b == blk)
                b = nullptr;
        blk = nullptr;
    }

    void release()
    {
        ShHostBlock *b = blk;
        forget();
        r2::heap::kernel_deallocate(b);
    }

    //  The shell's task is still in the table ("SH      .ELF").  A table
    //  that cannot be read just now counts as alive.
    bool shellAlive()
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (tasks.empty())
            return true;
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid && tasks[i].status < 4 && tasks[i].name[0] == 'S' && tasks[i].name[1] == 'H')
                return true;
        return false;
    }

    // ── The terminal ─────────────────────────────────────────────────────────

    char *row(long n) { return lines[n % Keep]; }

    void clearScreen()
    {
        first = last = 0;
        col = 0;
        back = 0;
        memset(lines[0], ' ', Cols);
    }

    void newLine()
    {
        last++;
        memset(row(last), ' ', Cols);
        if (last - first >= Keep)
            first = last - Keep + 1;
        col = 0;
    }

    void put(uint8_t c)
    {
        switch (c)
        {
        case '\n': newLine(); return;
        case '\r': col = 0; return;
        case '\b':
            if (col > 0)
                col--;
            return;
        case '\f': clearScreen(); return;
        case '\t':
            do
                put(' ');
            while (col % 8);
            return;
        default:
            break;
        }
        if (c < 0x20)
            return;
        if (col >= Cols)
            newLine();
        row(last)[col++] = (char)c;
    }

    //  What the shell has written since the last turn.
    bool drain()
    {
        uint32_t tail = blk->outTail, head = blk->outHead;
        if (tail == head)
            return false;
        //  At most one ring's worth: a head that ran further is not to be
        //  believed.
        if (head - tail > ShHostBlock::OutSize)
            tail = head - ShHostBlock::OutSize;
        for (; tail != head; tail++)
            put(blk->out[tail % ShHostBlock::OutSize]);
        asm volatile("" ::: "memory");
        blk->outTail = tail;
        back = 0; // new output brings the view back to the bottom
        return true;
    }

    void send(uint8_t c)
    {
        if (!blk || blk->exited)
            return;
        uint32_t head = blk->inHead;
        if (head - blk->inTail >= ShHostBlock::InSize)
            return; // the shell is not keeping up; drop rather than overwrite
        blk->in[head % ShHostBlock::InSize] = c;
        asm volatile("" ::: "memory");
        blk->inHead = head + 1;
    }

    void scroll(int lines_)
    {
        long kept = last - first + 1;
        long most = kept > Rows ? kept - Rows : 0;
        long b = back + lines_;
        if (b < 0)
            b = 0;
        if (b > most)
            b = most;
        if (b != back)
        {
            back = (int)b;
            wnd->Repaint();
        }
    }

    void onKey(PlatformKey *key)
    {
        if (!key->isKeyDown)
            return;
        if (key->isPageUp)
            return scroll(Rows - 1);
        if (key->isPageDown)
            return scroll(-(Rows - 1));
        if (key->isEnter)
            send('\n');
        else if (key->isBackspace)
            send('\b');
        else if (key->isEscape)
            send(0x1b);
        else if (key->isTab)
            send(' ');
        else if (key->isChar && key->theChar >= 0x20 && key->theChar < 0x7f)
            send((uint8_t)key->theChar);
    }

    void onIdle()
    {
        if (!blk)
            return;
        blk->hostBeat = blk->hostBeat + 1;
        if (drain())
            wnd->Repaint();
        uint64_t now = r2::ticks();
        if (now - lastCheck < 1000 || now - startedAt < 2000)
            return;
        lastCheck = now;
        if (blk->exited || !shellAlive())
        {
            drain(); // its goodbye
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
        default:
            return;
        }
    }

    // ── Drawing ──────────────────────────────────────────────────────────────

    void makeResources(PlatformDrawingContext *dc)
    {
        if (font)
            return;
        bg = dc->CreateColor(0xFF000000, nullptr, nullptr);
        fg = dc->CreateColor(0xFFAAAAAA, nullptr, nullptr);
        dim = dc->CreateColor(0xFF555555, nullptr, nullptr);
        cursor = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
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

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        makeResources(dc);
        if (!font || !bg)
            return;
        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        double W_ = COORD_VAL(Wc), H_ = COORD_VAL(Hc);
        target->FillRect(0, 0, Coord(W_), Coord(H_), bg, false);

        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = fg;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Begin;

        if (!blk)
        {
            o.foreground = cursor;
            target->DrawText(4, 4, Coord(W_ - 8), 10, (const mchar *)error, &o, false);
            return;
        }

        const double ox = (W_ - Cols * cw) / 2, oy = (H_ - Rows * ch) / 2;
        //  The bottom of the view is the cursor's line, less what is scrolled
        //  back; the top follows from it.
        long bottom = last - back;
        long top = bottom - (Rows - 1);
        if (top < first)
            top = first;
        char text[Cols + 1];
        for (long n = top; n <= bottom && n <= last; n++)
        {
            const char *r = row(n);
            int len = Cols;
            while (len > 0 && r[len - 1] == ' ')
                len--;
            if (!len)
                continue;
            for (int k = 0; k < len; k++)
            {
                uint8_t c = (uint8_t)r[k];
                text[k] = c < 0x20 ? ' ' : (char)c;
            }
            text[len] = 0;
            double y = oy + (n - top) * ch;
            target->DrawText(Coord(ox), Coord(y), Coord(len * cw + cw), Coord(ch), (const mchar *)text, &o, false);
        }

        if (back == 0)
        {
            //  The cursor: a bar under the cell it is on.
            double x = ox + (col < Cols ? col : Cols - 1) * cw;
            double y = oy + (last - top) * ch + ch - 1;
            target->FillRect(Coord(x), Coord(y), Coord(cw), 1, cursor, false);
        }
        else
        {
            //  Scrolled back: say how far, in the top right corner.
            char note[24] = "-";
            char num[12];
            int n = 0;
            long v = back;
            do
            {
                num[n++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            int at = 1;
            while (n)
                note[at++] = num[--n];
            strcpy(note + at, " lines");
            o.foreground = dim;
            o.horizontalAlign = PlatformAlign::End;
            target->DrawText(0, Coord(oy), Coord(W_ - 2), Coord(ch), (const mchar *)note, &o, false);
        }
    }
};
