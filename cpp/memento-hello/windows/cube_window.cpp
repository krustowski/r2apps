// cube.elf renders into a shared block; this window owns the screen, input
// and allocation lifetime. Works on both the VGA and framebuffer desktops.
#include "../../../c/cube/winhost_lifetime.h"

class CubeWindow
{
public:
    static const int W = 224, H = 177;
    explicit CubeWindow(const char *path)
    {
        if (strlen(path) < sizeof(program) && !strchr(path, ' '))
            strcpy(program, path);
        else
            strcpy(status, "That path cannot be passed to cube.");
    }
    ~CubeWindow()
    {
        if (!block) return;
        cube_store(&block->quit, 1);
        for (int i = 0; i < 50 && !cube_load(&block->exited) && alive(); i++)
            r2::sleep(20);
        dispose();
    }
    static void onEvent(void *p, PlatformWindowInterfaceInputEvent *e)
    {
        reinterpret_cast<CubeWindow *>(p)->event(e);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; start(); }
    static void ReapClosed()
    {
        if (!CubeWinLifetime::pending()) return;
        static uint64_t last = 0;
        uint64_t now = r2::ticks();
        if (now - last < 1000) return;
        last = now;
        auto tasks = r2::tasks();
        CubeWinLifetime::collect(
            [&tasks](uint8_t id) { return taskAlive(id, tasks); },
            [](CubeWinLifetime::Allocation *a) { r2::heap::kernel_deallocate(a); });
    }
private:
    static const int STATUS_H = 9;
    PlatformWindow *wnd = nullptr;
    PlatformColor *black = nullptr, *bar = nullptr, *text = nullptr;
    PlatformFont *font = nullptr;
    CubeWinBlock *block = nullptr;
    char program[160] = {}, status[96] = {};
    uint8_t pid = 0, paletteMap[9] = {};
    uint32_t shown = 0;
    uint64_t lastCheck = 0;
    bool paused = false;

