//
// Window — File manager, two panes  (ScListMounts 0x2C + ScListDirPath 0x2D)
//
// Laid out the way Norton Commander taught everyone to lay a file manager
// out: two directory panes side by side, one of them active, Tab between
// them, and a line of keys along the bottom. The point of the shape is that
// you can see where a file is and where it is going at the same time; this
// build has no copy syscall to move anything with, so what is here is the
// navigation half — but the shape is the same, and the second pane already
// earns its keep for comparing two directories.
//
// The top of the tree is the mount list rather than a directory. On this
// system "/" is the FAT12 floppy, not a root filesystem with the other mounts
// hanging off it, so there is no single directory that contains them all:
// going up from the top of a mount lands on a list of mounts, which is what
// Commander's drive bar does anyway.
//

class MountWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<MountWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;

    // ── Layout, in the window's own client coordinates ──────────────────────
    static const int PANE_W = 142;
    static const int PANE_X[2];
    static const int PANE_H = 126; // the box, including its border
    static const int HEAD_H = 10;  // the path line inside it
    static const int ROW_Y = 13;
    static const int ROW_H = 10;
    static const int VIS = 11; // 13 + 11*10 = 123, inside the box
    static const int NAME_X = 3, NAME_W = 92;
    static const int SIZE_X = 97, SIZE_W = 42;
    static const int INFO_Y = 129;
    static const int KEYS_Y = 139;

    static const int MAX_ENTRIES = 64;
    static const int MAX_MOUNTS = 8;

    // ── A pane ──────────────────────────────────────────────────────────────
    //
    // `atMounts` is the top of the tree: the pane is showing the mount list
    // rather than a directory. Everywhere else `path` is a directory and
    // `order` lists the entries in the order they are shown — directories
    // first, the way every file manager does it, without moving the entries
    // themselves about.
    struct Pane
    {
        char path[128];
        char mountRoot[40];
        bool atMounts;
        VfsDirEntry_T entries[MAX_ENTRIES];
        unsigned char order[MAX_ENTRIES];
        int nEntries;
        int sel;
        int scrollTop;
        bool stale;
    };

    Pane panes[2];
    int active = 0;

    MountInfo_T mounts[MAX_MOUNTS];
    int nMounts = 0;
    bool mountsStale = true;

    char scratch[160]; // path assembly, shared and short-lived

public:
    MountWindow()
    {
        for (int i = 0; i < 2; i++)
        {
            Pane &p = panes[i];
            p.path[0] = 0;
            p.mountRoot[0] = 0;
            p.atMounts = true;
            p.nEntries = 0;
            p.sel = 0;
            p.scrollTop = 0;
            p.stale = true;
        }
    }

