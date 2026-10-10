//
// Window 4 — Task Manager  (live data via r2::tasks(), syscall 0x2F)
//
// The table used to be read through a 20-byte struct declared here. The kernel
// writes 28-byte entries — id, mode, status, pad, a 16-byte name and the task's
// instruction pointer — and it advances 28 bytes per task whatever the caller
// believes, so every row after the first was decoded from the middle of the
// entry before it and the 10-entry buffer was overrun by 80 bytes. The layout
// now comes from libc++r2, which has the same one the kernel and c/libcr2 do,
// and the instruction pointer it was hiding is a column of its own.
//
// Killing: Del or K on a row, the Kill button, or a PID typed on the keyboard
// and Enter, then Y to confirm (syscall 0x3B, through r2::kill).  Kernel tasks
// and Memento itself are refused: the one would take the machine with it, the
// other every window on the screen.
//
// The two lists sit under tabs along the top, Tasks and Memory: a click on
// one, Tab, T and M, or Left and Right switch between them.  Memory is the user
// heap with a bar for how full it is, each program with its 2 MiB frame and
// what it holds on the heap, and Memento's own arena --- from syscall 0x3C
// through r2::meminfo(), read again every second while the tab is showing.
//
// Nothing but the tab strip has a fixed place: the columns are spread over the
// width the window has and the lists take the height left between the strip
// and the buttons, so a maximised window shows more rows, not a margin.  There
// can be 32 tasks: the list scrolls to keep the bar in view, the wheel and
// PgUp/PgDn scroll either list, and a thumb at the right edge says where the
// rows are.
//

class TasksWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<TasksWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    int sel = 0;
    int nLive = 0; // last known task count; key handlers use it
    int top = 0;    // the first task row shown
    int memTop = 0; // and the first row of the Memory tab's list
    int rowsShown = 0; // task rows the last paint had room for
    int memRows = 0;   // and Memory rows
    bool follow = true; // bring the bar into view on the next paint
    r2::vector<r2::TaskInfo> tasks; // what the last read returned
    unsigned long lastRead = 0;
    bool haveTasks = false;
    int backX = 0; // where the Back button ended up, for the hit test
    int backY = 0;
    int killX = 0; // and the Kill button
    char typed[4] = {}; // a PID being typed
    int confirmPid = -1; // waiting for Y to kill this one
    char status[64] = {}; // the line under the table
    bool memTab = false;     // showing Memory rather than the tasks
    r2::optional<r2::MemInfo> mem; // what the last read of it returned
    unsigned long memRead = 0;

    // Layout in the window's own client area — the root draws the frame, the
    // title bar and the line on the taskbar, so everything here starts at the
    // top left corner of the content.  The font is the 8x16 glyph, 4 x 8 of
    // these units.
    static const int HEAD_Y = TabStrip::BELOW; // column headers, under the tabs
    static const int ROW_Y = HEAD_Y + 12;    // first task row
    static const int ROW_H = 10;             // one row
    static const int BACK_W = 80, BACK_H = 11;
    static const int KILL_W = 60;
    static const int MARGIN = 6; // left of the first column, and right of the last
    static const int NCOLS = 5;

    // Columns: PID, name, mode, status, and where the task was last put down,
    // as they sit in the 290-wide window the layout was drawn for. A wider one
    // stretches them; the address is 16 digits and keeps the 64 they need.
    static constexpr int TASK_COLS[NCOLS] = {6, 28, 130, 162, 214};
    static constexpr int MEM_COLS[4] = {6, 28, 110, 200};
    static const int BASE_RIGHT = 278; // where the last column ends at 290

    // A list's columns for a window W wide: the starts above scaled from
    // their 290 layout, and each one as wide as the gap to the next.
    static void spread(const int *base, int n, int W, int *x, int *w)
    {
        int right = W - MARGIN - SCROLL_ROOM;
        for (int i = 0; i < n; i++)
            x[i] = MARGIN + (base[i] - MARGIN) * (right - MARGIN) / (BASE_RIGHT - MARGIN);
        for (int i = 0; i < n; i++)
            w[i] = (i + 1 < n ? x[i + 1] - 2 : right) - x[i];
    }

    // Where the bottom row of things goes: the buttons along the bottom edge,
    // a line over them and a line of text over that. The lists end above it.
    static int backTop(int H) { return H - BACK_H - 2; }
    static int statusTop(int H) { return backTop(H) - 3 - (ROW_H + 1); }
    static int rowsFit(int y, int H)
    {
        int rows = (statusTop(H) - 2 - y) / ROW_H;
        return rows < 1 ? 1 : rows;
    }

    static const char *statusStr(unsigned char s)
    {
        if (s == 0)
            return "Ready";
        if (s == 1)
            return "Running";
        if (s == 2)
            return "Idle";
        if (s == 3)
            return "Blocked";
        if (s == 4)
            return "Crashed";
        if (s == 5)
            return "Dead";
        return "?";
    }
    static const char *modeStr(unsigned char m) { return m ? "User" : "Kernel"; }

    static void pidStr(unsigned char n, char *out)
    {
        if (n >= 100)
        {
            out[0] = '0' + n / 100;
            out[1] = '0' + (n / 10) % 10;
            out[2] = '0' + n % 10;
            out[3] = 0;
        }
        else if (n >= 10)
        {
            out[0] = '0' + n / 10;
            out[1] = '0' + n % 10;
            out[2] = 0;
        }
        else
        {
            out[0] = '0' + n;
            out[1] = 0;
        }
    }

    // The name is a fixed 16-byte field and is not NUL-terminated when it
    // fills it, so the copy stops at the first NUL and at anything that is not
    // printable rather than handing stray bytes to the text drawer.
    static void nameStr(const unsigned char *name, char *out)
    {
        int at = 0;
        for (int i = 0; i < 16; i++)
        {
            unsigned char c = name[i];
            if (c == 0)
                break;
            out[at++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out[at] = 0;
    }

    // "TCPP    .ELF" -> "TCPP.ELF": the padding is the 8.3 name's, not
    // something to show in a sentence.
    static void compact(char *name)
    {
        int to = 0;
        for (int from = 0; name[from]; from++)
            if (name[from] != ' ')
                name[to++] = name[from];
        name[to] = 0;
    }

    // "0000000000601A30" — the address on its own; a 0x prefix would cost four
    // of the characters the column has for the digits that matter.
    static void ripStr(unsigned long long rip, char *out)
    {
        static const char digits[] = "0123456789ABCDEF";
        if (rip == 0)
        {
            out[0] = '-';
            out[1] = 0;
            return;
        }
        for (int i = 0; i < 16; i++)
            out[i] = digits[(rip >> ((15 - i) * 4)) & 0xF];
        out[16] = 0;
    }

    // n in decimal onto out.
    static void appendU(char *out, size_t cap, unsigned long long n)
    {
        char num[24];
        int k = 0;
        do
        {
            num[k++] = (char)('0' + n % 10);
            n /= 10;
        } while (n);
        size_t at = strlen(out);
        while (k && at + 1 < cap)
            out[at++] = num[--k];
        out[at] = 0;
    }

    // Bytes as KiB, rounded: "1166 KiB".
    static void appendKiB(char *out, size_t cap, unsigned long long bytes)
    {
        appendU(out, cap, (bytes + 512) / 1024);
        append(out, cap, " KiB");
    }

    static void appendHex(char *out, size_t cap, unsigned long long v)
    {
        char num[20];
        int k = 0;
        do
        {
            num[k++] = "0123456789ABCDEF"[v & 15];
            v >>= 4;
        } while (v);
        append(out, cap, "0x");
        size_t at = strlen(out);
        while (k && at + 1 < cap)
            out[at++] = num[--k];
        out[at] = 0;
    }

    void setTab(bool memory)
    {
        memTab = memory;
        typed[0] = 0;
        confirmPid = -1;
        status[0] = 0;
        memRead = 0;
        // The Memory tab repaints itself once a second: the idle loop turns
        // only while it is showing.
        wnd->SetImmediateMode(memory);
        wnd->Repaint();
    }

    void onIdle()
    {
        if (!memTab)
            return;
        unsigned long now = (unsigned long)r2::ticks();
        if (now - memRead >= 1000)
            wnd->Repaint();
        else
            r2::sleep(10);
    }

    static void append(char *out, size_t cap, const char *s)
    {
        size_t at = strlen(out);
        while (*s && at + 1 < cap)
            out[at++] = *s++;
        out[at] = 0;
    }

    const r2::TaskInfo *findTask(int pid) const
    {
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid)
                return &tasks[i];
        return nullptr;
    }

    // Asks before killing: says what the PID is and waits for Y.
    void askKill(int pid)
    {
        typed[0] = 0;
        confirmPid = -1;
        // The table may have moved on since the paint. The kernel answers an
        // empty list when its scheduler is busy; the last one stands then.
        {
            r2::vector<r2::TaskInfo> fresh = r2::tasks();
            if (!fresh.empty())
            {
                tasks = static_cast<r2::vector<r2::TaskInfo> &&>(fresh);
                lastRead = (unsigned long)r2::ticks();
            }
        }
        const r2::TaskInfo *t = findTask(pid);
        char pidbuf[4], namebuf[17];
        pidStr((unsigned char)pid, pidbuf);
        status[0] = 0;
        if (!t)
        {
            append(status, sizeof status, "No task has PID ");
            append(status, sizeof status, pidbuf);
            append(status, sizeof status, ".");
            return;
        }
        nameStr(t->name, namebuf);
        compact(namebuf);
        if (t->mode == 0)
        {
            append(status, sizeof status, namebuf);
            append(status, sizeof status, " is the kernel's: not killed.");
            return;
        }
        if (!strncmp(namebuf, "MEMENTO", 7))
        {
            append(status, sizeof status, "That is Memento itself: not killed.");
            return;
        }
        confirmPid = pid;
        append(status, sizeof status, "Kill ");
        append(status, sizeof status, pidbuf);
        append(status, sizeof status, " ");
        append(status, sizeof status, namebuf);
        append(status, sizeof status, "?  Y / N");
    }

    void doKill()
    {
        int pid = confirmPid;
        confirmPid = -1;
        char pidbuf[4];
        pidStr((unsigned char)pid, pidbuf);
        status[0] = 0;
        if (r2::kill((uint8_t)pid))
        {
            append(status, sizeof status, "Killed ");
            append(status, sizeof status, pidbuf);
            append(status, sizeof status, ".");
        }
        else
        {
            append(status, sizeof status, "Could not kill ");
            append(status, sizeof status, pidbuf);
            append(status, sizeof status, ".");
        }
        haveTasks = false; // read the table again on the paint
    }

    void killSelected()
    {
        if (sel < nLive && sel < (int)tasks.size())
            askKill(tasks[sel].id);
    }

    // The bar onto row s, or onto Back past the last one, and into view.
    void moveSel(int s)
    {
        if (s > nLive)
            s = nLive;
        if (s < 0)
            s = 0;
        if (s == sel)
            return;
        sel = s;
        follow = true;
        wnd->Repaint();
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            onIdle();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseWheel)
        {
            // The list under the wheel scrolls; the bar stays on its task,
            // in view or not. The paint keeps either list in range.
            int d = data->Data.OnMouseWheel.up ? -3 : 3;
            if (memTab)
                memTop += d;
            else
                top += d;
            wnd->Repaint();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            int tab = TabStrip::at(F_COORD(mx), F_COORD(my), 2); // 0 Tasks, 1 Memory
            if (tab >= 0)
            {
                if ((tab == 1) != memTab)
                    setTab(tab == 1);
                return;
            }
            if (my >= backY && my < backY + BACK_H && mx >= backX && mx < backX + BACK_W)
            {
                wnd->Close();
                return;
            }
            if (memTab)
                return;
            if (my >= backY && my < backY + BACK_H && mx >= killX && mx < killX + KILL_W)
            {
                killSelected();
                wnd->Repaint();
                return;
            }
            // Task rows: a hit selects the row it landed in
            for (int r = 0; r < rowsShown && top + r < nLive; r++)
            {
                if (my >= ROW_Y + r * ROW_H && my < ROW_Y + r * ROW_H + (ROW_H - 1))
                {
                    sel = top + r;
                    wnd->Repaint();
                    break;
                }
            }
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;

        auto *key = data->Data.OnKeyEvent.key;

        if (!key->isKeyDown)
            return;

        char c = key->isChar ? (char)key->theChar : 0;
        // Tab flips between the two; T and Left, M and Right pick one. Not
        // while a PID is being typed or a kill waits for its Y.
        if (confirmPid < 0 && !typed[0])
        {
            int want = -1;
            if (key->isTab)
                want = memTab ? 0 : 1;
            else if (c == 't' || c == 'T' || key->isArrowLeft)
                want = 0;
            else if (c == 'm' || c == 'M' || key->isArrowRight)
                want = 1;
            if (want >= 0)
            {
                if ((want == 1) != memTab)
                    setTab(want == 1);
                return;
            }
        }
        if (memTab)
        {
            int page = memRows > 1 ? memRows - 1 : 1;
            int to = memTop; // drawMemory stops it at the last row
            if (key->isEscape || key->isEnter)
            {
                wnd->Close();
                return;
            }
            if (key->isArrowUp)
                to--;
            else if (key->isArrowDown)
                to++;
            else if (key->isPageUp)
                to -= page;
            else if (key->isPageDown)
                to += page;
            else if (key->isHome)
                to = 0;
            else if (key->isEnd)
                to = r2::MaxSlots + 1;
            if (to < 0)
                to = 0;
            if (to != memTop)
            {
                memTop = to;
                wnd->Repaint();
            }
            return;
        }
        if (confirmPid >= 0)
        {
            // Y kills; anything else lets it be.
            if (c == 'y' || c == 'Y')
                doKill();
            else
            {
                confirmPid = -1;
                strcpy(status, "Not killed.");
            }
            wnd->Repaint();
            return;
        }
        if (c >= '0' && c <= '9')
        {
            size_t n = strlen(typed);
            if (n < 3)
            {
                typed[n] = c;
                typed[n + 1] = 0;
            }
            status[0] = 0;
            append(status, sizeof status, "Kill PID ");
            append(status, sizeof status, typed);
            append(status, sizeof status, "_   (Enter)");
            wnd->Repaint();
            return;
        }
        if (key->isBackspace && typed[0])
        {
            typed[strlen(typed) - 1] = 0;
            status[0] = 0;
            if (typed[0])
            {
                append(status, sizeof status, "Kill PID ");
                append(status, sizeof status, typed);
                append(status, sizeof status, "_   (Enter)");
            }
            wnd->Repaint();
            return;
        }
        if (key->isEnter && typed[0])
        {
            int pid = 0;
            for (const char *d = typed; *d; d++)
                pid = pid * 10 + (*d - '0');
            askKill(pid);
            wnd->Repaint();
            return;
        }
        if (key->isDelete || c == 'k' || c == 'K')
        {
            killSelected();
            wnd->Repaint();
            return;
        }
        if (key->isEscape)
        {
            if (typed[0] || status[0])
            {
                typed[0] = 0;
                status[0] = 0;
                wnd->Repaint();
                return;
            }
            wnd->Close();
            return;
        }
        // Up and Down go a row at a time, Down past the last row onto Back;
        // the page keys go a screenful and stay on the rows.
        int page = rowsShown > 1 ? rowsShown - 1 : 1;
        int last = nLive > 0 ? nLive - 1 : 0;
        if (key->isArrowUp)
            moveSel(sel - 1);
        else if (key->isArrowDown)
            moveSel(sel + 1);
        else if (key->isPageUp)
            moveSel(sel - page);
        else if (key->isPageDown)
            moveSel(sel + page < last ? sel + page : last);
        else if (key->isHome)
            moveSel(0);
        else if (key->isEnd)
            moveSel(last);
        else if (key->isEnter && sel == nLive)
            wnd->Close();
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!dark)
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light)
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        // One size, the same one every other window uses: on the 640x400
        // screen that is the 8x16 glyph, which is what lets five columns and a
        // 16-digit address share a 290-wide window.
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font)
            return;

        // Live task list, read at most a couple of times a second: a paint
        // happens on every mouse move now, and each read takes the scheduler's
        // lock. The vector comes from the arena and the entries have the
        // kernel's own layout, so nothing here has to know the stride.
        unsigned long now = (unsigned long)r2::ticks();
        if (!haveTasks || now - lastRead >= 500)
        {
            r2::vector<r2::TaskInfo> fresh = r2::tasks();
            if (!fresh.empty() || !haveTasks)
                tasks = static_cast<r2::vector<r2::TaskInfo> &&>(fresh);
            lastRead = now;
            haveTasks = true;
        }
        int n = (int)tasks.size();
        nLive = n;
        if (sel > nLive)
            sel = nLive;

        Coord cw = target->GetWidth();
        Coord ch = target->GetHeight();
        int W = F_COORD(cw), H = F_COORD(ch);

        target->FillRect(0, 0, cw, ch, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        // The page first: the strip's corner note on Memory needs the
        // reading it takes.
        if (memTab)
            drawMemory(target, opts, W, H);
        else
            drawTasks(target, opts, W, H, n);
        drawTabs(target, opts, W, n);

        // Separator + Back button along the bottom of the client area
        backX = (W - BACK_W) / 2;
        backY = backTop(H);
        target->FillRect(2, backY - 3, W - 4, 1, dark, false);
        bool backFocused = !memTab && (sel == nLive);
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        if (backFocused)
        {
            target->FillRect(backX, backY, BACK_W, BACK_H, dark, false);
            opts.foreground = light;
        }
        else
        {
            target->FillRect(backX, backY, BACK_W, BACK_H, dark, false);
            target->FillRect(backX + 1, backY + 1, BACK_W - 2, BACK_H - 2, light, false);
            opts.foreground = dark;
        }

        target->DrawText(backX, backY, BACK_W, BACK_H, "Back", &opts, false);

        // Kill, at the left: the task on the bar.
        killX = MARGIN;
        if (!memTab)
            button(target, opts, killX, backY, "Kill");
    }

    void button(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, const char *label)
    {
        target->FillRect(x, y, KILL_W, BACK_H, dark, false);
        target->FillRect(x + 1, y + 1, KILL_W - 2, BACK_H - 2, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(x, y, KILL_W, BACK_H, (const mchar *)label, &opts, false);
    }

    // The strip: Tasks and Memory, and at the right end a word on what the
    // page holds.
    void drawTabs(PlatformBitmap *target, PlatformDrawTextOptions &opts, int W, int n)
    {
        static const char *const labels[2] = {"Tasks", "Memory"};
        char note[48];
        note[0] = 0;
        if (memTab)
        {
            if (mem)
            {
                append(note, sizeof note, "RAM ");
                appendU(note, sizeof note, mem->total_ram / (1024 * 1024));
                append(note, sizeof note, " MiB");
            }
        }
        else
        {
            int user = 0;
            for (int i = 0; i < n; i++)
                user += tasks[i].mode != 0;
            appendU(note, sizeof note, n);
            append(note, sizeof note, n == 1 ? " task, " : " tasks, ");
            appendU(note, sizeof note, user);
            append(note, sizeof note, " user");
        }
        TabStrip::draw(target, opts, dark, light, W, labels, 2, memTab ? 1 : 0, note);
    }

    // A thumb along the right edge for a list of `total` rows of which `shown`
    // fit, from `first`; nothing when they all fit.  Rows leave it room.
    static const int SCROLL_ROOM = 8;
    void scrollbar(PlatformBitmap *target, int y, int h, int first, int shown, int total, int W)
    {
        if (total <= shown)
            return;
        int thumbH = h * shown / total;
        if (thumbH < 4)
            thumbH = 4;
        int thumbY = y + (h - thumbH) * first / (total - shown);
        target->FillRect(W - 6, y, 1, h, dark, false);
        target->FillRect(W - 7, thumbY, 3, thumbH, dark, false);
    }

    void drawTasks(PlatformBitmap *target, PlatformDrawTextOptions &opts, int W, int H, int n)
    {
        int x[NCOLS], w[NCOLS];
        spread(TASK_COLS, NCOLS, W, x, w);

        // Column headers
        static const char *const heads[NCOLS] = {"PID", "Name", "Mode", "Status", "RIP"};
        opts.foreground = dark;
        for (int c = 0; c < NCOLS; c++)
            target->DrawText(x[c], HEAD_Y, w[c], 10, (const mchar *)heads[c], &opts, false);
        target->FillRect(2, ROW_Y - 2, W - 4, 1, dark, false);

        // As many rows as the height leaves room for. The bar is brought into
        // view when it moves and when the window changes size; the wheel may
        // scroll it out, and on Back the list stays where it was.
        int rows = rowsFit(ROW_Y, H);
        if (rows != rowsShown)
        {
            rowsShown = rows;
            follow = true;
        }
        if (follow && sel < n)
        {
            if (sel < top)
                top = sel;
            if (sel >= top + rows)
                top = sel - rows + 1;
        }
        follow = false;
        if (top > n - rows)
            top = n - rows;
        if (top < 0)
            top = 0;

        int rowW = n > rows ? W - 4 - SCROLL_ROOM : W - 4;
        for (int r = 0; r < rows && top + r < n; r++)
        {
            int i = top + r;
            const r2::TaskInfo &task = tasks[i];
            int ry = ROW_Y + r * ROW_H;
            if (sel == i)
            {
                target->FillRect(2, ry, rowW, ROW_H - 1, dark, false);
                opts.foreground = light;
            }
            else
            {
                opts.foreground = dark;
            }

            char pidbuf[4];
            pidStr(task.id, pidbuf);
            char namebuf[17];
            nameStr(task.name, namebuf);
            char ripbuf[17];
            ripStr(task.rip, ripbuf);

            target->DrawText(x[0], ry, w[0], ROW_H - 1, (const mchar *)pidbuf, &opts, false);
            target->DrawText(x[1], ry, w[1], ROW_H - 1, (const mchar *)namebuf, &opts, false);
            target->DrawText(x[2], ry, w[2], ROW_H - 1, (const mchar *)modeStr(task.mode), &opts, false);
            target->DrawText(x[3], ry, w[3], ROW_H - 1, (const mchar *)statusStr(task.status), &opts, false);
            target->DrawText(x[4], ry, w[4], ROW_H - 1, (const mchar *)ripbuf, &opts, false);
        }
        scrollbar(target, ROW_Y, rows * ROW_H - 1, top, rows, n, W);

        // What a kill is waiting for or came to, else how to ask for one.
        opts.foreground = dark;
        target->DrawText(MARGIN, statusTop(H), W - 2 * MARGIN, 10,
                         status[0] ? (const mchar *)status : "Del/K: kill the task on the bar   0-9 Enter: by PID", &opts,
                         false);
    }

    // ── The Memory tab ──────────────────────────────────────────────────────

    // The heap's line, its bar and the line under it, then the programs.
    static const int INFO_Y = HEAD_Y, BAR_Y = INFO_Y + 11, BAR_H = 7;
    static const int USAGE_Y = BAR_Y + BAR_H + 2;
    static const int MEM_HEAD_Y = USAGE_Y + 13, MEM_ROW_Y = MEM_HEAD_Y + 12;

    void text(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, int w, const char *s)
    {
        target->DrawText(x, y, w, ROW_H - 1, (const mchar *)s, &opts, false);
    }

    void drawMemory(PlatformBitmap *target, PlatformDrawTextOptions &opts, int W, int H)
    {
        unsigned long now = (unsigned long)r2::ticks();
        if (!mem || now - memRead >= 1000)
        {
            // The kernel says busy when the heap or the scheduler is locked
            // that instant; the last reading stands then.
            r2::optional<r2::MemInfo> fresh = r2::meminfo();
            if (fresh)
                mem = fresh;
            memRead = now;
        }
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        if (!mem)
        {
            text(target, opts, MARGIN, INFO_Y, W - 2 * MARGIN,
                 "No memory information: this kernel is older than syscall 0x3C.");
            return;
        }
        const r2::MemInfo &m = *mem;
        char line[96];

        // The user heap: where it is and how full, as a line and a bar.
        line[0] = 0;
        append(line, sizeof line, "User heap  ");
        appendKiB(line, sizeof line, m.heap_size);
        append(line, sizeof line, " at ");
        appendHex(line, sizeof line, m.heap_start);
        append(line, sizeof line, ", shared by all");
        text(target, opts, MARGIN, INFO_Y, W - 2 * MARGIN, line);

        const int barX = MARGIN, barW = W - 2 * MARGIN;
        target->FillRect(barX, BAR_Y, barW, BAR_H, dark, false);
        target->FillRect(barX + 1, BAR_Y + 1, barW - 2, BAR_H - 2, light, false);
        if (m.heap_size)
        {
            int used = (int)((unsigned long long)(barW - 2) * (m.heap_size - m.heap_free) / m.heap_size);
            target->FillRect(barX + 1, BAR_Y + 1, used, BAR_H - 2, dark, false);
        }

        line[0] = 0;
        append(line, sizeof line, "used ");
        appendKiB(line, sizeof line, m.heap_used);
        append(line, sizeof line, " (");
        appendU(line, sizeof line, m.heap_size ? m.heap_used * 100 / m.heap_size : 0);
        append(line, sizeof line, "%)  free ");
        appendKiB(line, sizeof line, m.heap_free);
        append(line, sizeof line, "  largest ");
        appendKiB(line, sizeof line, m.heap_largest_free);
        append(line, sizeof line, "  blocks ");
        appendU(line, sizeof line, m.heap_blocks);
        append(line, sizeof line, "/");
        appendU(line, sizeof line, m.heap_free_blocks);
        append(line, sizeof line, " free");
        text(target, opts, MARGIN, USAGE_Y, W - 2 * MARGIN, line);

        // Each program: its frame and its share of the heap.  Kernel tasks run
        // on the kernel's own mappings and hold nothing of either, so they
        // are counted on the last line rather than listed.
        int x[4], w[4];
        spread(MEM_COLS, 4, W, x, w);
        text(target, opts, x[0], MEM_HEAD_Y, w[0], "PID");
        text(target, opts, x[1], MEM_HEAD_Y, w[1], "Name");
        text(target, opts, x[2], MEM_HEAD_Y, w[2], "Frame (2 MiB)");
        text(target, opts, x[3], MEM_HEAD_Y, w[3], "Heap held");
        target->FillRect(2, MEM_ROW_Y - 2, W - 4, 1, dark, false);

        // The rows are picked first, as there can be more of them than fit
        // with 32 slots, and the list shown from memTop.
        int rows[r2::MaxSlots + 1];
        int nRows = 0, kernelTasks = 0;
        int slots = m.slots < r2::MaxSlots ? (int)m.slots : r2::MaxSlots;
        for (int sl = 0; sl <= r2::MaxSlots; sl++)
        {
            uint8_t id = sl < r2::MaxSlots ? m.slot_task[sl] : 0xFF;
            unsigned long long held = m.heap_by_slot[sl];
            const r2::TaskInfo *t = (sl < slots && id != 0xFF) ? findTask(id) : nullptr;
            if (t && t->mode == 0)
            {
                kernelTasks++;
                if (!held)
                    continue;
            }
            if (!t && !held)
                continue;
            rows[nRows++] = sl;
        }
        memRows = rowsFit(MEM_ROW_Y, H);
        if (memTop > nRows - memRows)
            memTop = nRows - memRows;
        if (memTop < 0)
            memTop = 0;

        for (int row = 0; row < memRows && memTop + row < nRows; row++)
        {
            int sl = rows[memTop + row];
            uint8_t id = sl < r2::MaxSlots ? m.slot_task[sl] : 0xFF;
            unsigned long long held = m.heap_by_slot[sl];
            const r2::TaskInfo *t = (sl < slots && id != 0xFF) ? findTask(id) : nullptr;

            int y = MEM_ROW_Y + row * ROW_H;
            char cell[40];
            if (t)
            {
                cell[0] = 0;
                appendU(cell, sizeof cell, id);
                text(target, opts, x[0], y, w[0], cell);
                char namebuf[17];
                nameStr(t->name, namebuf);
                compact(namebuf);
                text(target, opts, x[1], y, w[1], namebuf);
            }
            else
            {
                // Bytes tagged to a slot nobody is in: a program that died
                // without its blocks being swept yet --- or, for the last
                // row, blocks allocated with no owner at all.
                text(target, opts, x[0], y, w[0], "-");
                text(target, opts, x[1], y, w[1], sl == r2::MaxSlots ? "(no owner)" : "(exited)");
            }
            cell[0] = 0;
            if (t && t->mode != 0)
            {
                append(cell, sizeof cell, "slot ");
                appendU(cell, sizeof cell, sl);
                append(cell, sizeof cell, " at "); // the font has no @
                appendHex(cell, sizeof cell, m.frame_base + sl * m.frame_size);
            }
            else
                append(cell, sizeof cell, "-");
            text(target, opts, x[2], y, w[2], cell);
            cell[0] = 0;
            appendKiB(cell, sizeof cell, held);
            text(target, opts, x[3], y, w[3], cell);
        }
        scrollbar(target, MEM_ROW_Y, memRows * ROW_H - 1, memTop, memRows, nRows, W);

        // Memento's own heap: its arena, which grows from the user heap.
        r2::heap::Stats st = r2::heap::stats();
        line[0] = 0;
        append(line, sizeof line, "Memento arena ");
        appendKiB(line, sizeof line, st.arena_bytes);
        append(line, sizeof line, " in ");
        appendU(line, sizeof line, st.regions);
        append(line, sizeof line, st.regions == 1 ? " piece, " : " pieces, ");
        appendKiB(line, sizeof line, st.used_bytes);
        append(line, sizeof line, " used, largest free ");
        appendKiB(line, sizeof line, st.largest_free_block);
        if (kernelTasks)
        {
            append(line, sizeof line, "; ");
            appendU(line, sizeof line, kernelTasks);
            append(line, sizeof line, " kernel tasks");
        }
        text(target, opts, MARGIN, statusTop(H), W - 2 * MARGIN, line);
    }
};
