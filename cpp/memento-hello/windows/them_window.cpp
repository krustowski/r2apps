//
// Window — theM: a DOS program in a window
//
// The emulator is c/them (them.elf in /mnt/tar/bin), a process of its own,
// started with the address of a ThemWinBlock this window allocates on the
// user heap --- the same arrangement as the Video window and mpegplay.  The
// emulator puts each picture in the block in the screen's colours; this
// window scales it into its own pixels at 4:3, the shape a DOS monitor gave
// every mode, and hands every key on as the scancode the keyboard sent.  The
// block's layout and the rules for reading it are in c/them/winhost.h.
//
// Opened from the file manager (Enter, or Alt+Space and "Run in theM", on an
// .EXE or .COM).  The program starts in its own directory, as it would have
// on DOS.
//
// Keys:  all of them go to the program.  F12 is the emulator's own: it ends
//        the program.  Once it has ended, Enter runs it again and Esc closes.
//        Alt+F makes the window as large as the screen allows.
//

#include "../../../c/them/winhost.h"

class ThemWindow
{
public:
    explicit ThemWindow(const char *path)
    {
        size_t n = strlen(path);
        if (n >= sizeof(prog))
            n = sizeof(prog) - 1;
        memcpy(prog, path, n);
        prog[n] = 0;
        const char *base = strrchr(prog, '/');
        base = base ? base + 1 : prog;
        strcpy(title, "theM: ");
        size_t at = strlen(title);
        for (; *base && at + 1 < sizeof(title); base++)
            title[at++] = *base;
        title[at] = 0;
    }

    ~ThemWindow()
    {
        if (!blk)
            return;
        //  Closed with the program running: the emulator is told to go, and
        //  given a moment to let go of the block before the block goes.
        blk->quit = 1;
        for (int i = 0; i < 100 && !blk->exited && emulatorAlive(); i++)
        {
            blk->hostBeat = blk->hostBeat + 1;
            r2::sleep(20);
        }
        if (blk->exited || !emulatorAlive())
            release();
        else
            blk = nullptr; // still holding it: left to it rather than freed under it
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<ThemWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        start();
    }

    const char *Title() const { return title; }

    //  A 320x200 picture at 1.4 pixels to its one, with the status line under
    //  it: fits the VGA desktop.  Alt+F gives it the whole screen.
    static const int W = 292, H = 178;

private:
    static const int STATUS_H = 9;

    PlatformWindow *wnd = nullptr;
    PlatformColor *cBlack = nullptr, *cBar = nullptr, *cText = nullptr;
    PlatformFont *font = nullptr;

    char prog[128] = {};
    char title[48] = {};
    char status[112] = {};

    ThemWinBlock *blk = nullptr;
    uint8_t pid = 0;
    uint64_t startedAt = 0, lastCheck = 0;
    uint32_t shownFrame = 0;
    bool running = false;
    int xs[2048]; // the source column of each window column, for blit()

    // ── Starting and ending ───────────────────────────────────────────────────

    void start()
    {
        if (blk)
            return;
        status[0] = 0;
        //  "them.elf --host 0xC1A2B0 /mnt/tar/games/dyna/DYNA.EXE"
        char args[200] = "them.elf --host 0x";
        size_t at = strlen(args);
        if (at + 16 + 1 + strlen(prog) + 1 > sizeof(args) || strchr(prog, ' '))
        {
            strcpy(status, "That path cannot be passed to the emulator.");
            return;
        }
        //  The kernel heap hands blocks back zeroed.
        blk = (ThemWinBlock *)r2::heap::kernel_allocate(sizeof(ThemWinBlock));
        if (!blk)
        {
            strcpy(status, "No memory for the picture (it takes 600 KiB).");
            return;
        }
        blk->magic = THEMWIN_MAGIC;
        blk->version = THEMWIN_VERSION;
        blk->reading = THEMWIN_NONE;
        blk->hostBeat = 1;
        blk->colours = (uint16_t)MementoR2Impl::R2_Palette::Count();

        uint64_t addr = (uint64_t)(uintptr_t)blk;
        for (int shift = 60; shift >= 0; shift -= 4)
            if ((addr >> shift) || shift < 32)
                args[at++] = "0123456789ABCDEF"[(addr >> shift) & 15];
        args[at++] = ' ';
        strcpy(args + at, prog);

        r2::optional<uint8_t> id = r2::spawn("them.elf", args);
        if (!id)
        {
            strcpy(status, "Could not start them.elf (is it in /mnt/tar/bin?)");
            release();
            return;
        }
        pid = *id;
        running = true;
        startedAt = r2::ticks();
        shownFrame = 0;
        wnd->SetImmediateMode(true);
        wnd->Repaint();
    }