private:
    // ── Small string helpers ────────────────────────────────────────────────
    static int slen(const char *s)
    {
        int i = 0;
        while (s[i])
            i++;
        return i;
    }

    static bool streq(const char *a, const char *b)
    {
        while (*a && *b && *a == *b)
        {
            a++;
            b++;
        }
        return *a == 0 && *b == 0;
    }

    static void u32str(unsigned int n, char *out)
    {
        if (!n)
        {
            out[0] = '0';
            out[1] = 0;
            return;
        }
        char t[12];
        int i = 0;
        while (n && i < 11)
        {
            t[i++] = (char)('0' + n % 10);
            n /= 10;
        }
        int at = 0;
        while (i--)
            out[at++] = t[i];
        out[at] = 0;
    }

    static void entryName(const VfsDirEntry_T &e, char *out, int outSize)
    {
        int n = e.name_len < 32 ? e.name_len : 32;
        if (n > outSize - 1)
            n = outSize - 1;
        int at = 0;
        for (int i = 0; i < n; i++)
        {
            unsigned char c = e.name[i];
            out[at++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out[at] = 0;
    }

    static const char *fsType(unsigned char t)
    {
        if (t == 1)
            return "rootfs";
        if (t == 2)
            return "fat12";
        if (t == 3)
            return "iso9660";
        return "none";
    }

    // ── Reading ─────────────────────────────────────────────────────────────
    void refreshMounts()
    {
        if (!mountsStale)
            return;
        int n = (int)list_mounts(mounts);
        if (n < 0)
            n = 0;
        if (n > MAX_MOUNTS)
            n = MAX_MOUNTS;
        nMounts = n;
        mountsStale = false;
    }

    // The filesystem is read when the pane changes directory, not on every
    // paint: a paint can happen for reasons that have nothing to do with this
    // window, and the floppy is not something to call at that rate.
    void refreshPane(Pane &p)
    {
        if (!p.stale)
            return;
        p.stale = false;

        if (p.atMounts)
        {
            refreshMounts();
            p.nEntries = 0;
            if (p.sel >= nMounts)
                p.sel = nMounts > 0 ? nMounts - 1 : 0;
            clampScroll(p, nMounts);
            return;
        }

        int raw = (int)list_dir_path((const unsigned char *)p.path, p.entries);
        if (raw < 0)
            raw = 0;
        if (raw > MAX_ENTRIES)
            raw = MAX_ENTRIES;

        // "." and ".." come back from the filesystem; the pane draws its own
        // parent row and does not want a second one.
        int kept = 0;
        for (int i = 0; i < raw; i++)
        {
            unsigned char nl = p.entries[i].name_len;
            if (nl == 1 && p.entries[i].name[0] == '.')
                continue;
            if (nl == 2 && p.entries[i].name[0] == '.' && p.entries[i].name[1] == '.')
                continue;
            if (kept != i)
                p.entries[kept] = p.entries[i];
            kept++;
        }
        p.nEntries = kept;

        // Directories first, files after, each keeping the order the
        // filesystem gave them.
        int at = 0;
        for (int i = 0; i < kept; i++)
            if (p.entries[i].is_dir)
                p.order[at++] = (unsigned char)i;
        for (int i = 0; i < kept; i++)
            if (!p.entries[i].is_dir)
                p.order[at++] = (unsigned char)i;

        int items = rowCount(p);
        if (p.sel >= items)
            p.sel = items > 0 ? items - 1 : 0;
        clampScroll(p, items);
    }

    // How many rows the pane shows: the mount list, or the parent row plus
    // the directory's entries.
    int rowCount(const Pane &p) const
    {
        if (p.atMounts)
            return nMounts;
        return 1 + p.nEntries;
    }

    static void clampScroll(Pane &p, int items)
    {
        if (p.sel < 0)
            p.sel = 0;
        if (p.sel >= items)
            p.sel = items > 0 ? items - 1 : 0;
        if (p.sel < p.scrollTop)
            p.scrollTop = p.sel;
        if (p.sel >= p.scrollTop + VIS)
            p.scrollTop = p.sel - VIS + 1;
        if (p.scrollTop > items - VIS)
            p.scrollTop = items - VIS;
        if (p.scrollTop < 0)
            p.scrollTop = 0;
    }

    // ── Navigation ──────────────────────────────────────────────────────────
    void enterMount(Pane &p, int mi)
    {
        refreshMounts();
        if (mi < 0 || mi >= nMounts)
            return;
        int nl = mounts[mi].path_len < 32 ? mounts[mi].path_len : 32;
        for (int i = 0; i < nl; i++)
            p.path[i] = p.mountRoot[i] = (char)mounts[mi].path[i];
        p.path[nl] = p.mountRoot[nl] = 0;
        if (nl == 0)
        {
            p.path[0] = p.mountRoot[0] = '/';
            p.path[1] = p.mountRoot[1] = 0;
        }
        p.atMounts = false;
        p.sel = 0;
        p.scrollTop = 0;
        p.stale = true;
    }

    void goUp(Pane &p)
    {
        if (p.atMounts)
            return;
        if (streq(p.path, p.mountRoot))
        {
            p.atMounts = true;
            p.sel = 0;
            p.scrollTop = 0;
            p.stale = true;
            return;
        }
        int i = slen(p.path) - 1;
        while (i > 0 && p.path[i] != '/')
            i--;
        if (i == 0)
            p.path[1] = 0;
        else
            p.path[i] = 0;
        p.sel = 0;
        p.scrollTop = 0;
        p.stale = true;
    }

    void goInto(Pane &p, int ei)
    {
        if (ei < 0 || ei >= p.nEntries || !p.entries[ei].is_dir)
            return;
        int cl = slen(p.path);
        int nl = p.entries[ei].name_len < 32 ? p.entries[ei].name_len : 32;
        if (cl + 1 + nl >= 127)
            return;
        if (cl == 1) // "/" already ends in a separator
        {
            for (int i = 0; i < nl; i++)
                p.path[1 + i] = (char)p.entries[ei].name[i];
            p.path[1 + nl] = 0;
        }
        else
        {
            p.path[cl] = '/';
            for (int i = 0; i < nl; i++)
                p.path[cl + 1 + i] = (char)p.entries[ei].name[i];
            p.path[cl + 1 + nl] = 0;
        }
        p.sel = 0;
        p.scrollTop = 0;
        p.stale = true;
    }

    // The full path of an entry, into `scratch`.
    const char *entryPath(Pane &p, int ei)
    {
        int cl = slen(p.path);
        int nl = p.entries[ei].name_len < 32 ? p.entries[ei].name_len : 32;
        int at = 0;
        for (int i = 0; i < cl && at < 150; i++)
            scratch[at++] = p.path[i];
        if (cl > 1 && at < 150)
            scratch[at++] = '/';
        for (int i = 0; i < nl && at < 150; i++)
            scratch[at++] = (char)p.entries[ei].name[i];
        scratch[at] = 0;
        return scratch;
    }

    // Enter, or a click on the row that is already selected: descend, go up,
    // or hand the file to the viewer, which opens over this window.
    void openSelection()
    {
        Pane &p = panes[active];
        refreshPane(p);

        if (p.atMounts)
        {
            enterMount(p, p.sel);
            wnd->Repaint();
            return;
        }
        if (p.sel == 0)
        {
            goUp(p);
            wnd->Repaint();
            return;
        }
        int ei = p.order[p.sel - 1];
        if (ei >= p.nEntries)
            return;
        if (p.entries[ei].is_dir)
        {
            goInto(p, ei);
            wnd->Repaint();
            return;
        }
        openFileViewer(entryPath(p, ei), p.entries[ei].size);
    }

    void moveSel(int delta)
    {
        Pane &p = panes[active];
        refreshPane(p);
        p.sel += delta;
        clampScroll(p, rowCount(p));
        wnd->Repaint();
    }

    void setActive(int which)
    {
        if (which == active)
            return;
        active = which;
        wnd->Repaint();
    }

    // ── Events ──────────────────────────────────────────────────────────────
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
            if (my < ROW_Y || my >= ROW_Y + VIS * ROW_H)
                return;

            int which = -1;
            for (int i = 0; i < 2; i++)
                if (mx >= PANE_X[i] && mx < PANE_X[i] + PANE_W)
                    which = i;
            if (which < 0)
                return;

            int row = (F_COORD(my) - ROW_Y) / ROW_H;
            Pane &p = panes[which];
            refreshPane(p);
            int item = p.scrollTop + row;
            if (item < 0 || item >= rowCount(p))
                return;

            // A click puts the pointer where it landed; a second click on the
            // same row is what opens it, which is the nearest thing to a
            // double click a ten-millisecond poll can tell apart.
            bool again = (which == active && item == p.sel);
            active = which;
            p.sel = item;
            clampScroll(p, rowCount(p));
            if (again)
                openSelection();
            else
                wnd->Repaint();
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
        if (key->isTab || key->isArrowLeft || key->isArrowRight)
        {
            setActive(active ^ 1);
            return;
        }
        if (key->isArrowUp)
        {
            moveSel(-1);
            return;
        }
        if (key->isArrowDown)
        {
            moveSel(1);
            return;
        }
        if (key->isPageUp)
        {
            moveSel(-VIS);
            return;
        }
        if (key->isPageDown)
        {
            moveSel(VIS);
            return;
        }
        if (key->isHome)
        {
            panes[active].sel = 0;
            clampScroll(panes[active], rowCount(panes[active]));
            wnd->Repaint();
            return;
        }
        if (key->isEnd)
        {
            panes[active].sel = rowCount(panes[active]) - 1;
            clampScroll(panes[active], rowCount(panes[active]));
            wnd->Repaint();
            return;
        }
        if (key->isEnter)
        {
            openSelection();
            return;
        }
        if (key->isBackspace)
        {
            goUp(panes[active]);
            wnd->Repaint();
            return;
        }
        // F3 views the file under the bar, as it does in every Commander.
        if (key->isF && key->f == 3)
        {
            Pane &p = panes[active];
            refreshPane(p);
            if (!p.atMounts && p.sel > 0)
            {
                int ei = p.order[p.sel - 1];
                if (ei < p.nEntries && !p.entries[ei].is_dir)
                    openFileViewer(entryPath(p, ei), p.entries[ei].size);
            }
            return;
        }
        // F5 re-reads both panes: the floppy can change underneath them.
        if (key->isF && key->f == 5)
        {
            mountsStale = true;
            panes[0].stale = true;
            panes[1].stale = true;
            wnd->Repaint();
        }
    }

    // ── Painting ────────────────────────────────────────────────────────────
    void drawRow(PlatformBitmap *target, int px, int y, const char *name, const char *size,
                 bool selected, bool paneActive, PlatformDrawTextOptions &opts)
    {
        if (selected)
        {
            if (paneActive)
            {
                target->FillRect(px + 1, y, PANE_W - 2, ROW_H - 1, dark, false);
                opts.foreground = light;
            }
            else
            {
                // The pane that is not in use still shows where its bar is,
                // as an outline: you need to know what Tab would land on.
                target->FillRect(px + 1, y, PANE_W - 2, 1, dark, false);
                target->FillRect(px + 1, y + ROW_H - 2, PANE_W - 2, 1, dark, false);
                target->FillRect(px + 1, y, 1, ROW_H - 1, dark, false);
                target->FillRect(px + PANE_W - 2, y, 1, ROW_H - 1, dark, false);
                opts.foreground = dark;
            }
        }
        else
        {
            opts.foreground = dark;
        }

        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(px + NAME_X, y, NAME_W, ROW_H - 1, (const mchar *)name, &opts, false);
        if (size)
            target->DrawText(px + SIZE_X, y, SIZE_W, ROW_H - 1, (const mchar *)size, &opts, false);
    }

    void drawPane(PlatformBitmap *target, int which, PlatformDrawTextOptions &opts)
    {
        Pane &p = panes[which];
        refreshPane(p);

        const int px = PANE_X[which];
        const bool isActive = (which == active);

        // The box, and the path along the top of it.
        target->FillRect(px, 0, PANE_W, PANE_H, dark, false);
        target->FillRect(px + 1, 1, PANE_W - 2, PANE_H - 2, light, false);

        if (isActive)
            target->FillRect(px + 1, 1, PANE_W - 2, HEAD_H, dark, false);
        opts.foreground = isActive ? light : dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        const char *title = p.atMounts ? "Mounts" : p.path;
        target->DrawText(px + NAME_X, 1, PANE_W - 6, HEAD_H, (const mchar *)title, &opts, false);
        target->FillRect(px + 1, 1 + HEAD_H, PANE_W - 2, 1, dark, false);

        int items = rowCount(p);
        for (int row = 0; row < VIS; row++)
        {
            int item = p.scrollTop + row;
            if (item >= items)
                break;
            int y = ROW_Y + row * ROW_H;
            bool sel = (item == p.sel);

            if (p.atMounts)
            {
                char name[40];
                int nl = mounts[item].path_len < 32 ? mounts[item].path_len : 32;
                int at = 0;
                for (int i = 0; i < nl; i++)
                    name[at++] = (char)mounts[item].path[i];
                name[at] = 0;
                if (at == 0)
                {
                    name[0] = '/';
                    name[1] = 0;
                }
                drawRow(target, px, y, name, fsType(mounts[item].fs_type), sel, isActive, opts);
                continue;
            }

            if (item == 0)
            {
                drawRow(target, px, y, "..", "<UP>", sel, isActive, opts);
                continue;
            }

            int ei = p.order[item - 1];
            if (ei >= p.nEntries)
                continue;
            char name[36];
            entryName(p.entries[ei], name, sizeof(name));
            char sizebuf[12];
            if (p.entries[ei].is_dir)
            {
                const char *d = "<DIR>";
                int i = 0;
                for (; d[i]; i++)
                    sizebuf[i] = d[i];
                sizebuf[i] = 0;
            }
            else
            {
                u32str(p.entries[ei].size, sizebuf);
            }
            drawRow(target, px, y, name, sizebuf, sel, isActive, opts);
        }

        // More below than fits: a mark in the bottom right of the box.
        if (items > p.scrollTop + VIS)
        {
            opts.foreground = dark;
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(px + PANE_W - 12, PANE_H - 11, 8, 9, "v", &opts, false);
        }
        if (p.scrollTop > 0)
        {
            opts.foreground = dark;
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(px + PANE_W - 12, 1 + HEAD_H + 1, 8, 9, "^", &opts, false);
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

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        drawPane(target, 0, opts);
        drawPane(target, 1, opts);

        // The full name of whatever the bar is on, which the name column is
        // too narrow to promise.
        Pane &p = panes[active];
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        const char *info = "";
        if (p.atMounts)
            info = "Mount points";
        else if (p.sel == 0)
            info = "Parent directory";
        else
        {
            int ei = p.order[p.sel - 1];
            if (ei < p.nEntries)
                info = entryPath(p, ei);
        }
        target->DrawText(2, INFO_Y, W - 4, 9, (const mchar *)info, &opts, false);

        // The key bar, reversed out the way a Commander does it.
        target->FillRect(0, KEYS_Y, W, 11, dark, false);
        opts.foreground = light;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(0, KEYS_Y, W, 11,
                         "Tab Pane   Enter Open   Bksp Up   F3 View   F5 Rescan   Esc Close",
                         &opts, false);
    }
};

const int MountWindow::PANE_X[2] = {0, 148};
