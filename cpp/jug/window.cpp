//
//  window.cpp --- the Jug window: the server's list against what is here.
//
//  Drawn by jug.elf with Memento's software renderer and shown by Memento's
//  Jug window (hosted.cpp has the plumbing; this file is #included there).
//
//  One row a program: its name, size and SHA-256 as the list gives them,
//  where the copy that runs here is (jug: downloaded, tar/iso: shipped),
//  whether that copy is the server's build, and the tasks running it.
//
//  U fetches the list again (it is fetched when the window opens), G or
//  Enter downloads the program on the bar, A every program the server has a
//  newer build of, R restarts the running copies of the program on the bar
//  (Y to confirm), Del removes a download, Ctrl+C copies the SHA-256, and F
//  hides the programs that are current, or shows them again.
//  Tab / Shift+Tab cycle between the list and bottom buttons; Enter or Space
//  activates the focused button. Left / Right move between the buttons.
//
//  While it is open the window keeps looking: at the list every `check`
//  seconds of jug.cfg, and every few seconds at /mnt/tmp/jug, which the
//  console's jug may change.  Fresh builds a check finds are named on the
//  status line. Memento shows their count over the taskbar clock and marks
//  the window until it is looked at.
//

class JugWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<JugWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        wnd->SetTitle("Jug");
        if (!model.open())
            say("Cannot read the configuration; using ", model.config.repo);
        if (!jug::mounted("/mnt/tmp"))
            say("There is no RAM disk at /mnt/tmp: nothing can be downloaded.");
        startList();
    }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr, *light = nullptr, *warn = nullptr, *good = nullptr;
    PlatformFont *font = nullptr;

    jug::Model model;
    jug::Fetch fetch;

    enum Job
    {
        NO_JOB,
        LIST,
        PROGRAM,
    } job = NO_JOB;
    bool checking = false;                         // the LIST job is a check nobody asked for
    char current[jug::NAME_CAP] = {};              // the program being fetched
    r2::vector<r2::string> queue;                  // and those after it
    int sel = 0, top = 0;                          // into `view`
    char selName[jug::NAME_CAP] = {};              // the bar, across a new list or F
    char confirm[jug::NAME_CAP] = {};              // R pressed: Y restarts this
    bool mementoUpdated = false;                  // prompt after the download queue
    bool onlyUpdates = false;                      // F: the programs that are not current
    r2::vector<int> view;                          // the rows shown, as indices into model.rows
    r2::vector<r2::string> fresh;                  // what a refresh found new, named once read
    bool announce = false;
    bool checkedList = false;                     // first successful refresh reports existing updates too
    bool announceListChange = false;              // only automatic checks report changes without updates
    jug::Digest diskSeen;                          // /mnt/tmp/jug as this window left it
    bool restamp = true;                           // ... once the rows are read again
    char status[160] = {};
    char shownProgress[96] = {};
    uint64_t progressAt = 0, runningAt = 0, listAt = 0, diskAt = 0;

    static const uint64_t DISK_EVERY = 5000;       // ms between looks at /mnt/tmp/jug

    // Layout, in the window's units.  Text is 4 units a character.
    static const int TOP_Y = 2, HEAD_Y = 13, ROW_Y = 25, ROW_H = 10;
    static const int BTN_H = 11, BTN_W = 44, BTN_GAP = 4;
    static const int COL_NAME = 4, COL_SIZE = 42, COL_SUM = 82, COL_HERE = 154, COL_STATE = 176, COL_RUN = 214;
    enum Button
    {
        B_UPDATE,
        B_GET,
        B_ALL,
        B_RESTART,
        B_REMOVE,
        B_CLOSE,
        BUTTONS,
    };
    int btnY = 0, rowsShown = 1;
    int focusedButton = -1;                       // -1: the program list

    template <class... A> void say(const A &...parts)
    {
        status[0] = 0;
        const char *list[] = {parts...};
        for (const char *p : list)
            jug::scat(status, p, sizeof(status));
    }

    // Keep polling the task table even after hashing and downloads finish.
    void busy(bool) { wnd->SetImmediateMode(true); }

    // ── Jobs ────────────────────────────────────────────────────────────────

    //  `automatic`: a check, which keeps the status line unless it finds
    //  something, and gives way to anything the user asks for.
    void startList(bool automatic = false)
    {
        queue = r2::vector<r2::string>();
        if (!fetch.start(model.config.list, model.config.insecure))
        {
            listAt = r2::ticks();
            say(fetch.error());
            return;
        }
        job = LIST;
        checking = automatic;
        if (!automatic)
            say("Fetching ", model.config.list);
        busy(true);
    }

    //  Stops a check under way; the next one is due at once, so it runs
    //  again when the window is idle.
    void yieldCheck()
    {
        if (job != LIST || !checking)
            return;
        fetch.cancel();
        fetch.response().body.release();
        job = NO_JOB;
        checking = false;
    }

    bool checkDue(uint64_t now) const
    {
        return model.config.check && !confirm[0] && now - listAt >= (uint64_t)model.config.check * 1000;
    }

    bool startProgram(const char *name)
    {
        const jug::Row *r = model.row(name);
        const jug::Package *p = r ? model.package(*r) : nullptr;
        if (!p)
            return false;
        char url[jug::URL_CAP];
        model.config.urlOf(p->path, url, sizeof(url));
        if (!fetch.start(url, model.config.insecure))
        {
            say(name, ": ", fetch.error());
            return false;
        }
        job = PROGRAM;
        jug::scopy(current, name, sizeof(current));
        say("Downloading ", p->path);
        busy(true);
        return true;
    }

    void get(const jug::Row &r)
    {
        yieldCheck();
        if (job != NO_JOB)
        {
            say("Wait for ", job == LIST ? "the list" : current, " first.");
            return;
        }
        if (!model.package(r))
            say(r.name, " is no longer on the server.");
        else if (r.state == jug::State::Current)
            say(r.name, " is the server's build already.");
        else
            (void)startProgram(r.name);
    }

    void getAll()
    {
        yieldCheck();
        if (job != NO_JOB)
            return;
        if (model.count(jug::State::Unknown))
        {
            say("Finish reading the programs here before getting all updates.");
            return;
        }
        queue = r2::vector<r2::string>();
        for (const jug::Row &r : model.rows)
            if (r.state == jug::State::Outdated)
                (void)queue.push_back(r2::string(r.name));
        if (queue.empty())
        {
            say(model.count(jug::State::Unknown) ? "Still reading the programs here; a moment."
                                                 : "Everything here is the server's build.");
            return;
        }
        nextInQueue();
    }

    void nextInQueue()
    {
        while (!queue.empty())
        {
            r2::string name = queue[0];
            queue.erase(queue.begin());
            if (startProgram(name.c_str()))
                return;
        }
    }

    void finishJob()
    {
        Job done = job;
        job = NO_JOB;
        web::HttpResponse &resp = fetch.response();
        if (done == LIST)
        {
            bool automatic = checking, changed = true;
            checking = false;
            listAt = r2::ticks();
            jug::Catalog older;
            if (checkedList)
                older = model.catalog;
            if (!fetch.ok())
                say(automatic ? "Checking the list: " : "The list: ", fetch.error());
            else if (!model.takeList(resp.body.data, resp.body.len, resp.lastModified, automatic ? &changed : nullptr))
                say("That is no list of programs: ", model.config.list);
            else if (!changed && checkedList)
            {
                // the same list: nothing to say
            }
            else
            {
                restamp = true;
                //  Named once the programs here are read again. The first
                //  refresh compares against an empty list, even with a cache.
                fresh = model.catalog.freshSince(older);
                announce = true;
                announceListChange = automatic;
                checkedList = true;
                if (!automatic)
                {
                    char n[16] = {};
                    jug::scatU(n, model.catalog.packages.size(), sizeof(n));
                    say(n, " programs on the server.");
                }
                select(selName);
            }
            resp.body.release();
            return;
        }

        const jug::Row *r = model.row(current);
        const jug::Package *p = r ? model.package(*r) : nullptr;
        if (!fetch.ok())
            say(current, ": ", fetch.error());
        else if (!p)
            say(current, " left the list meanwhile.");
        else if (const char *why = jug::install(*p, resp.body.data, resp.body.len, model.registry))
            say(current, ": ", why);
        else
        {
            char size[16] = {};
            jug::scatSize(size, resp.body.len, sizeof(size));
            model.rescan();
            restamp = true;
            r = model.row(current);
            if (!strcmp(current, "memento") && r && r->npids)
                mementoUpdated = true;
            if (r && r->npids)
                say(current, ": ", size, " in /mnt/tmp/jug. Running: R restarts it.");
            else
                say(current, ": ", size, " in /mnt/tmp/jug; fg and bg start it.");
        }
        resp.body.release(); // the next download needs the room
        nextInQueue();
        if (job == NO_JOB && mementoUpdated)
        {
            mementoUpdated = false;
            select("memento"); // with F, the bar is on the next program shown
            askRestart(model.row("memento"));
        }
    }

    //  The builds a refresh found that this machine has not got.
    void tellFresh()
    {
        char names[64] = {};
        int n = 0;
        for (const r2::string &name : fresh)
            if (const jug::Row *r = model.row(name.view()))
                if (r->state == jug::State::Outdated || r->state == jug::State::Available)
                {
                    if (n++)
                        jug::scat(names, ", ", sizeof(names));
                    jug::scat(names, r->name, sizeof(names));
                }
        fresh = r2::vector<r2::string>();
        if (!n)
        {
            if (announceListChange)
                say("The list changed; nothing new for this machine.");
            return;
        }
        if (n > 4)
        {
            names[0] = 0;
            jug::scatU(names, (uint64_t)n, sizeof(names));
            jug::scat(names, " programs", sizeof(names));
        }
        say("Fresh on the server: ", names, ".  G gets one, A all updates.");
        notifyUpdates((uint32_t)n);
    }

    void onIdle()
    {
        uint64_t now = r2::ticks();
        if (job != NO_JOB)
        {
            if (!fetch.step())
            {
                finishJob();
                wnd->Repaint();
            }
            else if (now - progressAt >= 250 && strcmp(shownProgress, fetch.status()))
            {
                progressAt = now;
                wnd->Repaint();
            }
            return;
        }
        if (model.hashNext())
        {
            if (now - progressAt >= 250)
            {
                progressAt = now;
                wnd->Repaint();
            }
            return;
        }
        //  Everything here is read, and the registry saved.
        if (restamp)
        {
            restamp = false;
            diskSeen = jug::downloads_stamp();
            diskAt = now;
        }
        if (announce)
        {
            announce = false;
            tellFresh();
            wnd->Repaint();
        }
        //  Neither look talks over a Y / N question.
        if (!confirm[0] && now - diskAt >= DISK_EVERY)
        {
            diskAt = now;
            if (jug::downloads_stamp() != diskSeen)
            {
                model.reload();
                restamp = true;
                select(selName);
                say("Programs in /mnt/tmp/jug changed outside this window; reading them again.");
                wnd->Repaint();
                return;
            }
        }
        if (checkDue(now))
        {
            startList(true);
            wnd->Repaint();
            return;
        }
        if (now - runningAt >= 1000)
        {
            model.readRunning();
            runningAt = now;
            wnd->Repaint();
        }
    }

    // ── The bar ─────────────────────────────────────────────────────────────

    const jug::Row *selected() const
    {
        if (sel < 0 || sel >= (int)view.size() || view[sel] >= (int)model.rows.size())
            return nullptr;
        return &model.rows[view[sel]];
    }

    //  The rows to show: all, or with F those not current.  The bar stays
    //  on its program, or moves to the next one shown when that is hidden.
    //  Made again whenever the rows may have changed.
    void refreshView()
    {
        int want = -1;
        view.clear();
        for (size_t i = 0; i < model.rows.size(); i++)
        {
            if (!strcmp(model.rows[i].name, selName))
                want = (int)i;
            if (!onlyUpdates || model.rows[i].state != jug::State::Current)
                (void)view.push_back((int)i);
        }
        int at = sel;
        if (want >= 0)
        {
            at = (int)view.size() - 1;
            for (size_t i = 0; i < view.size(); i++)
                if (view[i] >= want)
                {
                    at = (int)i;
                    break;
                }
        }
        //  Scrolled only when the bar moved: the wheel leaves it behind.
        place(at, at != sel);
    }

    //  The bar on shown row `i`, scrolled into sight when `show`.
    void place(int i, bool show)
    {
        int n = (int)view.size();
        sel = i < 0 ? 0 : i >= n ? (n ? n - 1 : 0) : i;
        if (top > n - rowsShown)
            top = n - rowsShown;
        if (top < 0)
            top = 0;
        if (show && sel < top)
            top = sel;
        if (show && sel >= top + rowsShown)
            top = sel - rowsShown + 1;
        if (const jug::Row *r = selected())
            jug::scopy(selName, r->name, sizeof(selName));
    }

    void select(const char *name)
    {
        if (name != selName)
            jug::scopy(selName, name, sizeof(selName));
        refreshView();
        confirm[0] = 0;
    }

    void moveTo(int i)
    {
        place(i, true);
        confirm[0] = 0;
    }

    void askRestart(const jug::Row *r)
    {
        if (!r)
            return;
        model.readRunning();
        if (!r->npids)
        {
            say(r->name, " is not running.");
            return;
        }
        jug::scopy(confirm, r->name, sizeof(confirm));
        if (!strcmp(r->name, "memento"))
        {
            say("Relaunch Memento? Close all windows (save work first). Y / N");
            return;
        }
        char pids[24] = {};
        for (int i = 0; i < r->npids; i++)
        {
            jug::scat(pids, " ", sizeof(pids));
            jug::scatU(pids, r->pids[i], sizeof(pids));
        }
        say("Restart ", r->name, " (task", pids, ")?  Y / N");
    }

    void doRestart()
    {
        char name[jug::NAME_CAP];
        jug::scopy(name, confirm, sizeof(name));
        confirm[0] = 0;
        char msg[128];
        (void)jug::restart(name, true, msg, sizeof(msg));
        say(msg);
        model.readRunning();
        runningAt = r2::ticks();
    }

    void remove()
    {
        const jug::Row *r = selected();
        if (!r)
            return;
        char name[jug::NAME_CAP];
        jug::scopy(name, r->name, sizeof(name));
        if (const char *why = jug::uninstall(name, model.registry))
            say(name, ": ", why);
        else
        {
            model.rescan();
            restamp = true;
            say(name, ": download removed; the shipped copy runs again.");
            busy(true); // hash the copy that runs now
        }
        select(name);
    }

    void copySum()
    {
        const jug::Row *r = selected();
        const jug::Package *p = r ? model.package(*r) : nullptr;
        if (!p)
            return;
        char hex[65];
        p->sum.hex(hex);
        clipboardSet(hex);
        say("SHA-256 of ", r->name, " copied.");
    }

    void press(int b)
    {
        confirm[0] = 0;
        switch (b)
        {
        case B_UPDATE:
            if (job == NO_JOB)
                startList();
            else if (job == LIST && checking)
            {
                checking = false; // the user's own now: its outcome is told
                say("Fetching ", model.config.list);
            }
            break;
        case B_GET:
            if (const jug::Row *r = selected())
                get(*r);
            break;
        case B_ALL:
            getAll();
            break;
        case B_RESTART:
            askRestart(selected());
            break;
        case B_REMOVE:
            remove();
            break;
        case B_CLOSE:
            wnd->Close();
            return;
        }
        wnd->Repaint();
    }

    // ── Input ───────────────────────────────────────────────────────────────

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type != PlatformWindowInputEventType::OnImmediateModeIdleLoop &&
            data->type != PlatformWindowInputEventType::OnMouseMove)
            refreshView();
        switch (data->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop:
            onIdle();
            return;
        case PlatformWindowInputEventType::OnPaint:
            paint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        case PlatformWindowInputEventType::OnMouseWheel:
            top += data->Data.OnMouseWheel.up ? -3 : 3;
            if (top > (int)view.size() - rowsShown)
                top = (int)view.size() - rowsShown;
            if (top < 0)
                top = 0;
            wnd->Repaint();
            return;
        case PlatformWindowInputEventType::OnMouseClick:
            click(data->Data.OnMouseClick);
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            key(data->Data.OnKeyEvent.key);
            return;
        default:
            return;
        }
    }

    template <class Click> void click(const Click &c)
    {
        if (c.state != PlatformWindowButtonState::Pressed)
            return;
        Coord cx = c.mouseX, cy = c.mouseY;
        int x = F_COORD(cx), y = F_COORD(cy);
        if (y >= btnY && y < btnY + BTN_H)
        {
            int b = (x - 4) / (BTN_W + BTN_GAP);
            if (x >= 4 && b < BUTTONS && (x - 4) % (BTN_W + BTN_GAP) < BTN_W)
            {
                focusedButton = b;
                press(b);
            }
            return;
        }
        if (y >= ROW_Y && y < ROW_Y + rowsShown * ROW_H)
        {
            int i = top + (y - ROW_Y) / ROW_H;
            if (i < (int)view.size())
            {
                focusedButton = -1;
                moveTo(i);
                wnd->Repaint();
            }
        }
    }

    void key(PlatformKey *k)
    {
        if (!k->isKeyDown)
            return;
        char c = k->isChar ? (char)k->theChar : 0;
        bool ctrl = k->isLeftControl || k->isRightControl;
        if (k->isTab && !ctrl && !k->isLeftAlt && !k->isRightAlt)
        {
            if (confirm[0])
            {
                confirm[0] = 0;
                say("Not restarted.");
            }
            focusedButton += k->isLeftShift || k->isRightShift ? -1 : 1;
            if (focusedButton < -1)
                focusedButton = BUTTONS - 1;
            else if (focusedButton >= BUTTONS)
                focusedButton = -1;
            wnd->Repaint();
            return;
        }
        if (confirm[0])
        {
            if (c == 'y' || c == 'Y')
                doRestart();
            else
            {
                confirm[0] = 0;
                say("Not restarted.");
            }
            wnd->Repaint();
            return;
        }
        if (k->isArrowUp || k->isArrowDown || k->isPageUp || k->isPageDown || k->isHome || k->isEnd)
            focusedButton = -1;
        if (ctrl && (c == 'c' || c == 'C'))
            copySum();
        else if (k->isEscape)
        {
            if (job != NO_JOB)
            {
                if (job == LIST)
                    listAt = r2::ticks(); // a check waits its turn again
                queue = r2::vector<r2::string>();
                fetch.cancel();
                job = NO_JOB;
                checking = false;
                fetch.response().body.release();
                say("Stopped.");
            }
            else
            {
                wnd->Close();
                return;
            }
        }
        else if (focusedButton >= 0 && (k->isArrowLeft || k->isArrowRight))
            focusedButton = (focusedButton + (k->isArrowRight ? 1 : BUTTONS - 1)) % BUTTONS;
        else if (focusedButton >= 0 && (k->isEnter || k->isSpace || c == ' '))
        {
            press(focusedButton);
            return;
        }
        else if (k->isArrowUp)
            moveTo(sel - 1);
        else if (k->isArrowDown)
            moveTo(sel + 1);
        else if (k->isPageUp)
            moveTo(sel - rowsShown);
        else if (k->isPageDown)
            moveTo(sel + rowsShown);
        else if (k->isHome)
            moveTo(0);
        else if (k->isEnd)
            moveTo((int)view.size() - 1);
        else if (k->isEnter || c == 'g' || c == 'G')
        {
            press(B_GET);
            return;
        }
        else if (k->isDelete || c == 'x' || c == 'X')
        {
            press(B_REMOVE);
            return;
        }
        else if (c == 'u' || c == 'U')
        {
            press(B_UPDATE);
            return;
        }
        else if (c == 'a' || c == 'A')
        {
            press(B_ALL);
            return;
        }
        else if (c == 'r' || c == 'R')
        {
            press(B_RESTART);
            return;
        }
        else if ((c == 'f' || c == 'F') && !ctrl && !k->isLeftAlt && !k->isRightAlt) // Alt+F: Memento's maximise
        {
            onlyUpdates = !onlyUpdates;
            refreshView();
            say(onlyUpdates ? "Hiding the programs that are current; F shows them again." : "Showing every program.");
        }
        else
            return;
        wnd->Repaint();
    }

    // ── Painting ────────────────────────────────────────────────────────────

    void text(PlatformBitmap *t, PlatformDrawTextOptions &o, int x, int y, int w, const char *s)
    {
        t->DrawText(x, y, w, ROW_H - 1, (const mchar *)s, &o, false);
    }

    //  At the right edge, over whatever runs into it.
    void textRight(PlatformBitmap *t, PlatformDrawTextOptions &o, int y, int W, const char *s)
    {
        int w = (int)strlen(s) * 4;
        t->FillRect(W - 4 - w - 4, y, w + 4, ROW_H - 1, light, false);
        o.horizontalAlign = PlatformAlign::End;
        text(t, o, COL_NAME, y, W - 8, s);
        o.horizontalAlign = PlatformAlign::Begin;
    }

    void paint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!font)
        {
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
            warn = dc->CreateColor(0xFFAA0000, nullptr, nullptr);
            good = dc->CreateColor(0xFF008800, nullptr, nullptr);
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        }
        if (!dark || !light || !warn || !good || !font)
            return;

        uint64_t now = r2::ticks();
        if (now - runningAt >= 1000)
        {
            model.readRunning();
            runningAt = now;
        }

        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        int W = F_COORD(Wc), H = F_COORD(Hc);
        btnY = H - BTN_H - 2;
        int statusY = btnY - 14;
        rowsShown = (statusY - 2 - ROW_Y) / ROW_H;
        if (rowsShown < 1)
            rowsShown = 1;
        if (sel >= top + rowsShown)
            top = sel - rowsShown + 1;

        target->FillRect(0, 0, Wc, Hc, light, false);
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = dark;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Middle;

        // Which list, and how old it is.
        char line[160] = "List ";
        jug::scat(line, model.config.list, sizeof(line));
        text(target, o, COL_NAME, TOP_Y, W - 8, line);
        if (model.catalog.updated[0])
        {
            jug::scopy(line, "updated ", sizeof(line));
            jug::scat(line, model.catalog.updated, sizeof(line));
            textRight(target, o, TOP_Y, W, line);
        }

        text(target, o, COL_NAME, HEAD_Y, 36, "Name");
        text(target, o, COL_SIZE, HEAD_Y, 38, "Size");
        text(target, o, COL_SUM, HEAD_Y, 70, "SHA-256");
        text(target, o, COL_HERE, HEAD_Y, 22, "Here");
        text(target, o, COL_STATE, HEAD_Y, 36, "State");
        text(target, o, COL_RUN, HEAD_Y, W - COL_RUN - 4, "Running");
        if (onlyUpdates)
        {
            jug::scopy(line, "", sizeof(line));
            jug::scatU(line, (uint64_t)model.count(jug::State::Current), sizeof(line));
            jug::scat(line, " hidden", sizeof(line));
            textRight(target, o, HEAD_Y, W, line);
        }
        target->FillRect(2, ROW_Y - 2, W - 4, 1, dark, false);

        if (view.empty())
            text(target, o, COL_NAME, ROW_Y, W - 8,
                 !model.rows.empty() ? "Every program here is the server's build; F shows them."
                 : model.haveList    ? "The server lists no programs."
                                     : "No list yet.");
        for (int i = 0; i < rowsShown && top + i < (int)view.size(); i++)
            drawRow(target, o, model.rows[view[top + i]], ROW_Y + i * ROW_H, W, top + i == sel);

        // Where the work is, or what came of it.
        o.foreground = dark;
        const char *what = status;
        if (job != NO_JOB)
        {
            jug::scopy(shownProgress, fetch.status(), sizeof(shownProgress));
            jug::scopy(line, job == PROGRAM ? current : checking ? "Checking the list: " : "The list: ", sizeof(line));
            if (job == PROGRAM)
                jug::scat(line, ": ", sizeof(line));
            jug::scat(line, shownProgress, sizeof(line));
            what = line;
        }
        else if (int unknown = model.count(jug::State::Unknown))
        {
            jug::scopy(line, "Reading the programs here... ", sizeof(line));
            jug::scatU(line, (uint64_t)unknown, sizeof(line));
            jug::scat(line, " to go", sizeof(line));
            what = line;
        }
        target->FillRect(2, statusY - 2, W - 4, 1, dark, false);
        text(target, o, COL_NAME, statusY, W - 8, what);

        static const char *const labels[BUTTONS] = {"Update", "Get", "Get all", "Restart", "Remove", "Close"};
        o.horizontalAlign = PlatformAlign::Middle;
        for (int b = 0; b < BUTTONS; b++)
        {
            int x = 4 + b * (BTN_W + BTN_GAP);
            target->FillRect(x, btnY, BTN_W, BTN_H, dark, false);
            target->FillRect(x + 1, btnY + 1, BTN_W - 2, BTN_H - 2, b == focusedButton ? dark : light, false);
            o.foreground = b == focusedButton ? light : dark;
            target->DrawText(x, btnY, BTN_W, BTN_H, (const mchar *)labels[b], &o, false);
        }
    }

    void drawRow(PlatformBitmap *t, PlatformDrawTextOptions &o, const jug::Row &r, int y, int W, bool bar)
    {
        if (bar)
            t->FillRect(2, y, W - 4, ROW_H - 1, dark, false);
        o.foreground = bar ? light : dark;
        const jug::Package *p = model.package(r);
        const jug::Local *l = model.local(r);

        char cell[48] = {};
        if (p && p->size >= 0)
            jug::scatSize(cell, (uint64_t)p->size, sizeof(cell));
        else if (l)
            jug::scatSize(cell, l->size, sizeof(cell));
        text(t, o, COL_NAME, y, 36, r.name);
        text(t, o, COL_SIZE, y, 38, cell);
        if (p)
        {
            p->sum.hex(cell, 16);
            text(t, o, COL_SUM, y, 70, cell);
        }
        text(t, o, COL_HERE, y, 22, l ? jug::origin_name(l->origin) : "-");

        if (!bar && (r.state == jug::State::Outdated || r.state == jug::State::Gone))
            o.foreground = warn;
        else if (!bar && r.state == jug::State::Available)
            o.foreground = good;
        text(t, o, COL_STATE, y, 36, r.unreadable ? "unread" : jug::state_name(r.state));
        o.foreground = bar ? light : dark;

        cell[0] = 0;
        for (int i = 0; i < r.npids; i++)
        {
            if (i)
                jug::scat(cell, " ", sizeof(cell));
            jug::scatU(cell, r.pids[i], sizeof(cell));
        }
        text(t, o, COL_RUN, y, W - COL_RUN - 4, cell[0] ? cell : "-");
    }
};