    //  The emulator is gone: say how it went, and wait for Enter or Esc.
    void finished()
    {
        running = false;
        if (blk && blk->failed)
        {
            size_t i = 0;
            for (; i < sizeof(blk->message) && blk->message[i] && i < sizeof(status) - 1; i++)
                status[i] = blk->message[i];
            status[i] = 0;
        }
        else if (blk && blk->exited)
        {
            strcpy(status, "The program ended (code ");
            char n[8];
            u16str(blk->exitCode, n);
            web::scat(status, n, sizeof(status));
            web::scat(status, "). Enter runs it again, Esc closes.", sizeof(status));
        }
        else
            strcpy(status, "The emulator stopped. Enter runs it again, Esc closes.");
        if (blk && (blk->exited || !emulatorAlive()))
            release();
        else
            blk = nullptr;
        wnd->SetImmediateMode(false);
        wnd->Repaint();
    }

    void release()
    {
        ThemWinBlock *b = blk;
        blk = nullptr;
        r2::heap::kernel_deallocate(b);
    }

    static void u16str(unsigned v, char *out)
    {
        char t[8];
        int n = 0;
        do
            t[n++] = (char)('0' + v % 10);
        while ((v /= 10) && n < 7);
        for (int i = 0; i < n; i++)
            out[i] = t[n - 1 - i];
        out[n] = 0;
    }

