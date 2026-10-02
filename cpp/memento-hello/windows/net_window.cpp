//
// Window — Network Status  (ScNetStatus 0x38)
// Shows IP, MAC, driver state and bound TCP port registry, and beside them
// c/nsk's host discovery (nsk.cpp): a subnet to sweep and the computers on it
// that answered.
//

#include "../nsk.h"

class NetWindow
{
public:
    // Dialog metrics, in the window's own client coordinates — the root draws
    // the frame, the title bar and the taskbar entry. The widest thing here
    // is a MAC address, seventeen characters at four units each, and the
    // status pane is sized to that: 132 units across.
    static const int LABEL_X = 6, LABEL_W = 34;
    static const int VALUE_X = 42, VALUE_W = 84;
    static const int ROW1_Y = 4, ROW2_Y = 15, ROW3_Y = 26;
    // The ports go under their label, as many to a row as the width takes
    // (four in the window's own size), rather than beside it where they would
    // force the window wider. The kernel reports up to sixteen, and the
    // window is tall enough for four rows of them; maximised, it lays them
    // out wider. If they still do not fit, the last cell says how many more.
    static const int PORTS_SEP_Y = 38, PORTS_Y = 41, PORT_GRID_Y = 53;
    static const int PORT_ROW_H = 11, PORT_COL_W = 30;
    static const int BACK_W = 52, BACK_H = 11;

    // The scan pane, right of the status: a subnet field and a button on top,
    // a line of progress under them, and a row per live host --- address
    // (fifteen characters), MAC (seventeen) and how it answered with its
    // round trip, which is 166 units across.
    static const int PANE = 132; // where the status ends and the scan begins
    static const int SCAN_X = PANE + 4;
    static const int FIELD_Y = 3, FIELD_H = 11;
    static const int BTN_W = 34;
    static const int SCAN_STATUS_Y = 16;
    static const int LIST_SEP_Y = 28, LIST_Y = 31, LIST_ROW_H = 10;
    static const int COL_MAC = 62, COL_HOW = 134;
    static const int W = SCAN_X + 166 + 4;
    static const int H = 150; // six rows of ports, eight hosts, a Back button
    static const int CIDR_CAP = 19;

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

    HostScan scan;
    char cidr[CIDR_CAP + 1] = {};
    int cidrLen = 0;
    bool cidrSuggested = false;
    int listTop = 0;  // the first live host shown
    int listRows = 1; // how many fit, from the last paint
    int btnX = 0;     // the Scan/Stop button, from the last paint
    bool idleOn = false;
    uint64_t lastRepaint = 0;

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

    void setIdle(bool on)
    {
        if (on == idleOn || !wnd)
            return;
        wnd->SetImmediateMode(on);
        idleOn = on;
    }

    void startOrStop()
    {
        if (scan.busy())
        {
            scan.stop();
            setIdle(false);
        }
        else
        {
            listTop = 0;
            //  A failed start has its reason to show; either way the driver
            //  line may have changed (the scan takes the NIC when it is free).
            if (scan.start(cidr))
                setIdle(true);
            statusStale = true;
        }
        wnd->Repaint();
    }

    void onIdle()
    {
        bool changed = scan.step();
        if (!scan.busy())
        {
            setIdle(false);
            wnd->Repaint();
            return;
        }
        //  A repaint is time the network is not read: a few a second.
        uint64_t now = r2::ticks();
        if (changed && now - lastRepaint >= 250)
        {
            lastRepaint = now;
            wnd->Repaint();
        }
    }

    // The n-th live host, as an index into the scan's range; -1 past the end.
    int liveAt(int n) const
    {
        for (int i = 0; i < scan.count(); i++)
            if (scan.isUp(i) && n-- == 0)
                return i;
        return -1;
    }