    static bool taskAlive(uint8_t id, const r2::vector<r2::TaskInfo> &tasks)
    {
        // Failure to read the table is not evidence that the child stopped.
        if (tasks.empty()) return true;
        for (const auto &task : tasks)
            if (task.id == id && task.status < 4 &&
                (task.name[0] | 0x20) == 'c' && (task.name[1] | 0x20) == 'u' &&
                (task.name[2] | 0x20) == 'b' && (task.name[3] | 0x20) == 'e' &&
                (task.name[4] == ' ' || task.name[4] == '.'))
                return true;
        return false;
    }
    bool alive() { return taskAlive(pid, r2::tasks()); }
    void dispose()
    {
        if (cube_load(&block->exited) || !alive())
            r2::heap::kernel_deallocate(block);
        else
            CubeWinLifetime::retire(block, pid);
        block = nullptr;
    }
    void start()
    {
        if (block || !program[0]) return;
        ReapClosed();
        block = (CubeWinBlock *)r2::heap::kernel_allocate(sizeof(CubeWinLifetime::Allocation));
        if (!block) { strcpy(status, "No memory for the cube's picture."); return; }
        block->magic = CUBEWIN_MAGIC;
        block->version = CUBEWIN_VERSION;
        block->reading = CUBEWIN_NONE;
        block->hostBeat = 1;
        char args[200] = "cube.elf --host 0x";
        size_t at = strlen(args);
        uint64_t address = (uint64_t)(uintptr_t)block;
        for (int shift = 60; shift >= 0; shift -= 4)
            args[at++] = "0123456789ABCDEF"[(address >> shift) & 15];
        args[at] = 0;
        // The ELF syscall accepts a bare 8.3 name. Launch from the file's
        // directory, then restore the shared working directory.
        r2::optional<uint8_t> id;
        const char *base = strrchr(program, '/');
        if (!base)
            id = r2::spawn(program, args);
        else
        {
            auto info = r2::sysinfo();
            char previous[33] = {}, directory[160];
            if (info)
            {
                memcpy(previous, info->system_path, 32);
                size_t length = (size_t)(base - program);
                memcpy(directory, program, length);
                directory[length] = 0;
                if (r2::fs::change_dir(directory))
                {
                    id = r2::spawn(base + 1, args);
                    (void)r2::fs::change_dir(previous);
                }
            }
        }
        if (!id)
        {
            r2::heap::kernel_deallocate(block);
            block = nullptr;
            strcpy(status, "Could not start cube.elf. Enter retries, Esc closes.");
            return;
        }
        pid = *id;
        paused = false;
        shown = 0;
        lastCheck = r2::ticks();
        status[0] = 0;
        // Find the closest desktop entry once for each of cube's nine colours.
        const uint8 *palette = MementoR2Impl::R2_Palette::Table();
        unsigned count = MementoR2Impl::R2_Palette::Count();
        for (int c = 0; c < 9; c++)
        {
            int best = 0x7fffffff;
            for (unsigned i = 0; i < count; i++)
            {
                int r = cube_palette[c][0] * 255 / 63 - palette[i * 3];
                int g = cube_palette[c][1] * 255 / 63 - palette[i * 3 + 1];
                int b = cube_palette[c][2] * 255 / 63 - palette[i * 3 + 2];
                int distance = 3 * r * r + 4 * g * g + 2 * b * b;
                if (distance < best) { best = distance; paletteMap[c] = (uint8_t)i; }
            }
        }
        wnd->SetImmediateMode(true);
        wnd->Repaint();
    }
    void idle()
    {
        if (!block) return;
        cube_store(&block->hostBeat, cube_load(&block->hostBeat) + 1);
        uint32_t frame = cube_load(&block->frame);
        if (frame != shown) { shown = frame; wnd->Repaint(); }
        uint64_t now = r2::ticks();
        if (!cube_load(&block->exited) && now - lastCheck < 1000) return;
        lastCheck = now;
        if (cube_load(&block->exited) || !alive())
        {
            dispose();
            strcpy(status, "Cube stopped. Enter restarts, Esc closes.");
            wnd->SetImmediateMode(false);
            wnd->Repaint();
        }
    }
    void paint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target) return;
        if (!black) black = dc->CreateColor(0xff000000, nullptr, nullptr);
        if (!bar) bar = dc->CreateColor(0xffaaaaaa, nullptr, nullptr);
        if (!text) text = dc->CreateColor(0xff000000, nullptr, nullptr);
        if (!font) font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!black || !bar || !text || !font) return;
        Coord wc = target->GetWidth(), hc = target->GetHeight();
        double width = COORD_VAL(wc), height = COORD_VAL(hc);
        target->FillRect(0, 0, Coord(width), Coord(height), black, false);
        if (block && cube_load(&block->frame))
        {
            auto *bitmap = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
            uint8 *pixels = bitmap->GetPixels();
            int bw = bitmap->GetRealWidth().intValue(), bh = bitmap->GetRealHeight().intValue();
            double scale = wnd->GetEffectiveDPI() > 0 ? wnd->GetEffectiveDPI() / 96.0 : 2;
            int aw = (int)(width * scale), ah = (int)((height - STATUS_H) * scale);
            if (aw > bw) aw = bw;
            if (ah > bh) ah = bh;
            // Mode 13h has rectangular pixels on a 4:3 monitor.
            int dw = aw, dh = aw * 3 / 4;
            if (dh > ah) { dh = ah; dw = ah * 4 / 3; }
            if (pixels && dw > 0 && dh > 0)
                cube_win_blit(block, pixels + ((ah - dh) / 2) * bw + (aw - dw) / 2,
                              bw, dw, dh, paletteMap);
        }
        target->FillRect(0, Coord(height - STATUS_H), Coord(width), Coord(STATUS_H), bar, false);
        PlatformDrawTextOptions options{};
        options.font = font;
        options.foreground = text;
        options.verticalAlign = PlatformAlign::Middle;
        options.horizontalAlign = PlatformAlign::Begin;
        const char *line = block ? (paused ? "Paused. Space resumes, Esc closes."
                                          : "Space pauses, Esc closes.")
                                 : (status[0] ? status : "Cube");
        target->DrawText(Coord(3), Coord(height - STATUS_H), Coord(width - 6), Coord(STATUS_H),
                         line, &options, false);
    }
    void event(PlatformWindowInterfaceInputEvent *e)
    {
        switch (e->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop: idle(); return;
        case PlatformWindowInputEventType::OnPaint:
            paint(e->Data.OnPaint.ctx, e->Data.OnPaint.target); return;
        case PlatformWindowInputEventType::OnKeyEvent:
        {
            auto *key = e->Data.OnKeyEvent.key;
            if (!key->isKeyDown) return;
            if (key->isEscape) wnd->Close();
            else if (block && key->isChar && key->theChar == ' ')
            {
                paused = !paused;
                cube_store(&block->paused, paused ? 1 : 0);
                wnd->Repaint();
            }
            else if (!block && key->isEnter) start();
            return;
        }
        default: return;
        }
    }
};
