//
// Window — Network Status  (ScNetStatus 0x38)
// Shows IP, MAC, driver state and bound TCP port registry.
//

class NetWindow
{
public:
    // Dialog metrics, in the window's own client coordinates — the root draws
    // the frame, the title bar and the taskbar entry. The widest thing here
    // is a MAC address, seventeen characters at four units each, and the
    // window is sized to that and not to the screen: 132 units across.
    static const int LABEL_X = 6, LABEL_W = 34;
    static const int VALUE_X = 42, VALUE_W = 84;
    static const int ROW1_Y = 4, ROW2_Y = 15, ROW3_Y = 26;
    // The ports go under their label, four to a row across the full width,
    // rather than beside it where they would force the window wider. The
    // kernel can report sixteen; the rest are simply not shown.
    static const int PORTS_SEP_Y = 38, PORTS_Y = 41, PORT_GRID_Y = 53;
    static const int PORT_ROWS = 2, PORT_COLS = 4, PORT_COL_W = 30;
    static const int BACK_W = 52, BACK_H = 11;

public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<NetWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    // What the last read returned, and whether another one is due.
    NetStatus_T ns{};
    SysInfo_T si{};
    bool statusStale = true;
    int backX = 0; // where the Back button was last drawn, for the hit test
    int backY = 0;

    static char hexNibble(unsigned char n) { return n < 10 ? '0' + n : 'a' + (n - 10); }

    static void byteToStr(unsigned char b, char *out)
    {
        if (b >= 100)
        {
            out[0] = '0' + b / 100;
            out[1] = '0' + (b / 10) % 10;
            out[2] = '0' + b % 10;
            out[3] = 0;
        }
        else if (b >= 10)
        {
            out[0] = '0' + b / 10;
            out[1] = '0' + b % 10;
            out[2] = 0;
        }
        else
        {
            out[0] = '0' + b;
            out[1] = 0;
        }
    }

    static void u16ToStr(unsigned short n, char *out)
    {
        if (!n)
        {
            out[0] = '0';
            out[1] = 0;
            return;
        }
        char t[6];
        int i = 0;
        while (n)
        {
            t[i++] = '0' + n % 10;
            n /= 10;
        }
        for (int j = 0; j < i; j++)
            out[j] = t[i - 1 - j];
        out[i] = 0;
    }

    // "a.b.c.d\0" — caller supplies buf[16]
    static void ipToStr(const unsigned char ip[4], char *buf)
    {
        int pos = 0;
        for (int i = 0; i < 4; i++)
        {
            char tmp[4];
            byteToStr(ip[i], tmp);
            for (int j = 0; tmp[j]; j++)
                buf[pos++] = tmp[j];
            if (i < 3)
                buf[pos++] = '.';
        }
        buf[pos] = 0;
    }

    // "aa:bb:cc:dd:ee:ff\0" — caller supplies buf[18]
    static void macToStr(const unsigned char mac[6], char *buf)
    {
        for (int i = 0; i < 6; i++)
        {
            buf[i * 3] = hexNibble(mac[i] >> 4);
            buf[i * 3 + 1] = hexNibble(mac[i] & 0xF);
            buf[i * 3 + 2] = (i < 5) ? ':' : 0;
        }
        buf[17] = 0;
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
            // Only the Back button closes it. Closing on any click made sense
            // when this filled the screen; now it has a title bar with a
            // close box on it, and clicking the body is how you focus it.
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            if (my >= backY && my < backY + BACK_H && mx >= backX && mx < backX + BACK_W)
                wnd->Close();
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        if (key->isEscape || key->isEnter)
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
        if (!font)
            font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font)
            return;

        // Read once and on request, not on every paint: with windows over a
        // live desktop a paint happens on every mouse move.
        if (statusStale)
        {
            memset(&ns, 0, sizeof(ns));
            get_net_status(&ns);
            memset(&si, 0, sizeof(si));
            read_sysinfo(&si);
            statusStale = false;
        }

        Coord W = target->GetWidth(), H = target->GetHeight();
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        // Section: Interface
        opts.horizontalAlign = PlatformAlign::Begin;

        // IP row — from sysinfo (set by ETH driver via ScSysInfo 0x02)
        char ipbuf[16];
        ipToStr(si.ip_addr, ipbuf);
        target->DrawText(LABEL_X, ROW1_Y, LABEL_W, 10, "IP:", &opts, false);
        target->DrawText(VALUE_X, ROW1_Y, VALUE_W, 10, (const mchar *)ipbuf, &opts, false);

        // MAC row
        char macbuf[18];
        macToStr(ns.mac, macbuf);
        target->DrawText(LABEL_X, ROW2_Y, LABEL_W, 10, "MAC:", &opts, false);
        target->DrawText(VALUE_X, ROW2_Y, VALUE_W, 10, (const mchar *)macbuf, &opts, false);

        // Driver row
        target->DrawText(LABEL_X, ROW3_Y, LABEL_W, 10, "Driver:", &opts, false);
        target->DrawText(VALUE_X, ROW3_Y, VALUE_W, 10,
                         ns.drv_active ? "Active" : "Inactive", &opts, false);

        // Separator before port table
        target->FillRect(2, PORTS_SEP_Y, W - 4, 1, dark, false);

        // Ports section header
        target->DrawText(LABEL_X, PORTS_Y, LABEL_W + 30, 10, "TCP ports:", &opts, false);

        if (ns.n_ports == 0)
        {
            target->DrawText(LABEL_X, PORT_GRID_Y, VALUE_W, 10, "none", &opts, false);
        }
        else
        {
            for (int i = 0; i < ns.n_ports && i < PORT_ROWS * PORT_COLS; i++)
            {
                int row = i / PORT_COLS, col = i % PORT_COLS;
                Coord ry = PORT_GRID_Y + row * 11;
                Coord rx = LABEL_X + col * PORT_COL_W;
                char pbuf[6];
                u16ToStr(ns.ports[i], pbuf);
                target->DrawText(rx, ry, PORT_COL_W - 2, 10, (const mchar *)pbuf, &opts, false);
            }
        }

        // Back button
        backX = (F_COORD(W) - BACK_W) / 2;
        backY = F_COORD(H) - BACK_H - 2;
        target->FillRect(2, backY - 3, W - 4, 1, dark, false);
        target->FillRect(backX, backY, BACK_W, BACK_H, dark, false);
        target->FillRect(backX + 1, backY + 1, BACK_W - 2, BACK_H - 2, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;
        target->DrawText(backX, backY, BACK_W, BACK_H, "Back", &opts, false);
    }
};
