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
    r2::vector<r2::TaskInfo> tasks; // what the last read returned
    unsigned long lastRead = 0;
    bool haveTasks = false;
    int backX = 0; // where the Back button ended up, for the hit test
    int backY = 0;

    // Layout in the window's own client area — the root draws the frame, the
    // title bar and the line on the taskbar, so everything here starts at the
    // top left corner of the content.
    static const int HEAD_Y = 3;    // column headers
    static const int ROW_Y = 15;    // first task row
    static const int ROW_H = 10;    // one row
    static const int MAX_ROWS = 10; // the kernel's own limit on 0x2F
    static const int BACK_W = 80, BACK_H = 11;

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
            if (my >= backY && my < backY + BACK_H && mx >= backX && mx < backX + BACK_W)
            {
                wnd->Close();
                return;
            }
            // Task rows: a hit selects the row it landed in
            for (int i = 0; i < nLive; i++)
            {
                if (my >= ROW_Y + i * ROW_H && my < ROW_Y + i * ROW_H + (ROW_H - 1))
                {
                    sel = i;
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
        if (key->isEscape)
        {
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
            tasks = r2::tasks();
            lastRead = now;
            haveTasks = true;
        }
        int n = (int)tasks.size();
        if (n > MAX_ROWS)
            n = MAX_ROWS;
        nLive = n;
        if (sel > nLive)
            sel = nLive;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        // Column headers
        target->DrawText(COL_PID, HEAD_Y, COLW_PID, 10, "PID", &opts, false);
        target->DrawText(COL_NAME, HEAD_Y, COLW_NAME, 10, "Name", &opts, false);
        target->DrawText(COL_MODE, HEAD_Y, COLW_MODE, 10, "Mode", &opts, false);
        target->DrawText(COL_STATUS, HEAD_Y, COLW_STATUS, 10, "Status", &opts, false);
        target->DrawText(COL_RIP, HEAD_Y, COLW_RIP, 10, "RIP", &opts, false);
        target->FillRect(2, ROW_Y - 2, W - 4, 1, dark, false);

        for (int i = 0; i < n; i++)
        {
            const r2::TaskInfo &task = tasks[i];
            Coord ry = ROW_Y + i * ROW_H;
            if (sel == i)
            {
                target->FillRect(2, ry, W - 4, ROW_H - 1, dark, false);
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

        // Separator + Back button along the bottom of the client area
        backX = (F_COORD(W) - BACK_W) / 2;
        backY = F_COORD(H) - BACK_H - 2;
        target->FillRect(2, backY - 3, W - 4, 1, dark, false);
        bool backFocused = (sel == nLive);
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
    }
};