    void scrollBy(int d)
    {
        int maxTop = scan.up() - listRows;
        listTop += d;
        if (listTop > maxTop)
            listTop = maxTop;
        if (listTop < 0)
            listTop = 0;
        wnd->Repaint();
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            onIdle();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseWheel)
        {
            scrollBy(data->Data.OnMouseWheel.up ? -3 : 3);
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
            else if (my >= FIELD_Y && my < FIELD_Y + FIELD_H && mx >= btnX && mx < btnX + BTN_W)
                startOrStop();
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;
        // Esc closes; Enter scans the subnet in the field (or stops a scan),
        // and what is typed goes into the field.
        if (key->isEscape)
            wnd->Close();
        else if (key->isEnter)
            startOrStop();
        else if (key->isArrowUp)
            scrollBy(-1);
        else if (key->isArrowDown)
            scrollBy(1);
        else if (key->isPageUp)
            scrollBy(-listRows);
        else if (key->isPageDown)
            scrollBy(listRows);
        else if (key->isBackspace)
        {
            if (cidrLen > 0)
                cidr[--cidrLen] = 0;
            wnd->Repaint();
        }
        else if (key->isChar && cidrLen < CIDR_CAP)
        {
            char c = (char)key->theChar;
            if ((c >= '0' && c <= '9') || c == '.' || c == '/')
            {
                cidr[cidrLen++] = c;
                cidr[cidrLen] = 0;
                wnd->Repaint();
            }
        }
    }

    // A button: dark frame, light face, the label centred.
    void button(PlatformBitmap *target, PlatformDrawTextOptions *opts, int x, int y, int w, int h, const char *label)
    {
        target->FillRect(x, y, w, h, dark, false);
        target->FillRect(x + 1, y + 1, w - 2, h - 2, light, false);
        opts->horizontalAlign = PlatformAlign::Middle;
        target->DrawText(x, y, w, h, (const mchar *)label, opts, false);
        opts->horizontalAlign = PlatformAlign::Begin;
    }

