// Network tools share the process frame mux; closing any way stops them and
// releases their port. Stats prefer the kernel NIC counters and owner table,
// with Memento-only traffic and port numbers as the old-kernel fallback.
#include "../nsk.h"
#include <r2/net.hpp>

class NetWindow
{
public:
    static const int W = 306, H = 170;
    static void onEvent(void *instance, PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<NetWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; updateIdle(); }

private:
    enum { STATS, SCAN, PING, TRACE, CHARTS, TABS };
    static const int MARGIN = 6, ROW_H = 10, BUTTON_W = 40, BUTTON_H = 11;
    static const int FIELD_Y = TabStrip::BELOW, FIELD_H = 11;
    static const int STATUS_Y = FIELD_Y + 13, HEAD_Y = STATUS_Y + 12, LIST_Y = HEAD_Y + 12;
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr, *light = nullptr;
    PlatformFont *font = nullptr;
    NetTools tools;
    MetricsCharts charts;
    uint64_t chartRead = 0;
    NetStatus_T ns{};
    int tab = STATS, top[TABS] = {}, rows = 1;
    bool followPing = true, idleOn = false, suggested = false, sampled = false;
    bool haveStatus = false, haveKernelTraffic = false, havePorts = false;
    r2::NetPortTable portTable{};
    char cidr[20] = {}, pingIp[16] = "1.1.1.1", traceIp[16] = "1.1.1.1";
    char inputError[TABS][64] = {};
    int listBottom = 0, btnX = 0;
    uint64_t lastSample = 0, lastRepaint = 0;
    r2::NetStats previous{};
    uint64_t rxRate = 0, txRate = 0;

