//
// Window — File manager, two panes  (ScListMounts 0x2C + ScListDirPath 0x2D)
//
// Laid out the way Norton Commander taught everyone to lay a file manager
// out: two directory panes side by side, one of them active, Tab between
// them, and a line of keys along the bottom. The point of the shape is that
// you can see where a file is and where it is going at the same time: the
// file operations work from the pane with the bar to the other one.
//
//      F5  Copy the file to the other pane's directory
//      F6  Move it there --- or, when both panes show the same directory,
//          rename it
//      F7  Make a directory in this pane's
//      F8  Delete the file, or the directory if it is empty
//      F2 or Ctrl+R  read both panes again
//      Alt+Space, Shift+F10 or a right click   the menu for the entry under
//          the bar: Run in theM (an .EXE or .COM), Set as wallpaper (a .PNG),
//          View, Edit, Copy, Move, Delete, and so on
//
// Enter on an .EXE or .COM runs it in theM, the DOS emulator, in a window of
// its own (windows/them_window.cpp); on anything else it opens the viewer.
//
// Only the floppy can be written; the CD is read-only, so from it Copy is the
// one operation there is.  Folders are not copied or moved (one file at a
// time), and a folder is deleted only when it is empty --- the kernel refuses
// the rest rather than take a folder's files with it.  A copy goes a few KiB
// at a time from the idle loop, with its progress on the info line, so the
// other windows keep going while the floppy works.
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
    //  Two boxes with a margin round them and a gap between, sharing the
    //  window's width; the info line and the key bar along the bottom, and the
    //  boxes down to them.  What is fixed is the shape of a row; how wide the
    //  boxes are and how many rows they show follow the window, so maximised
    //  it is a bigger Commander rather than a small one in a corner.  The
    //  numbers below are those of the window's own 290x150, which layout()
    //  works out again from the size at every paint.
    static const int MARGIN = 4, GAP = 6;
    static const int PANE_Y = 3;
    static const int HEAD_H = 10; // the path line inside a box
    static const int ROW_Y = PANE_Y + 13;
    static const int ROW_H = 10;
    static const int NAME_X = 3;
    static const int SIZE_W = 42;
    int paneW = 138;
    int paneX[2] = {MARGIN, MARGIN + 138 + GAP};
    int paneH = 123; // the box, including its border
    int vis = 10;    // rows a box shows: 16 + 10*10 = 116, inside the box
    int nameW = 88, sizeX = 92;
    int infoY = 129;
    int keysY = 139;

    void layout(int w, int h)
    {
        int oldVis = vis;

        paneW = (w - 2 * MARGIN - GAP) / 2;
        if (paneW < 60)
            paneW = 60;
        paneX[0] = MARGIN;
        paneX[1] = MARGIN + paneW + GAP;
        keysY = h - 11;
        infoY = keysY - 10;
        paneH = infoY - 3 - PANE_Y;
        vis = (PANE_Y + paneH - 3 - ROW_Y) / ROW_H;
        if (vis < 1)
            vis = 1;
        // The size keeps to the right of the box and the name has the rest.
        sizeX = paneW - SIZE_W - 4;
        nameW = sizeX - 4;

        // Fewer rows than before: the bar may have been left below them.
        if (vis != oldVis)
            for (int i = 0; i < 2; i++)
                clampScroll(panes[i], rowCount(panes[i]));
    }

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
        unsigned char fsType; // of the mount it is in: 1 rootfs, 2 fat12, 3 iso9660, 4 tar
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
    // Why the last F4 did not open the editor; shown on the info line until
    // the next key.
    char note[64] = {};

    MountInfo_T mounts[MAX_MOUNTS];
    int nMounts = 0;
    bool mountsStale = true;

    char scratch[160]; // path assembly, shared and short-lived

    // ── The context menu ────────────────────────────────────────────────────
    enum MenuCmd
    {
        M_RUN_THEM,
        M_WALLPAPER,
        M_OPEN,
        M_VIEW,
        M_EDIT,
        M_COPY,
        M_MOVE,
        M_MKDIR,
        M_DELETE,
        M_RESCAN,
    };
    struct MenuItem
    {
        MenuCmd cmd;
        const char *label;
        const char *keys;
        char accel; // the letter that picks it, underlined
    };
    static const int MENU_CAP = 10, MENU_ROW = 9, MENU_W = 96;
    MenuItem menu[MENU_CAP];
    int menuLen = 0, menuSel = 0;
    bool menuOpen = false;
    int menuX = 0, menuY = 0;

    // ── A file operation under way ──────────────────────────────────────────
    //
    // Normal, or waiting for Y (Confirm), or for a name (Prompt), or copying
    // (Busy: a few KiB per turn of the idle loop).
    enum Mode
    {
        NORMAL,
        CONFIRM,
        PROMPT,
        BUSY
    };
    enum Action
    {
        ACT_NONE,
        ACT_DELETE_FILE,
        ACT_DELETE_DIR,
        ACT_COPY,      // after "replace it?"
        ACT_MOVE,      // likewise
        ACT_MKDIR,
        ACT_RENAME,
    };
    Mode mode = NORMAL;
    Action action = ACT_NONE;
    char question[96] = {}; // what Confirm and Prompt are asking
    char input[13] = {};    // what has been typed at a Prompt: an 8.3 name
    // The operation's file: its directory and name, where it is going, and
    // how far a copy has got.
    char srcDir[128] = {}, srcName[16] = {};
    char dstDir[128] = {};
    char srcPath[160] = {}, dstPath[160] = {};
    unsigned int copySize = 0, copyDone = 0;
    bool moveAfter = false; // a move: the source goes once the copy is done
    uint8_t *copyBuf = nullptr;

