//
// Clock window — analog clock using RTC
//

class ClockWindow
{
public:
    static void onEvent(void *inst, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<ClockWindow *>(inst)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    // sin(2π*i/60)*64, i=0..59; cos at position p = sin[(p+15)%60]
    static const signed char sin60[60];

    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformColor *face = nullptr;
    PlatformFont *font = nullptr;
    unsigned char lastSec = 0xFF;
    unsigned long startTick = 0; // r2::ticks() snapshot at OnCreate
    RTC_raw cachedRtc = {};      // RTC read once at OnCreate

    // Bresenham line on bitmap
    static void drawLine(PlatformBitmap *bm, PlatformColor *col,
                         int x0, int y0, int x1, int y1, int thick)
    {
        int dx = x1 > x0 ? x1 - x0 : x0 - x1;
        int dy = y1 > y0 ? y1 - y0 : y0 - y1;
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        for (int guard = 0; guard < 512; guard++)
        {
            bm->FillRect(x0, y0, thick, thick, col, false);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 > -dy)
            {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    static void drawHand(PlatformBitmap *bm, PlatformColor *col,
                         int pos, int len, int thick, int cx, int cy)
    {
        int dx = (int)sin60[pos] * len / 64;
        int dy = -(int)sin60[(pos + 15) % 60] * len / 64;
        drawLine(bm, col, cx, cy, cx + dx, cy + dy, thick);
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target)
            return;

        if (!dark)
            dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light)
            light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!face)
            face = dc->CreateColor(0xFFD0D0F8, nullptr, nullptr);
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light)
            return;

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        // The client area is the clock face and its readout; the root draws
        // the frame, the title bar and the taskbar entry.
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        // ── Clock face — centred across the client, the readout below it
        const int CX = F_COORD(W) / 2, CY = 42;
        const int R = 36;

        int fx = CX - R, fy = CY - R, fs = R * 2;
        target->FillRect(fx, fy, fs, fs, dark, false);                 // rim
        target->FillRect(fx + 2, fy + 2, fs - 4, fs - 4, face, false); // face fill
        // clip corners to look rounded
        target->FillRect(fx, fy, 5, 5, light, false);
        target->FillRect(fx + fs - 5, fy, 5, 5, light, false);
        target->FillRect(fx, fy + fs - 5, 5, 5, light, false);
        target->FillRect(fx + fs - 5, fy + fs - 5, 5, 5, light, false);

        // ── Hour tick marks
        for (int h = 0; h < 12; h++)
        {
            int p = h * 5;
            int tx = CX + (int)sin60[p] * (R - 8) / 64;
            int ty = CY - (int)sin60[(p + 15) % 60] * (R - 8) / 64;
            int tsz = (h % 3 == 0) ? 4 : 2;
            target->FillRect(tx - tsz / 2, ty - tsz / 2, tsz, tsz, dark, false);
        }

        // ── Derive current time from a single RTC snapshot + tick delta.
        // read_rtc is NOT called here; cachedRtc is filled once in OnCreate.
        unsigned long elapsed = r2::ticks() - startTick;
        unsigned long baseSec = (unsigned long)cachedRtc.hours * 3600u + (unsigned long)cachedRtc.minutes * 60u + (unsigned long)cachedRtc.seconds;
        unsigned long nowSec = baseSec + elapsed / 1000u;
        int sec = (int)(nowSec % 60);
        int min = (int)((nowSec / 60) % 60);
        int hr24 = (int)((nowSec / 3600) % 24);
        int hr = hr24 % 12;

        // ── Hands
        drawHand(target, dark, hr * 5 + min / 12, R - 20, 3, CX, CY); // hour
        drawHand(target, dark, min, R - 10, 2, CX, CY);               // minute
        drawHand(target, dark, sec, R - 4, 1, CX, CY);                // second
        target->FillRect(CX - 2, CY - 2, 5, 5, dark, false);          // pivot

        // ── Digital readout inside panel, below clock face
        if (font)
        {
            char tbuf[9] = {
                (char)('0' + hr24 / 10), (char)('0' + hr24 % 10), ':',
                (char)('0' + min / 10), (char)('0' + min % 10), ':',
                (char)('0' + sec / 10), (char)('0' + sec % 10), '\0'};
            target->DrawText(CX - 40, CY + R + 4, 80, 9, tbuf, &opts, false);
        }
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnCreate)
        {
            // Read RTC exactly once before IM mode starts (CPU not yet at 100%).
            // All subsequent time computation uses r2::ticks() deltas — no more
            // read_rtc calls during normal operation eliminates the UIP spin freeze.
            read_rtc(&cachedRtc);
            startTick = (unsigned long)r2::ticks();
            wnd->SetImmediateMode(true);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            unsigned long elapsed = (unsigned long)r2::ticks() - startTick;
            unsigned long baseSec = (unsigned long)cachedRtc.hours * 3600u + (unsigned long)cachedRtc.minutes * 60u + (unsigned long)cachedRtc.seconds;
            unsigned char curSec = (unsigned char)((baseSec + elapsed / 1000u) % 60u);
            if (curSec != lastSec)
            {
                lastSec = curSec;
                wnd->Repaint();
            }
            return;
        }
        // Moving the window and closing it are both the root's job now, so a
        // click in the face does nothing. It used to close the clock, which
        // was right when the clock filled the screen and wrong the moment it
        // became a window you might want to click on to bring forward.
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
            return;
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        if (key->isEscape)
        {
            wnd->SetImmediateMode(false);
            wnd->Close();
            return;
        }
    }
};

const signed char ClockWindow::sin60[60] = {
    0, 7, 13, 20, 26, 32, 38, 43, 48, 52,
    55, 58, 61, 63, 64, 64, 64, 63, 61, 58,
    55, 52, 48, 43, 38, 32, 26, 20, 13, 7,
    0, -7, -13, -20, -26, -32, -38, -43, -48, -52,
    -55, -58, -61, -63, -64, -64, -64, -63, -61, -58,
    -55, -52, -48, -43, -38, -32, -26, -20, -13, -7};