    static void append(char *out, const char *s) { strcpy(out + strlen(out), s); }
    static void number(uint64_t n, char *out)
    {
        char tmp[24]; int k = 0;
        do { tmp[k++] = (char)('0' + n % 10); n /= 10; } while (n);
        int at = 0; while (k) out[at++] = tmp[--k]; out[at] = 0;
    }
    static void macText(const uint8_t mac[6], char *out)
    {
        const char *hex = "0123456789abcdef";
        for (int i = 0; i < 6; i++)
        {
            out[i * 3] = hex[mac[i] >> 4]; out[i * 3 + 1] = hex[mac[i] & 15]; out[i * 3 + 2] = i == 5 ? 0 : ':';
        }
    }
    void updateIdle()
    {
        bool on = tab == STATS || tab == CHARTS || tools.busy();
        if (wnd && idleOn != on) { wnd->SetImmediateMode(on); idleOn = on; }
    }
    void sample(uint64_t now)
    {
        memset(&ns, 0, sizeof(ns)); haveStatus = get_net_status(&ns) == 0;
        auto owners = r2::net::ports();
        havePorts = (bool)owners;
        if (owners) portTable = *owners;

        auto traffic = r2::net::stats();
        bool kernelTraffic = (bool)traffic;
        r2::NetStats current{};
        if (traffic) current = *traffic;
        else
        {
            NetmuxStats local = netmux_stats();
            current = {now, local.rx_frames, local.rx_bytes, local.tx_frames, local.tx_bytes};
        }
        rxRate = txRate = 0;
        if (sampled && kernelTraffic == haveKernelTraffic && current.timestamp_ms > previous.timestamp_ms)
        {
            uint64_t elapsed = current.timestamp_ms - previous.timestamp_ms;
            if (current.rx_bytes >= previous.rx_bytes) rxRate = (current.rx_bytes - previous.rx_bytes) * 1000 / elapsed;
            if (current.tx_bytes >= previous.tx_bytes) txRate = (current.tx_bytes - previous.tx_bytes) * 1000 / elapsed;
        }
        previous = current; haveKernelTraffic = kernelTraffic; lastSample = now; sampled = true;
    }
    void setTab(int next)
    {
        tab = (next + TABS) % TABS;
        if (tab == STATS) { sampled = false; rxRate = txRate = 0; }
        updateIdle(); wnd->Repaint();
    }
    bool active() const
    {
        return tab == SCAN ? tools.scan.busy() : tab == PING ? tools.ping.busy() : tab == TRACE ? tools.trace.busy() : false;
    }
    void startOrStop()
    {
        inputError[tab][0] = 0;
        if (active())
        {
            if (tab == SCAN) tools.scan.stop();
            else if (tab == PING) tools.ping.stop();
            else tools.trace.stop();
        }
        else if (tab == SCAN) { top[tab] = 0; tools.scan.start(tools.link, cidr); }
        else if (tab == PING || tab == TRACE)
        {
            uint8_t dst[4]; const char *s = tab == PING ? pingIp : traceIp;
            if (!NetLink::parseAddress(s, dst)) strcpy(inputError[tab], "Enter an IPv4 address, e.g. 1.1.1.1.");
            else
            {
                top[tab] = 0;
                if (tab == PING) { followPing = true; tools.ping.start(tools.link, dst); }
                else tools.trace.start(tools.link, dst);
            }
        }
        tools.step(); updateIdle(); wnd->Repaint();
    }
    int count() const
    {
        return tab == STATS ? (havePorts ? portTable.n_ports : (ns.n_ports > 16 ? 16 : ns.n_ports)) : tab == SCAN ? tools.scan.up() :
               tab == PING ? tools.ping.lines() : tools.trace.hops();
    }
    void scroll(int by)
    {
        int max = count() - rows; if (max < 0) max = 0;
        top[tab] += by;
        if (top[tab] < 0) top[tab] = 0;
        if (top[tab] > max) top[tab] = max;
        if (tab == PING) followPing = top[tab] == max;
        wnd->Repaint();
    }
    void onIdle()
    {
        uint64_t now = r2::ticks();
        bool wasBusy = tools.busy(), changed = tools.step();
        if (tab == STATS && (!sampled || now - lastSample >= 1000)) { sample(now); changed = true; }
        if (tab == CHARTS && now / 1000 != chartRead / 1000) { chartRead = now; changed = true; }
        updateIdle();
        if ((changed && now - lastRepaint >= 250) || (wasBusy && !tools.busy()))
        {
            lastRepaint = now; wnd->Repaint();
        }
        if (!tools.busy()) r2::sleep(10);
    }
    void onEvent_(PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnClose)
        {
            tools.stopAll();
            if (idleOn) { wnd->SetImmediateMode(false); idleOn = false; }
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnPaint) { paint(data->Data.OnPaint.ctx, data->Data.OnPaint.target); return; }
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop) { onIdle(); return; }
        if (data->type == PlatformWindowInputEventType::OnMouseWheel) { if (tab != CHARTS) scroll(data->Data.OnMouseWheel.up ? -3 : 3); return; }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed) return;
            int x = F_COORD(data->Data.OnMouseClick.mouseX), y = F_COORD(data->Data.OnMouseClick.mouseY);
            int hit = TabStrip::at(x, y, TABS);
            if (hit >= 0) { setTab(hit); return; }
            if (tab == CHARTS) { if (charts.click(x, y)) wnd->Repaint(); return; }
            if (tab != STATS && x >= btnX && x < btnX + BUTTON_W && y >= FIELD_Y && y < FIELD_Y + FIELD_H) startOrStop();
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent) return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown) return;
        if (key->isEscape) { wnd->Close(); return; }
        if (key->isTab || key->isArrowRight) { setTab(tab + 1); return; }
        if (key->isArrowLeft) { setTab(tab - 1); return; }
        if (tab == CHARTS) { if (key->isChar && charts.key((char)key->theChar)) wnd->Repaint(); return; }
        if (key->isArrowUp) { scroll(-1); return; }
        if (key->isArrowDown) { scroll(1); return; }
        if (key->isPageUp) { scroll(-rows); return; }
        if (key->isPageDown) { scroll(rows); return; }
        if (key->isHome) { scroll(-count()); return; }
        if (key->isEnd) { scroll(count()); return; }
        if (key->isEnter) { if (tab != STATS) startOrStop(); else { sample(r2::ticks()); wnd->Repaint(); } return; }
        if (tab == STATS || active()) return;
        char *field = tab == SCAN ? cidr : tab == PING ? pingIp : traceIp;
        int len = (int)strlen(field), cap = tab == SCAN ? 19 : 15;
        if (key->isDelete) field[0] = 0;
        else if (key->isBackspace && len) field[len - 1] = 0;
        else if (key->isChar && len < cap)
        {
            char c = (char)key->theChar;
            if ((c >= '0' && c <= '9') || c == '.' || (tab == SCAN && c == '/')) { field[len] = c; field[len + 1] = 0; }
        }
        inputError[tab][0] = 0; wnd->Repaint();
    }
    void text(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, int w, const char *s)
    {
        target->DrawText(x, y, w, ROW_H, (const mchar *)s, &opts, false);
    }
    void button(PlatformBitmap *target, PlatformDrawTextOptions &opts, int x, int y, int w, const char *s)
    {
        target->FillRect(x, y, w, BUTTON_H, dark, false); target->FillRect(x + 1, y + 1, w - 2, BUTTON_H - 2, light, false);
        opts.horizontalAlign = PlatformAlign::Middle; text(target, opts, x, y, w, s); opts.horizontalAlign = PlatformAlign::Begin;
    }
    void listLayout(PlatformBitmap *target, int width, int y)
    {
        rows = (listBottom - y) / ROW_H; if (rows < 1) rows = 1;
        int max = count() - rows; if (max < 0) max = 0;
        if (top[tab] > max) top[tab] = max;
        if (tab == PING && followPing) top[tab] = max;
        if (count() > rows)
        {
            int height = rows * ROW_H;
            target->FillRect(width - 4, y, 1, height, dark, false);
            int thumb = height * rows / count(); if (thumb < 3) thumb = 3;
            target->FillRect(width - 5, y + (height - thumb) * top[tab] / max, 3, thumb, dark, false);
        }
    }
    void stats(PlatformBitmap *target, PlatformDrawTextOptions &opts, int width)
    {
        char buf[128], ip[16], mac[18]; NetLink::formatAddress(ns.ip, ip); macText(ns.mac, mac);
        int y = TabStrip::BELOW;
        strcpy(buf, "IP: "); append(buf, ip); append(buf, "    MAC: "); append(buf, mac); text(target, opts, MARGIN, y, width - 12, buf);
        if (havePorts && portTable.driver_pid != r2::NetNoPid)
        {
            strcpy(buf, "Network driver: PID "); number(portTable.driver_pid, buf + strlen(buf));
        }
        else strcpy(buf, havePorts ? "Network driver: inactive" : !haveStatus ? "Network status unavailable" : ns.drv_active ? "Network driver: active" : "Network driver: inactive");
        text(target, opts, MARGIN, y + 11, width - 12, buf);
        strcpy(buf, haveKernelTraffic ? "NIC traffic: RX " : "Memento traffic: RX "); number(rxRate, buf + strlen(buf)); append(buf, " B/s    TX "); number(txRate, buf + strlen(buf)); append(buf, " B/s");
        text(target, opts, MARGIN, y + 24, width - 12, buf);
        strcpy(buf, "Totals: RX "); number(previous.rx_bytes / 1024, buf + strlen(buf)); append(buf, " KiB    TX "); number(previous.tx_bytes / 1024, buf + strlen(buf)); append(buf, " KiB");
        text(target, opts, MARGIN, y + 35, width - 12, buf);
        text(target, opts, MARGIN, y + 48, width - 12, havePorts ? "Bound TCP ports" : "Bound TCP ports (owners unavailable)");
        int head = y + 61, first = head + 12;
        text(target, opts, 6, head, 38, "Port"); text(target, opts, 48, head, 48, "PID");
        text(target, opts, 100, head, 98, "Process"); text(target, opts, 202, head, width - 214, "Memento use");
        target->FillRect(4, head + 10, width - 8, 1, dark, false); listLayout(target, width, first);
        if (!count()) text(target, opts, MARGIN, first, width - 12, (havePorts || haveStatus) ? "No bound TCP ports." : "Port registry unavailable.");
        for (int r = 0; r < rows && top[tab] + r < count(); r++)
        {
            int index = top[tab] + r, ry = first + r * ROW_H;
            uint16_t port = havePorts ? portTable.bindings[index].port : ns.ports[index];
            number(port, buf); text(target, opts, 6, ry, 38, buf);
            char name[17] = "-";
            if (havePorts)
            {
                const r2::NetPortBinding &owner = portTable.bindings[index];
                number(owner.pid, buf);
                memcpy(name, owner.name, 16); name[16] = 0;
            }
            else strcpy(buf, "-");
            text(target, opts, 48, ry, 48, buf); text(target, opts, 100, ry, 98, name[0] ? name : "-");
            const char *use = netmux_port_use(port); text(target, opts, 202, ry, width - 214, use ? use : "-");
        }
    }
    int liveAt(int row) const
    {
        for (int i = 0; i < tools.scan.count(); i++) if (tools.scan.isUp(i) && row-- == 0) return i;
        return -1;
    }
    void scanRows(PlatformBitmap *target, PlatformDrawTextOptions &opts, int width)
    {
        text(target, opts, 6, HEAD_Y, 64, "Address"); text(target, opts, 74, HEAD_Y, 76, "MAC");
        text(target, opts, 154, HEAD_Y, 48, "Reply"); text(target, opts, 206, HEAD_Y, width - 218, "RTT / probe");
        int idx = liveAt(top[tab]);
        for (int r = 0; r < rows && idx >= 0 && idx < tools.scan.count(); idx++)
        {
            if (!tools.scan.isUp(idx)) continue;
            const HostScan::Host &h = tools.scan.host(idx); int y = LIST_Y + r++ * ROW_H;
            uint8_t ip[4]; char buf[40]; tools.scan.addressOf(idx, ip); NetLink::formatAddress(ip, buf);
            text(target, opts, 6, y, 64, buf);
            if (h.flags & HostScan::F_MAC) { macText(h.mac, buf); text(target, opts, 74, y, 76, buf); }
            text(target, opts, 154, y, 48, h.flags & HostScan::F_SELF ? "self" : h.flags & HostScan::F_ARP ? "ARP" : "ICMP");
            if (h.flags & HostScan::F_RTT)
            {
                number(h.rttMs, buf); append(buf, " ms / "); number(h.answeredOn, buf + strlen(buf)); text(target, opts, 206, y, width - 218, buf);
            }
        }
    }
    void pingRows(PlatformBitmap *target, PlatformDrawTextOptions &opts, int width)
    {
        char buf[128]; tools.ping.summary(buf, sizeof(buf)); text(target, opts, 6, HEAD_Y, width - 12, buf);
        for (int r = 0; r < rows && top[tab] + r < count(); r++)
        {
            const Pinger::Line &l = tools.ping.line(top[tab] + r); char ip[16]; NetLink::formatAddress(l.from, ip);
            number(l.seq, buf); append(buf, ": "); append(buf, ip); append(buf, "  ");
            const char *kind = l.kind == Pinger::L_TIMEOUT ? "timeout" : l.kind == Pinger::L_UNREACH ? "unreachable" :
                               l.kind == Pinger::L_NOARP ? "no ARP reply" : l.kind == Pinger::L_OPEN ? "port 80 open" :
                               l.kind == Pinger::L_CLOSED ? "port 80 closed" : "reply";
            append(buf, kind);
            if (l.kind <= Pinger::L_CLOSED) { append(buf, "  "); number(l.ms, buf + strlen(buf)); append(buf, " ms"); }
            if (l.ttl) { append(buf, "  TTL "); number(l.ttl, buf + strlen(buf)); }
            text(target, opts, 6, LIST_Y + r * ROW_H, width - 18, buf);
        }
    }
    void traceRows(PlatformBitmap *target, PlatformDrawTextOptions &opts, int width)
    {
        text(target, opts, 6, HEAD_Y, 28, "Hop"); text(target, opts, 36, HEAD_Y, 64, "Address");
        text(target, opts, 104, HEAD_Y, width - 116, "RTT ms (three probes)");
        for (int r = 0; r < rows && top[tab] + r < count(); r++)
        {
            int idx = top[tab] + r, y = LIST_Y + r * ROW_H; const Tracer::Hop &h = tools.trace.hop(idx); char buf[80];
            number(idx + 1, buf); text(target, opts, 6, y, 28, buf);
            if (h.flags & Tracer::H_ADDR) NetLink::formatAddress(h.ip, buf); else strcpy(buf, "*");
            text(target, opts, 36, y, 64, buf); buf[0] = 0;
            for (int i = 0; i < Tracer::ROUNDS; i++)
            {
                if (i) append(buf, " / ");
                if (h.answered & (1 << i)) number(h.ms[i], buf + strlen(buf)); else append(buf, "*");
            }
            if (h.flags & Tracer::H_DEST) append(buf, "  reached");
            if (h.flags & Tracer::H_UNREACH) { append(buf, "  unreachable "); number(h.code, buf + strlen(buf)); }
            text(target, opts, 104, y, width - 116, buf);
        }
    }
    void paint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
        if (!target) return;
        if (!dark) dark = dc->CreateColor(0xFF0000AA, nullptr, nullptr);
        if (!light) light = dc->CreateColor(0xFFE0E0FF, nullptr, nullptr);
        if (!font) font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
        if (!dark || !light || !font) return;
        if (!suggested) { HostScan::suggest(cidr); suggested = true; }
        if (tab == STATS && !sampled) sample(r2::ticks());
        sampleMementoMetrics(r2::ticks());
        Coord cw = target->GetWidth(), ch = target->GetHeight();
        int width = F_COORD(cw), height = F_COORD(ch);
        listBottom = height - MARGIN; btnX = width - MARGIN - BUTTON_W;
        target->FillRect(0, 0, width, height, light, false);
        PlatformDrawTextOptions opts{}; opts.font = font; opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Begin; opts.verticalAlign = PlatformAlign::Middle;
        const char *labels[TABS] = {"Stats", "Scan", "Ping", "Trace", "Charts"};
        TabStrip::draw(target, opts, dark, light, width, labels, TABS, tab, tools.busy() ? "Tools running" : "");
        if (tab == STATS) stats(target, opts, width);
        else if (tab == CHARTS) charts.draw(dc, target, opts, dark, light, width, listBottom, true);
        else
        {
            const char *label = tab == SCAN ? "Subnet:" : "Host IPv4:";
            text(target, opts, MARGIN, FIELD_Y, 44, label);
            target->FillRect(52, FIELD_Y, btnX - 55, FIELD_H, dark, false);
            target->FillRect(53, FIELD_Y + 1, btnX - 57, FIELD_H - 2, light, false);
            char field[24]; strcpy(field, tab == SCAN ? cidr : tab == PING ? pingIp : traceIp);
            if (!active()) append(field, "_");
            text(target, opts, 55, FIELD_Y, btnX - 60, field);
            button(target, opts, btnX, FIELD_Y, BUTTON_W, active() ? "Stop" : tab == SCAN ? "Scan" : tab == PING ? "Ping" : "Trace");
            char line[128];
            if (inputError[tab][0]) strcpy(line, inputError[tab]);
            else if (tab == SCAN) tools.scan.describe(line, sizeof(line));
            else if (tab == PING) tools.ping.describe(line, sizeof(line));
            else tools.trace.describe(line, sizeof(line));
            text(target, opts, MARGIN, STATUS_Y, width - 12, line);
            target->FillRect(4, HEAD_Y + 10, width - 8, 1, dark, false); listLayout(target, width, LIST_Y);
            if (tab == SCAN) scanRows(target, opts, width);
            else if (tab == PING) pingRows(target, opts, width);
            else traceRows(target, opts, width);
        }
    }
};