public:
    ~MountWindow() { r2::heap::deallocate(copyBuf); }

    MountWindow()
    {
        for (int i = 0; i < 2; i++)
        {
            Pane &p = panes[i];
            p.path[0] = 0;
            p.mountRoot[0] = 0;
            p.atMounts = true;
            p.fsType = 0;
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
        if (t == 4)
            return "tar";
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

    void clampScroll(Pane &p, int items)
    {
        if (p.sel < 0)
            p.sel = 0;
        if (p.sel >= items)
            p.sel = items > 0 ? items - 1 : 0;
        if (p.sel < p.scrollTop)
            p.scrollTop = p.sel;
        if (p.sel >= p.scrollTop + vis)
            p.scrollTop = p.sel - vis + 1;
        if (p.scrollTop > items - vis)
            p.scrollTop = items - vis;
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
        p.fsType = mounts[mi].fs_type;
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

    // A DOS program theM can run: .EXE or .COM.
    static bool isProgram(const VfsDirEntry_T &e)
    {
        int n = e.name_len < 32 ? e.name_len : 32;
        if (e.is_dir || n < 5 || e.name[n - 4] != '.')
            return false;
        char x[3] = {(char)(e.name[n - 3] | 0x20), (char)(e.name[n - 2] | 0x20), (char)(e.name[n - 1] | 0x20)};
        return (x[0] == 'e' && x[1] == 'x' && x[2] == 'e') || (x[0] == 'c' && x[1] == 'o' && x[2] == 'm');
    }

    // A picture the wallpaper can be: .PNG.
    static bool isPng(const VfsDirEntry_T &e)
    {
        int n = e.name_len < 32 ? e.name_len : 32;
        if (e.is_dir || n < 5 || e.name[n - 4] != '.')
            return false;
        return (e.name[n - 3] | 0x20) == 'p' && (e.name[n - 2] | 0x20) == 'n' && (e.name[n - 1] | 0x20) == 'g';
    }

    // The picture over the whole screen, behind every window, from now on.
    void useAsWallpaper(const char *path)
    {
        const char *why = setWallpaper(path);
        if (why)
        {
            copyStr(note, sizeof(note), "Wallpaper: ");
            catStr(note, sizeof(note), why);
            return;
        }
        copyStr(note, sizeof(note), "Wallpaper set.");
        repaintDesktop();
    }

    void runInThem(const char *path)
    {
        if (!openThemWindow(path))
        {
            strncpy(note, "theM: no memory left for its window.", sizeof(note) - 1);
            wnd->Repaint();
        }
    }

    // ── The context menu ────────────────────────────────────────────────────

    void addItem(MenuCmd c, const char *label, const char *keys, char accel)
    {
        if (menuLen < MENU_CAP)
            menu[menuLen++] = {c, label, keys, accel};
    }

    //  For the entry under the bar in the active pane, next to it.
    void openMenu()
    {
        Pane &p = panes[active];
        refreshPane(p);
        menuLen = 0;
        if (p.atMounts || p.sel == 0)
            addItem(M_OPEN, "Open", "Enter", 'O');
        else
        {
            int ei = p.order[p.sel - 1];
            if (ei >= p.nEntries)
                return;
            const VfsDirEntry_T &e = p.entries[ei];
            if (e.is_dir)
                addItem(M_OPEN, "Open", "Enter", 'O');
            else
            {
                if (isProgram(e))
                    addItem(M_RUN_THEM, "Run in theM", "Enter", 'R');
                if (isPng(e))
                    addItem(M_WALLPAPER, "Set as wallpaper", "", 'W');
                addItem(M_VIEW, "View", "F3", 'V');
                addItem(M_EDIT, "Edit", "F4", 'E');
                addItem(M_COPY, "Copy", "F5", 'C');
                addItem(M_MOVE, "Move / rename", "F6", 'M');
            }
            addItem(M_DELETE, "Delete", "F8", 'D');
        }
        if (!p.atMounts)
            addItem(M_MKDIR, "Make directory", "F7", 'K');
        addItem(M_RESCAN, "Rescan", "F2", 'S');

        int row = p.sel - p.scrollTop;
        menuX = paneX[active] + 24;
        menuY = ROW_Y + (row + 1) * ROW_H;
        int h = menuLen * MENU_ROW + 4;
        if (menuY + h > keysY)
            menuY = ROW_Y + row * ROW_H - h;
        if (menuY < 1)
            menuY = 1;
        menuSel = 0;
        menuOpen = true;
        wnd->Repaint();
    }

    void closeMenu()
    {
        menuOpen = false;
        wnd->Repaint();
    }

    void pickMenu(int i)
    {
        if (i < 0 || i >= menuLen)
            return;
        MenuCmd c = menu[i].cmd;
        menuOpen = false;
        Pane &p = panes[active];
        int ei = (!p.atMounts && p.sel > 0) ? p.order[p.sel - 1] : -1;
        switch (c)
        {
        case M_RUN_THEM:
            if (ei >= 0 && ei < p.nEntries)
                runInThem(entryPath(p, ei));
            break;
        case M_WALLPAPER:
            if (ei >= 0 && ei < p.nEntries)
                useAsWallpaper(entryPath(p, ei));
            break;
        case M_OPEN:
            openSelection();
            break;
        case M_VIEW:
            if (ei >= 0 && ei < p.nEntries)
                openFileViewer(entryPath(p, ei), p.entries[ei].size);
            break;
        case M_EDIT:
            if (ei >= 0 && ei < p.nEntries && !openInEditor(entryPath(p, ei)))
                strncpy(note, g_launchError[0] ? g_launchError : "Editor: could not start.", sizeof(note) - 1);
            break;
        case M_COPY:
            startTransfer(false);
            break;
        case M_MOVE:
            startTransfer(true);
            break;
        case M_MKDIR:
            startMkdir();
            break;
        case M_DELETE:
            startDelete();
            break;
        case M_RESCAN:
            rescan();
            break;
        }
        wnd->Repaint();
    }

    //  The menu has the keys while it is open.
    void menuKey(PlatformKey *key)
    {
        if (key->isEscape || key->isTab || (key->isChar && key->theChar == ' ' && (key->isLeftAlt || key->isRightAlt)))
            closeMenu();
        else if (key->isArrowDown)
        {
            menuSel = (menuSel + 1) % menuLen;
            wnd->Repaint();
        }
        else if (key->isArrowUp)
        {
            menuSel = (menuSel + menuLen - 1) % menuLen;
            wnd->Repaint();
        }
        else if (key->isEnter)
            pickMenu(menuSel);
        else if (key->isChar)
        {
            char c = upper((char)key->theChar);
            for (int i = 0; i < menuLen; i++)
                if (menu[i].accel == c)
                {
                    pickMenu(i);
                    return;
                }
        }
    }

    int menuItemAt(int x, int y) const
    {
        if (!menuOpen || x < menuX || x >= menuX + MENU_W || y < menuY + 2)
            return -1;
        int i = (y - menuY - 2) / MENU_ROW;
        return i < menuLen ? i : -1;
    }

    void drawMenu(PlatformBitmap *target, PlatformDrawTextOptions &opts)
    {
        int h = menuLen * MENU_ROW + 4;
        target->FillRect(menuX + 1, menuY + 1, MENU_W, h, dark, false); // shadow
        target->FillRect(menuX, menuY, MENU_W, h, dark, false);
        target->FillRect(menuX + 1, menuY + 1, MENU_W - 2, h - 2, light, false);
        for (int i = 0; i < menuLen; i++)
        {
            int y = menuY + 2 + i * MENU_ROW;
            bool sel = i == menuSel;
            if (sel)
                target->FillRect(menuX + 1, y, MENU_W - 2, MENU_ROW, dark, false);
            opts.foreground = sel ? light : dark;
            opts.horizontalAlign = PlatformAlign::Begin;
            target->DrawText(menuX + 4, y, MENU_W - 8, MENU_ROW, (const mchar *)menu[i].label, &opts, false);
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(menuX + 4, y, MENU_W - 8, MENU_ROW, (const mchar *)menu[i].keys, &opts, false);
            //  Its letter, underlined.
            for (int k = 0; menu[i].label[k]; k++)
                if (upper(menu[i].label[k]) == menu[i].accel)
                {
                    target->FillRect(Coord(menuX + 4 + k * 3), Coord(y + MENU_ROW - 1.5), Coord(2.5), Coord(0.5),
                                     sel ? light : dark, false);
                    break;
                }
        }
        opts.horizontalAlign = PlatformAlign::Begin;
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
        if (isProgram(p.entries[ei]))
        {
            runInThem(entryPath(p, ei));
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

    // ── File operations ─────────────────────────────────────────────────────

    static void join(char *out, int cap, const char *dir, const char *name)
    {
        int at = 0;
        for (int i = 0; dir[i] && at < cap - 1; i++)
            out[at++] = dir[i];
        if (at > 0 && out[at - 1] != '/' && at < cap - 1)
            out[at++] = '/';
        for (int i = 0; name[i] && at < cap - 1; i++)
            out[at++] = name[i];
        out[at] = 0;
    }

    static void copyStr(char *out, int cap, const char *s)
    {
        int i = 0;
        for (; s[i] && i < cap - 1; i++)
            out[i] = s[i];
        out[i] = 0;
    }

    static void catStr(char *out, int cap, const char *s)
    {
        int at = slen(out);
        for (int i = 0; s[i] && at < cap - 1; i++)
            out[at++] = s[i];
        out[at] = 0;
    }

    static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

    static bool sameName(const char *a, const char *b)
    {
        while (*a && *b && upper(*a) == upper(*b))
        {
            a++;
            b++;
        }
        return !*a && !*b;
    }

    // The floppy can be written; the CD cannot.
    static bool writable(const Pane &p) { return !p.atMounts && (p.fsType == 1 || p.fsType == 2); }

    void say(const char *s)
    {
        copyStr(note, sizeof(note), s);
        wnd->Repaint();
    }

    // The entry under the active pane's bar, or -1 for "..", the mount list,
    // or nothing.
    int selectedEntry()
    {
        Pane &p = panes[active];
        refreshPane(p);
        if (p.atMounts || p.sel <= 0)
            return -1;
        int ei = p.order[p.sel - 1];
        return ei < p.nEntries ? ei : -1;
    }

    // Both panes read again: an operation changed what is on the disk.
    void rescan()
    {
        mountsStale = true;
        panes[0].stale = panes[1].stale = true;
    }

    // Looks `name` up in `p`'s listing (which has to be fresh).
    int findIn(Pane &p, const char *name)
    {
        refreshPane(p);
        for (int i = 0; i < p.nEntries; i++)
        {
            char n[36];
            entryName(p.entries[i], n, sizeof(n));
            if (sameName(n, name))
                return i;
        }
        return -1;
    }

    // Delete and rename name a file by itself, in the working directory: the
    // kernel does not resolve a path below the top of the floppy for them.
    bool inDir(const char *dir)
    {
        return r2::fs::change_dir(r2::string_view(dir, (size_t)slen(dir)));
    }
    static r2::string_view sv(const char *s) { return r2::string_view(s, (size_t)slen(s)); }

    // An 8.3 name: up to eight characters, an optional dot and up to three
    // more, of the ones FAT allows.  Typed in any case; stored in capitals.
    static bool valid83(const char *n)
    {
        int base = 0, ext = -1;
        if (!n[0] || n[0] == '.')
            return false;
        for (int i = 0; n[i]; i++)
        {
            char c = n[i];
            if (c == '.')
            {
                if (ext >= 0)
                    return false;
                ext = 0;
                continue;
            }
            bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~' ||
                      c == '!' || c == '#' || c == '$' || c == '&';
            if (!ok)
                return false;
            if (ext >= 0)
            {
                if (++ext > 3)
                    return false;
            }
            else if (++base > 8)
                return false;
        }
        return ext != 0; // "NAME." has an empty extension
    }

    void ask(Action a, const char *q)
    {
        action = a;
        mode = CONFIRM;
        copyStr(question, sizeof(question), q);
        wnd->Repaint();
    }

    void prompt(Action a, const char *q, const char *initial)
    {
        action = a;
        mode = PROMPT;
        copyStr(question, sizeof(question), q);
        copyStr(input, sizeof(input), initial);
        wnd->Repaint();
    }

    // F5 and F6: the file under the bar, to the other pane's directory.
    void startTransfer(bool move)
    {
        int ei = selectedEntry();
        if (ei < 0)
            return say(move ? "Move: put the bar on a file first." : "Copy: put the bar on a file first.");
        Pane &src = panes[active];
        Pane &dst = panes[active ^ 1];
        char name[36];
        entryName(src.entries[ei], name, sizeof(name));

        if (move && !dst.atMounts && streq(src.path, dst.path))
        {
            // Both panes on the same directory: F6 renames, as it does in a
            // Commander.
            if (!writable(src))
                return say("The CD is read-only: nothing on it can be renamed.");
            copyStr(srcDir, sizeof(srcDir), src.path);
            copyStr(srcName, sizeof(srcName), name);
            char q[96] = "Rename ";
            catStr(q, sizeof(q), name);
            catStr(q, sizeof(q), " to: ");
            return prompt(ACT_RENAME, q, name);
        }
        if (src.entries[ei].is_dir)
            return say(move ? "Folders cannot be moved yet: one file at a time."
                            : "Folders cannot be copied yet: one file at a time.");
        if (dst.atMounts)
            return say("Open a directory in the other pane first: that is where it goes.");
        if (!writable(dst))
            return say("The CD is read-only: copy onto the floppy instead.");
        if (move && !writable(src))
            return say("The CD is read-only: its files can be copied, not moved.");
        if (streq(src.path, dst.path))
            return say("That is the same directory: open another one in the other pane.");

        copyStr(srcDir, sizeof(srcDir), src.path);
        copyStr(srcName, sizeof(srcName), name);
        copyStr(dstDir, sizeof(dstDir), dst.path);
        join(srcPath, sizeof(srcPath), srcDir, srcName);
        join(dstPath, sizeof(dstPath), dstDir, srcName);
        copySize = src.entries[ei].size;
        moveAfter = move;

        int there = findIn(dst, name);
        if (there >= 0)
        {
            if (dst.entries[there].is_dir)
                return say("The other pane has a folder of that name.");
            char q[96] = "Replace ";
            catStr(q, sizeof(q), name);
            catStr(q, sizeof(q), " in ");
            catStr(q, sizeof(q), dstDir);
            catStr(q, sizeof(q), "?  Y / N");
            return ask(move ? ACT_MOVE : ACT_COPY, q);
        }
        beginCopy();
    }

    void beginCopy()
    {
        //  write_at() leaves what lies past what it writes, so a file being
        //  replaced goes first.
        if (inDir(dstDir))
            (void)r2::fs::remove(sv(srcName));
        if (copySize == 0)
        {
            rescan();
            return say("That file is empty: the kernel has no call that makes an empty file.");
        }
        if (!copyBuf)
            copyBuf = (uint8_t *)r2::heap::allocate(COPY_CHUNK);
        if (!copyBuf)
            return say("No memory for the copy.");
        copyDone = 0;
        mode = BUSY;
        //  The idle loop does the work, a chunk a turn.
        wnd->SetImmediateMode(true);
        progress();
    }

    static const unsigned int COPY_CHUNK = 4096;

    void progress()
    {
        char s[64];
        copyStr(s, sizeof(s), moveAfter ? "Moving " : "Copying ");
        catStr(s, sizeof(s), srcName);
        catStr(s, sizeof(s), ":  ");
        char n[12];
        u32str(copyDone / 1024, n);
        catStr(s, sizeof(s), n);
        catStr(s, sizeof(s), " / ");
        u32str((copySize + 1023) / 1024, n);
        catStr(s, sizeof(s), n);
        catStr(s, sizeof(s), " KiB");
        say(s);
    }

    void copyStep()
    {
        unsigned int want = copySize - copyDone < COPY_CHUNK ? copySize - copyDone : COPY_CHUNK;
        int64_t got = r2::fs::read_at(sv(srcPath), r2::byte_span(copyBuf, want), copyDone);
        int64_t put = got > 0 ? r2::fs::write_at(sv(dstPath), r2::const_byte_span(copyBuf, (size_t)got), copyDone) : -1;
        if (got <= 0 || put != got)
        {
            endBusy();
            rescan();
            return say(got <= 0 ? "Could not read the file: the copy stopped." : "Could not write: is the floppy full?");
        }
        copyDone += (unsigned int)got;
        if (copyDone < copySize)
            return progress();

        endBusy();
        rescan();
        if (moveAfter)
        {
            if (!inDir(srcDir) || !r2::fs::remove(sv(srcName)))
                return say("Copied, but the original could not be deleted.");
            char s[64];
            copyStr(s, sizeof(s), "Moved ");
            catStr(s, sizeof(s), srcName);
            return say(s);
        }
        char s[64];
        copyStr(s, sizeof(s), "Copied ");
        catStr(s, sizeof(s), srcName);
        say(s);
    }

    void endBusy()
    {
        mode = NORMAL;
        action = ACT_NONE;
        wnd->SetImmediateMode(false);
    }

    // F7: a new directory in the active pane's.
    void startMkdir()
    {
        Pane &p = panes[active];
        if (p.atMounts)
            return say("Open a directory first: the new one goes in it.");
        if (!writable(p))
            return say("The CD is read-only: make it on the floppy.");
        copyStr(dstDir, sizeof(dstDir), p.path);
        prompt(ACT_MKDIR, "New folder name: ", "");
    }

    // F8: the file, or the empty directory, under the bar.
    void startDelete()
    {
        int ei = selectedEntry();
        if (ei < 0)
            return say("Delete: put the bar on a file or a folder first.");
        Pane &p = panes[active];
        if (!writable(p))
            return say("The CD is read-only: nothing on it can be deleted.");
        char name[36];
        entryName(p.entries[ei], name, sizeof(name));
        copyStr(srcDir, sizeof(srcDir), p.path);
        copyStr(srcName, sizeof(srcName), name);
        char q[96];
        copyStr(q, sizeof(q), p.entries[ei].is_dir ? "Delete the folder " : "Delete ");
        catStr(q, sizeof(q), name);
        catStr(q, sizeof(q), "?  Y / N");
        ask(p.entries[ei].is_dir ? ACT_DELETE_DIR : ACT_DELETE_FILE, q);
    }

    // Y at a question, or Enter at a prompt.
    void carryOut()
    {
        Action a = action;
        mode = NORMAL;
        action = ACT_NONE;
        switch (a)
        {
        case ACT_DELETE_FILE:
        case ACT_DELETE_DIR:
        {
            bool ok = inDir(srcDir) &&
                      (a == ACT_DELETE_DIR ? r2::fs::remove_dir(sv(srcName)) : r2::fs::remove(sv(srcName)));
            if (ok && a == ACT_DELETE_DIR)
            {
                // A pane that was showing the folder, or a folder in it, goes
                // to where the folder was.
                char gone[160];
                join(gone, sizeof(gone), srcDir, srcName);
                int gl = slen(gone);
                for (Pane &q : panes)
                    if (!q.atMounts && !strncmp(q.path, gone, (size_t)gl) && (q.path[gl] == 0 || q.path[gl] == '/'))
                    {
                        copyStr(q.path, sizeof(q.path), srcDir);
                        q.sel = 0;
                        q.scrollTop = 0;
                    }
            }
            rescan();
            char s[64];
            if (ok)
            {
                copyStr(s, sizeof(s), "Deleted ");
                catStr(s, sizeof(s), srcName);
            }
            else
                copyStr(s, sizeof(s), a == ACT_DELETE_DIR ? "Not deleted: the folder is not empty."
                                                          : "Could not delete it.");
            return say(s);
        }
        case ACT_COPY:
        case ACT_MOVE:
            return beginCopy();
        case ACT_MKDIR:
        {
            if (!valid83(input))
                return say("Not a name the floppy can hold: up to 8 letters, a dot, 3 more.");
            if (findIn(panes[active], input) >= 0)
                return say("Something of that name is already there.");
            bool ok = r2::fs::make_dir(sv(dstDir), sv(input));
            rescan();
            char s[64];
            copyStr(s, sizeof(s), ok ? "Made " : "Could not make ");
            catStr(s, sizeof(s), input);
            return say(s);
        }
        case ACT_RENAME:
        {
            if (!valid83(input))
                return say("Not a name the floppy can hold: up to 8 letters, a dot, 3 more.");
            if (sameName(input, srcName))
                return say("");
            if (findIn(panes[active], input) >= 0)
                return say("Something of that name is already there.");
            bool ok = inDir(srcDir) && r2::fs::rename(sv(srcName), sv(input));
            rescan();
            char s[64];
            copyStr(s, sizeof(s), ok ? "Renamed to " : "Could not rename ");
            catStr(s, sizeof(s), ok ? input : srcName);
            return say(s);
        }
        default:
            return;
        }
    }

    // Keys while a question or a prompt is up.  Returns true when it took
    // the key.
    bool modalKey(PlatformKey *key)
    {
        if (mode == BUSY)
            return true; // the copy finishes first
        if (mode == CONFIRM)
        {
            char c = key->isChar ? upper((char)key->theChar) : 0;
            if (c == 'Y')
                carryOut();
            else
            {
                mode = NORMAL;
                action = ACT_NONE;
                say("Nothing done.");
            }
            return true;
        }
        if (mode == PROMPT)
        {
            int n = slen(input);
            if (key->isEscape)
            {
                mode = NORMAL;
                action = ACT_NONE;
                say("");
            }
            else if (key->isEnter)
                carryOut();
            else if (key->isBackspace)
            {
                if (n)
                    input[n - 1] = 0;
                wnd->Repaint();
            }
            else if (key->isChar && key->theChar > ' ' && key->theChar < 0x7F && n < 12)
            {
                input[n] = upper((char)key->theChar);
                input[n + 1] = 0;
                wnd->Repaint();
            }
            return true;
        }
        return false;
    }

    // ── Events ──────────────────────────────────────────────────────────────
    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            if (mode == BUSY)
                copyStep();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed || mode != NORMAL)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            bool right = data->Data.OnMouseClick.button == PlatformWindowMouseButton::Right;
            if (menuOpen)
            {
                int i = menuItemAt(F_COORD(mx), F_COORD(my));
                if (i >= 0)
                    pickMenu(i);
                else
                    closeMenu();
                return;
            }
            if (my < ROW_Y || my >= ROW_Y + vis * ROW_H)
                return;

            int which = -1;
            for (int i = 0; i < 2; i++)
                if (mx >= paneX[i] && mx < paneX[i] + paneW)
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
            if (right)
                openMenu();
            else if (again)
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
        if (modalKey(key))
            return;
        if (menuOpen)
        {
            menuKey(key);
            return;
        }
        if (note[0])
        {
            note[0] = 0;
            wnd->Repaint();
        }
        if ((key->isChar && key->theChar == ' ' && (key->isLeftAlt || key->isRightAlt)) ||
            (key->isF && key->f == 10 && (key->isLeftShift || key->isRightShift)))
        {
            openMenu();
            return;
        }

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
            moveSel(-vis);
            return;
        }
        if (key->isPageDown)
        {
            moveSel(vis);
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
        // F4 edits it, in Turbo C++, in an Editor window of its own beside
        // this one; when that cannot be made, the info line says why.
        if (key->isF && key->f == 4)
        {
            Pane &p = panes[active];
            refreshPane(p);
            if (!p.atMounts && p.sel > 0)
            {
                int ei = p.order[p.sel - 1];
                if (ei < p.nEntries && !p.entries[ei].is_dir && !openInEditor(entryPath(p, ei)))
                {
                    strncpy(note, g_launchError[0] ? g_launchError : "Editor: could not start.", sizeof(note) - 1);
                    wnd->Repaint();
                }
            }
            return;
        }
        // F2 or Ctrl+R reads both panes again: the floppy can change
        // underneath them.
        bool ctrl = key->isLeftControl || key->isRightControl;
        if ((key->isF && key->f == 2) || (ctrl && key->isChar && (key->theChar == 'r' || key->theChar == 'R')))
        {
            rescan();
            wnd->Repaint();
            return;
        }
        if (key->isF && key->f == 5)
            return startTransfer(false);
        if (key->isF && key->f == 6)
            return startTransfer(true);
        if (key->isF && key->f == 7)
            return startMkdir();
        if ((key->isF && key->f == 8) || key->isDelete)
            return startDelete();
    }

    // ── Painting ────────────────────────────────────────────────────────────
    void drawRow(PlatformBitmap *target, int px, int y, const char *name, const char *size,
                 bool selected, bool paneActive, PlatformDrawTextOptions &opts)
    {
        if (selected)
        {
            if (paneActive)
            {
                target->FillRect(px + 1, y, paneW - 2, ROW_H - 1, dark, false);
                opts.foreground = light;
            }
            else
            {
                // The pane that is not in use still shows where its bar is,
                // as an outline: you need to know what Tab would land on.
                target->FillRect(px + 1, y, paneW - 2, 1, dark, false);
                target->FillRect(px + 1, y + ROW_H - 2, paneW - 2, 1, dark, false);
                target->FillRect(px + 1, y, 1, ROW_H - 1, dark, false);
                target->FillRect(px + paneW - 2, y, 1, ROW_H - 1, dark, false);
                opts.foreground = dark;
            }
        }
        else
        {
            opts.foreground = dark;
        }

        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(px + NAME_X, y, nameW, ROW_H - 1, (const mchar *)name, &opts, false);
        if (size)
            target->DrawText(px + sizeX, y, SIZE_W, ROW_H - 1, (const mchar *)size, &opts, false);
    }

    void drawPane(PlatformBitmap *target, int which, PlatformDrawTextOptions &opts)
    {
        Pane &p = panes[which];
        refreshPane(p);

        const int px = paneX[which];
        const bool isActive = (which == active);

        // The box, and the path along the top of it.
        target->FillRect(px, PANE_Y, paneW, paneH, dark, false);
        target->FillRect(px + 1, PANE_Y + 1, paneW - 2, paneH - 2, light, false);

        if (isActive)
            target->FillRect(px + 1, PANE_Y + 1, paneW - 2, HEAD_H, dark, false);
        opts.foreground = isActive ? light : dark;
        opts.horizontalAlign = PlatformAlign::Begin;
        const char *title = p.atMounts ? "Mounts" : p.path;
        target->DrawText(px + NAME_X, PANE_Y + 1, paneW - 6, HEAD_H, (const mchar *)title, &opts, false);
        target->FillRect(px + 1, PANE_Y + 1 + HEAD_H, paneW - 2, 1, dark, false);

        int items = rowCount(p);
        for (int row = 0; row < vis; row++)
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
        if (items > p.scrollTop + vis)
        {
            opts.foreground = dark;
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(px + paneW - 12, PANE_Y + paneH - 11, 8, 9, "v", &opts, false);
        }
        if (p.scrollTop > 0)
        {
            opts.foreground = dark;
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(px + paneW - 12, PANE_Y + 1 + HEAD_H + 1, 8, 9, "^", &opts, false);
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
        layout(F_COORD(W), F_COORD(H));
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
        if (note[0])
            info = note;
        // A question, or a name being typed, takes the line over.
        char asking[112];
        if (mode == CONFIRM)
            info = question;
        else if (mode == PROMPT)
        {
            copyStr(asking, sizeof(asking), question);
            catStr(asking, sizeof(asking), input);
            catStr(asking, sizeof(asking), "_   Enter / Esc");
            info = asking;
        }
        if (mode == CONFIRM || mode == PROMPT)
        {
            target->FillRect(0, infoY - 1, W, 10, dark, false);
            opts.foreground = light;
        }
        target->DrawText(paneX[0], infoY, W - 2 * paneX[0], 9, (const mchar *)info, &opts, false);

        // The key bar, reversed out the way a Commander does it.
        target->FillRect(0, keysY, W, 11, dark, false);
        opts.foreground = light;
        opts.horizontalAlign = PlatformAlign::Middle;
        target->DrawText(0, keysY, W, 11,
                         "F2 Rescan F3 View F4 Edit F5 Copy F6 Move F7 MkDir F8 Del  Alt+Space Menu",
                         &opts, false);

        if (menuOpen)
            drawMenu(target, opts);
    }
};

