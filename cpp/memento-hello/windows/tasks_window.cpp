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
// Memory, the second tab (Tab, M, or the button at the right): the user heap
// with a bar for how full it is, each program with its 2 MiB frame and what it
// holds on the heap, and Memento's own arena --- from syscall 0x3C through
// r2::meminfo(), read again every second while the tab is showing.
//
// There can be 32 tasks, more than the ten rows the window has room for: the
// list scrolls to keep the bar in view, and the Memory tab's list scrolls with
// the arrows.  A thumb at the right edge says where the rows are.
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
    int tabX = 0; // the Memory/Tasks button

    // Layout in the window's own client area — the root draws the frame, the
    // title bar and the line on the taskbar, so everything here starts at the
    // top left corner of the content.
    static const int HEAD_Y = 3;    // column headers
    static const int ROW_Y = 15;    // first task row
    static const int ROW_H = 10;    // one row
    static const int VISIBLE_ROWS = 10; // task rows on screen at once
    static const int BACK_W = 80, BACK_H = 11;
    static const int KILL_W = 60;
    static const int STATUS_Y = ROW_Y + VISIBLE_ROWS * ROW_H + 5;

    // Columns: PID, name, mode, status, and where the task was last put down.
    static const int COL_PID = 6, COLW_PID = 22;
    static const int COL_NAME = 30, COLW_NAME = 104;
    static const int COL_MODE = 136, COLW_MODE = 32;
    static const int COL_STATUS = 170, COLW_STATUS = 48;
    static const int COL_RIP = 220, COLW_RIP = 66;

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
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            if (my >= backY && my < backY + BACK_H && mx >= backX && mx < backX + BACK_W)
            {
                wnd->Close();
                return;
            }
            if (my >= backY && my < backY + BACK_H && mx >= tabX && mx < tabX + KILL_W)
            {
                setTab(!memTab);
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
            for (int r = 0; r < VISIBLE_ROWS && top + r < nLive; r++)
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
        if (confirmPid < 0 && !typed[0] && (key->isTab || c == 'm' || c == 'M' || (memTab && (c == 't' || c == 'T'))))
        {
            setTab(!memTab);
            return;
        }
        if (memTab)
        {
            if (key->isEscape || key->isEnter)
                wnd->Close();
            else if (key->isArrowUp && memTop > 0)
            {
                memTop--;
                wnd->Repaint();
            }
            else if (key->isArrowDown)
            {
                memTop++; // drawMemory stops it at the last row
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
        if (key->isArrowUp)
        {
            if (sel > 0)
            {
                sel--;
                wnd->Repaint();
            }
            return;
        }
        if (key->isArrowDown)
        {
            if (sel < nLive)
            {
                sel++;
                wnd->Repaint();
            }
            return;
        }
        if (key->isEnter && sel == nLive)
        {
            wnd->Close();
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
        // One size, the same one every other window uses: on the 640x400
        // screen that is the 8x16 glyph, which is what lets five columns and a
        // 16-digit address share a 320-wide window.
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
        // The bar stays in view; on Back the list stays where it was.
        if (sel < nLive && sel < top)
            top = sel;
        if (sel < nLive && sel >= top + VISIBLE_ROWS)
            top = sel - VISIBLE_ROWS + 1;
        if (top > n - VISIBLE_ROWS)
            top = n - VISIBLE_ROWS;
        if (top < 0)
            top = 0;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        if (memTab)
            drawMemory(target, opts, F_COORD(W));
        else
            drawTasks(target, opts, F_COORD(W), n);

        // Separator + Back button along the bottom of the client area
        backX = (F_COORD(W) - BACK_W) / 2;
        backY = F_COORD(H) - BACK_H - 2;
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
        killX = 6;
        if (!memTab)
            button(target, opts, killX, backY, "Kill");

        // The other tab, at the right.
        tabX = F_COORD(W) - 6 - KILL_W;
        button(target, opts, tabX, backY, memTab ? "Tasks" : "Memory");
    }

    void button(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, const char *label)
    {
        target->FillRect(x, y, KILL_W, BACK_H, dark, false);
        target->FillRect(x + 1, y + 1, KILL_W - 2, BACK_H - 2, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(x, y, KILL_W, BACK_H, (const mchar *)label, &opts, false);
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

    void drawTasks(PlatformBitmap *target, PlatformDrawTextOptions &opts, int W, int n)
    {
        // Column headers
        target->DrawText(COL_PID, HEAD_Y, COLW_PID, 10, "PID", &opts, false);
        target->DrawText(COL_NAME, HEAD_Y, COLW_NAME, 10, "Name", &opts, false);
        target->DrawText(COL_MODE, HEAD_Y, COLW_MODE, 10, "Mode", &opts, false);
        target->DrawText(COL_STATUS, HEAD_Y, COLW_STATUS, 10, "Status", &opts, false);
        target->DrawText(COL_RIP, HEAD_Y, COLW_RIP, 10, "RIP", &opts, false);
        target->FillRect(2, ROW_Y - 2, W - 4, 1, dark, false);

        int rowW = n > VISIBLE_ROWS ? W - 4 - SCROLL_ROOM : W - 4;
        for (int r = 0; r < VISIBLE_ROWS && top + r < n; r++)
        {
            int i = top + r;
            const r2::TaskInfo &task = tasks[i];
            Coord ry = ROW_Y + r * ROW_H;
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

            target->DrawText(COL_PID, ry, COLW_PID, ROW_H - 1, (const mchar *)pidbuf, &opts, false);
            target->DrawText(COL_NAME, ry, COLW_NAME, ROW_H - 1, (const mchar *)namebuf, &opts, false);
            target->DrawText(COL_MODE, ry, COLW_MODE, ROW_H - 1, (const mchar *)modeStr(task.mode), &opts, false);
            target->DrawText(COL_STATUS, ry, COLW_STATUS, ROW_H - 1, (const mchar *)statusStr(task.status), &opts, false);
            target->DrawText(COL_RIP, ry, COLW_RIP, ROW_H - 1, (const mchar *)ripbuf, &opts, false);
        }
        scrollbar(target, ROW_Y, VISIBLE_ROWS * ROW_H - 1, top, VISIBLE_ROWS, n, W);

        // What a kill is waiting for or came to, else how to ask for one.
        opts.foreground = dark;
        target->DrawText(COL_PID, STATUS_Y, W - 2 * COL_PID, 10,
                         status[0] ? (const mchar *)status : "Del/K: kill the task on the bar   0-9 Enter: by PID", &opts,
                         false);
    }

    // ── The Memory tab ──────────────────────────────────────────────────────

    // Columns: PID, name, the private frame, what it holds on the user heap.
    static const int MCOL_PID = 6, MCOL_NAME = 30, MCOL_FRAME = 112, MCOL_HEAP = 214;
    static const int MEM_HEAD_Y = 38, MEM_ROW_Y = 49, MEM_ROWS = 7;

    void text(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, int w, const char *s)
    {
        target->DrawText(x, y, w, ROW_H - 1, (const mchar *)s, &opts, false);
    }

    void drawMemory(PlatformBitmap *target, PlatformDrawTextOptions &opts, int W)
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
            text(target, opts, 6, HEAD_Y, W - 12, "No memory information: this kernel is older than syscall 0x3C.");
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
        text(target, opts, 6, HEAD_Y, W - 12, line);
        line[0] = 0;
        append(line, sizeof line, "RAM ");
        appendU(line, sizeof line, m.total_ram / (1024 * 1024));
        append(line, sizeof line, " MiB");
        opts.horizontalAlign = PlatformAlign::End;
        text(target, opts, 6, HEAD_Y, W - 12, line);
        opts.horizontalAlign = PlatformAlign::Begin;

        const int barX = 6, barY = 14, barW = W - 12, barH = 7;
        target->FillRect(barX, barY, barW, barH, dark, false);
        target->FillRect(barX + 1, barY + 1, barW - 2, barH - 2, light, false);
        if (m.heap_size)
        {
            int used = (int)((unsigned long long)(barW - 2) * (m.heap_size - m.heap_free) / m.heap_size);
            target->FillRect(barX + 1, barY + 1, used, barH - 2, dark, false);
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
        text(target, opts, 6, 24, W - 12, line);

        // Each program: its frame and its share of the heap.  Kernel tasks run
        // on the kernel's own mappings and hold nothing of either, so they
        // are counted on the last line rather than listed.
        text(target, opts, MCOL_PID, MEM_HEAD_Y, 22, "PID");
        text(target, opts, MCOL_NAME, MEM_HEAD_Y, 80, "Name");
        text(target, opts, MCOL_FRAME, MEM_HEAD_Y, 100, "Frame (2 MiB)");
        text(target, opts, MCOL_HEAP, MEM_HEAD_Y, 70, "Heap held");
        target->FillRect(2, MEM_ROW_Y - 2, W - 4, 1, dark, false);

        // The rows are picked first, as there can be more than MEM_ROWS of
        // them with 32 slots, and the list shown from memTop.
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
        if (memTop > nRows - MEM_ROWS)
            memTop = nRows - MEM_ROWS;
        if (memTop < 0)
            memTop = 0;

        for (int row = 0; row < MEM_ROWS && memTop + row < nRows; row++)
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
                text(target, opts, MCOL_PID, y, 22, cell);
                char namebuf[17];
                nameStr(t->name, namebuf);
                compact(namebuf);
                text(target, opts, MCOL_NAME, y, 80, namebuf);
            }
            else
            {
                // Bytes tagged to a slot nobody is in: a program that died
                // without its blocks being swept yet --- or, for the last
                // row, blocks allocated with no owner at all.
                text(target, opts, MCOL_PID, y, 22, "-");
                text(target, opts, MCOL_NAME, y, 80, sl == r2::MaxSlots ? "(no owner)" : "(exited)");
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
            text(target, opts, MCOL_FRAME, y, 100, cell);
            cell[0] = 0;
            appendKiB(cell, sizeof cell, held);
            text(target, opts, MCOL_HEAP, y, 70, cell);
        }
        scrollbar(target, MEM_ROW_Y, MEM_ROWS * ROW_H - 1, memTop, MEM_ROWS, nRows, W);

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
        text(target, opts, 6, STATUS_Y, W - 12, line);
    }
};
