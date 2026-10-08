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
    //  With <script>, the shell runs it first (r2sh --run, bsh's `bsh`) and
    //  stays for more.
    explicit ShellWindow(const char *script = nullptr)
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

        //  --color: this window shows the shell's ANSI colours (put()).  The
        //  kernel takes 127 characters of arguments.
        char args[128] = "sh.elf --color --host 0x";
        size_t at = strlen(args);
        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 28; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at] = 0;
        if (script && *script)
        {
            if (at + 7 + strlen(script) >= sizeof(args))
            {
                strcpy(error, "The script's path is too long.");
                release();
                return;
            }
            for (const char *k = " --run "; *k; k++)
                args[at++] = *k;
            for (const char *k = script; *k; k++)
                args[at++] = *k;
            args[at] = 0;
        }
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
    //  And each cell's colours: the foreground in the low nibble and the
    //  background in the high one, as VGA has them (palette below).
    uint8_t attrs[Keep][Cols];
    static const uint8_t Plain = 0x07; // light grey on black
    uint8_t attr = Plain;              // what put() writes with now
    //  An ANSI escape sequence being read (ESC [ n ; n m): 0 none, 1 after
    //  ESC, 2 in the parameters.
    int esc = 0;
    uint8_t escArgs[8];
    int escN = 0;
    long first = 0, last = 0;
    int col = 0;
    int back = 0; // lines scrolled back from the bottom

    PlatformColor *bg = nullptr, *fg = nullptr, *dim = nullptr, *cursor = nullptr;
    PlatformColor *palette[16] = {};
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

    uint8_t *attrRow(long n) { return attrs[n % Keep]; }

    void clearScreen()
    {
        first = last = 0;
        col = 0;
        back = 0;
        memset(lines[0], ' ', Cols);
        memset(attrs[0], attr, Cols);
    }

    void newLine()
    {
        last++;
        memset(row(last), ' ', Cols);
        memset(attrRow(last), attr, Cols);
        if (last - first >= Keep)
            first = last - Keep + 1;
        col = 0;
    }

    //  One SGR parameter onto `attr`, the way the kernel's console takes it:
    //  0 resets, 1 brightens, 30-37/90-97 the foreground, 40-47/100-107 the
    //  background, 39/49 the defaults.
    void sgr(uint8_t a)
    {
        static const uint8_t ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7}; // ANSI's order in VGA's
        uint8_t f = attr & 15, b = attr >> 4;
        if (a == 0)
            f = Plain & 15, b = Plain >> 4;
        else if (a == 1)
            f |= 8;
        else if (a == 22)
            f &= 7;
        else if (a >= 30 && a <= 37)
            f = (uint8_t)(ansi[a - 30] | (f & 8));
        else if (a == 39)
            f = Plain & 15;
        else if (a >= 40 && a <= 47)
            b = ansi[a - 40];
        else if (a == 49)
            b = Plain >> 4;
        else if (a >= 90 && a <= 97)
            f = (uint8_t)(ansi[a - 90] | 8);
        else if (a >= 100 && a <= 107)
            b = (uint8_t)(ansi[a - 100] | 8);
        attr = (uint8_t)(b << 4 | f);
    }

    //  Takes <c> when it belongs to an escape sequence.  Only colours (the
    //  final 'm') do anything; any other sequence is swallowed, and a control
    //  byte in the middle of one ends it and is put as ever.
    bool escape(uint8_t c)
    {
        if (esc == 0)
        {
            if (c != 0x1b)
                return false;
            esc = 1;
            return true;
        }
        if (esc == 1)
        {
            esc = 0;
            if (c != '[')
                return false;
            esc = 2;
            memset(escArgs, 0, sizeof(escArgs));
            escN = 0;
            return true;
        }
        if (c >= '0' && c <= '9')
        {
            if (escN < 8)
                escArgs[escN] = (uint8_t)(escArgs[escN] * 10 + (c - '0'));
            return true;
        }
        if (c == ';')
        {
            if (escN < 8)
                escN++;
            return true;
        }
        if (c >= 0x40 && c <= 0x7e)
        {
            esc = 0;
            if (c == 'm')
                for (int i = 0; i <= escN && i < 8; i++)
                    sgr(escArgs[i]);
            return true;
        }
        if (c >= 0x20)
            return true;
        esc = 0;
        return false;
    }

    void put(uint8_t c)
    {
        if (escape(c))
            return;
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
        attrRow(last)[col] = attr;
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
        static const uint32_t vga[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                         0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                         0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
        for (int i = 0; i < 16; i++)
            palette[i] = dc->CreateColor(0xFF000000 | vga[i], nullptr, nullptr);
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
            const uint8_t *a = attrRow(n);
            //  Up to the last cell that shows anything: a character, or a
            //  background of its own.
            int len = Cols;
            while (len > 0 && r[len - 1] == ' ' && !(a[len - 1] >> 4))
                len--;
            double y = oy + (n - top) * ch;
            //  In runs of one colour.
            for (int k = 0; k < len;)
            {
                int end = k;
                while (end < len && a[end] == a[k])
                    end++;
                for (int j = k; j < end; j++)
                {
                    uint8_t c = (uint8_t)r[j];
                    text[j - k] = c < 0x20 ? ' ' : (char)c;
                }
                text[end - k] = 0;
                double x = ox + k * cw;
                if ((a[k] >> 4) && palette[a[k] >> 4])
                    target->FillRect(Coord(x), Coord(y), Coord((end - k) * cw), Coord(ch), palette[a[k] >> 4], false);
                o.foreground = palette[a[k] & 15] ? palette[a[k] & 15] : fg;
                target->DrawText(Coord(x), Coord(y), Coord((end - k) * cw + cw), Coord(ch), (const mchar *)text, &o, false);
                k = end;
            }
        }
        o.foreground = fg;

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