    // The scan pane: from SCAN_X to the right edge, from the top to `bottom`.
    void PaintScan(PlatformBitmap *target, PlatformDrawTextOptions *opts, int fullW, int bottom)
    {
        target->FillRect(PANE, 2, 1, bottom - 2, dark, false);
        opts->horizontalAlign = PlatformAlign::Begin;
        opts->verticalAlign = PlatformAlign::Middle;

        // The subnet field, with a caret, and the button beside it.
        btnX = fullW - 4 - BTN_W;
        int fieldW = btnX - 3 - SCAN_X;
        target->FillRect(SCAN_X, FIELD_Y, fieldW, FIELD_H, dark, false);
        target->FillRect(SCAN_X + 1, FIELD_Y + 1, fieldW - 2, FIELD_H - 2, light, false);
        char field[CIDR_CAP + 2];
        strcpy(field, cidr);
        if (!scan.busy())
            strcpy(field + strlen(field), "_");
        target->DrawText(SCAN_X + 3, FIELD_Y, fieldW - 4, FIELD_H, (const mchar *)field, opts, false);
        button(target, opts, btnX, FIELD_Y, BTN_W, FIELD_H, scan.busy() ? "Stop" : "Scan");

        char line[64];
        scan.describe(line, sizeof(line));
        target->DrawText(SCAN_X, SCAN_STATUS_Y, fullW - SCAN_X - 4, 10, (const mchar *)line, opts, false);
        target->FillRect(PANE + 2, LIST_SEP_Y, fullW - PANE - 4, 1, dark, false);

        listRows = (bottom - LIST_Y) / LIST_ROW_H;
        if (listRows < 1)
            listRows = 1;
        int maxTop = scan.up() - listRows;
        if (listTop > maxTop)
            listTop = maxTop > 0 ? maxTop : 0;

        int idx = liveAt(listTop);
        for (int row = 0; row < listRows && idx >= 0; idx++)
        {
            if (idx >= scan.count())
                break;
            if (!scan.isUp(idx))
                continue;
            const HostScan::Host &h = scan.host(idx);
            Coord ry = LIST_Y + row * LIST_ROW_H;

            unsigned char ip[4];
            scan.addressOf(idx, ip);
            char buf[20];
            ipToStr(ip, buf);
            target->DrawText(SCAN_X, ry, COL_MAC - 2, 10, (const mchar *)buf, opts, false);
            if (h.flags & HostScan::F_MAC)
            {
                macToStr(h.mac, buf);
                target->DrawText(SCAN_X + COL_MAC, ry, COL_HOW - COL_MAC - 2, 10, (const mchar *)buf, opts, false);
            }

            // How it answered: "self", or the round trip when there is one,
            // else "arp" or "ping".
            if (h.flags & HostScan::F_SELF)
                strcpy(buf, "self");
            else if (h.flags & HostScan::F_RTT)
            {
                unsigned int ms = h.rttMs > 9999 ? 9999 : h.rttMs;
                u16ToStr((unsigned short)ms, buf);
                strcpy(buf + strlen(buf), "ms");
            }
            else
                strcpy(buf, (h.flags & HostScan::F_ARP) ? "arp" : "ping");
            target->DrawText(SCAN_X + COL_HOW, ry, fullW - 4 - SCAN_X - COL_HOW, 10, (const mchar *)buf, opts, false);
            row++;
        }

        // More above or below than fits: say so in the separator's corner.
        if (listTop > 0 || scan.up() > listTop + listRows)
        {
            char more[16];
            u16ToStr((unsigned short)(listTop + 1), more);
            strcpy(more + strlen(more), "-");
            int last = listTop + listRows < scan.up() ? listTop + listRows : scan.up();
            u16ToStr((unsigned short)last, more + strlen(more));
            opts->horizontalAlign = PlatformAlign::End;
            target->DrawText(fullW - 60, SCAN_STATUS_Y, 56, 10, (const mchar *)more, opts, false);
            opts->horizontalAlign = PlatformAlign::Begin;
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

        if (!cidrSuggested)
        {
            HostScan::suggest(cidr);
            cidrLen = (int)strlen(cidr);
            cidrSuggested = true;
        }

        // The status pane keeps its own width; the scan pane has the rest.
        Coord fullW = target->GetWidth(), H = target->GetHeight();
        Coord W = PANE;
        target->FillRect(0, 0, fullW, H, light, false);

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

        // Back button's place first: the port grid stops above it.
        backX = (F_COORD(fullW) - BACK_W) / 2;
        backY = F_COORD(H) - BACK_H - 2;

        // Ports section header, with how many there are
        int nPorts = ns.n_ports > 16 ? 16 : ns.n_ports;
        char head[24] = "TCP ports: ";
        u16ToStr((unsigned short)nPorts, head + 11);
        target->DrawText(LABEL_X, PORTS_Y, LABEL_W + 60, 10, (const mchar *)head, &opts, false);

        if (nPorts == 0)
        {
            target->DrawText(LABEL_X, PORT_GRID_Y, VALUE_W, 10, "none", &opts, false);
        }
        else
        {
            int cols = (F_COORD(W) - 2 * LABEL_X) / PORT_COL_W;
            int rows = (backY - 4 - PORT_GRID_Y) / PORT_ROW_H;
            if (cols < 1)
                cols = 1;
            if (rows < 1)
                rows = 1;
            int cells = cols * rows;
            for (int i = 0; i < nPorts && i < cells; i++)
            {
                int row = i / cols, col = i % cols;
                Coord ry = PORT_GRID_Y + row * PORT_ROW_H;
                Coord rx = LABEL_X + col * PORT_COL_W;
                char pbuf[8];
                if (i == cells - 1 && nPorts > cells)
                {
                    strcpy(pbuf, "+");
                    u16ToStr((unsigned short)(nPorts - i), pbuf + 1);
                }
                else
                    u16ToStr(ns.ports[i], pbuf);
                target->DrawText(rx, ry, PORT_COL_W - 2, 10, (const mchar *)pbuf, &opts, false);
            }
        }

        PaintScan(target, &opts, F_COORD(fullW), backY - 4);

        target->FillRect(2, backY - 3, fullW - 4, 1, dark, false);
        target->FillRect(backX, backY, BACK_W, BACK_H, dark, false);
        target->FillRect(backX + 1, backY + 1, BACK_W - 2, BACK_H - 2, light, false);
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;
        target->DrawText(backX, backY, BACK_W, BACK_H, "Back", &opts, false);
    }
};