    //  The emulator's task is still in the table.  A table that cannot be
    //  read just now counts as alive.
    bool emulatorAlive()
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (tasks.empty())
            return true;
        for (size_t i = 0; i < tasks.size(); i++)
            if (tasks[i].id == pid && tasks[i].status < 4 && (tasks[i].name[0] | 0x20) == 't' &&
                (tasks[i].name[1] | 0x20) == 'h')
                return true;
        return false;
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
        uint64_t now = r2::ticks();
        //  Once a second: has it gone?  It says so when it leaves properly;
        //  a crash shows only in the task table.
        if (now - lastCheck < 1000 || now - startedAt < 1500)
            return;
        lastCheck = now;
        if (blk->exited || !emulatorAlive())
            finished();
    }

    // ── Keys ──────────────────────────────────────────────────────────────────

    void onKey(PlatformKey *key)
    {
        if (running && blk)
        {
            //  As the keyboard sent it: presses and releases, prefix and all.
            uint8_t codes[2];
            int n = 0;
            if (key->scancodeExtended)
                codes[n++] = 0xE0;
            codes[n++] = key->scancode;
            if (!key->scancode)
                return;
            if (blk->keyHead - blk->keyTail + n > THEMWIN_KEYS)
                return; // it is not keeping up; a lost key beats a garbled ring
            for (int i = 0; i < n; i++)
            {
                blk->keys[blk->keyHead % THEMWIN_KEYS] = codes[i];
                __asm__ volatile("" ::: "memory");
                blk->keyHead = blk->keyHead + 1;
            }
            return;
        }
        if (!key->isKeyDown)
            return;
        if (key->isEscape)
            wnd->Close();
        else if (key->isEnter)
            start();
    }

    // ── Painting ──────────────────────────────────────────────────────────────

    //  The newest picture, scaled into the window's pixels at 4:3 and centred,
    //  read the way winhost.h says.
    void blit(PlatformBitmap *target, double Wd, double Hd)
    {
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
        uint8 *px = bm->GetPixels();
        int bw = bm->GetRealWidth().intValue(), bh = bm->GetRealHeight().intValue();
        //  Pixels a unit from the DPI: the bitmap is often wider than the
        //  window (Memento allocates in steps of 150 pixels).
        double s = wnd->GetEffectiveDPI() > 0 ? wnd->GetEffectiveDPI() / 96.0 : 2;
        int aw = (int)(Wd * s), ah = (int)((Hd - STATUS_H) * s);
        if (aw > bw)
            aw = bw;
        if (ah > bh)
            ah = bh;
        int dw = aw, dh = aw * 3 / 4;
        if (dh > ah)
        {
            dh = ah;
            dw = ah * 4 / 3;
        }
        if (!px || dw < 8 || dh < 8)
            return;
        int x0 = (aw - dw) / 2, y0 = (ah - dh) / 2;

        for (int tries = 0; tries < 4; tries++)
        {
            uint8_t f = blk->front;
            blk->reading = f;
            __asm__ volatile("" ::: "memory");
            if (blk->front != f)
                continue;
            int w = blk->width, h = blk->height;
            if (w > 0 && h > 0 && w <= THEMWIN_MAX_W && h <= THEMWIN_MAX_H)
            {
                const uint8_t *src = blk->pixels[f & 1];
                for (int dx = 0; dx < dw && dx < 2048; dx++)
                    xs[dx] = dx * w / dw;
                for (int dy = 0; dy < dh; dy++)
                {
                    const uint8_t *row = src + (size_t)(dy * h / dh) * w;
                    uint8 *out = px + (size_t)(y0 + dy) * bw + x0;
                    for (int dx = 0; dx < dw && dx < 2048; dx++)
                        out[dx] = row[xs[dx]];
                }
            }
            break;
        }
        blk->reading = THEMWIN_NONE;
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;
        if (!cBlack)
        {
            cBlack = dc->CreateColor(0xFF000000, nullptr, nullptr);
            cBar = dc->CreateColor(0xFFAAAAAA, nullptr, nullptr);
            cText = dc->CreateColor(0xFF000000, nullptr, nullptr);
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        }
        if (!cBlack || !font)
            return;
        Coord Wc = target->GetWidth(), Hc = target->GetHeight();
        double Wd = COORD_VAL(Wc), Hd = COORD_VAL(Hc);
        target->FillRect(0, 0, Coord(Wd), Coord(Hd - STATUS_H), cBlack, false);
        if (blk && running)
            blit(target, Wd, Hd);

        //  The status line: what runs, or how it ended.
        target->FillRect(0, Coord(Hd - STATUS_H), Coord(Wd), Coord(STATUS_H), cBar, false);
        char line[160];
        if (running && blk)
        {
            strcpy(line, prog);
            if (blk->width)
            {
                char n[8];
                web::scat(line, "  ", sizeof(line));
                u16str(blk->width, n);
                web::scat(line, n, sizeof(line));
                web::scat(line, "x", sizeof(line));
                u16str(blk->height, n);
                web::scat(line, n, sizeof(line));
                web::scat(line, blk->mode == 3 ? " text" : " graphics", sizeof(line));
            }
            web::scat(line, "  F12 ends it", sizeof(line));
        }
        else
            strcpy(line, status[0] ? status : prog);
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = cText;
        o.horizontalAlign = PlatformAlign::Begin;
        o.verticalAlign = PlatformAlign::Middle;
        target->DrawText(Coord(3), Coord(Hd - STATUS_H), Coord(Wd - 6), Coord(STATUS_H), (const mchar *)line, &o,
                         false);
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
};
