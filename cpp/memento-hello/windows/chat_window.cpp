//
// Window — Chat client (ETH, TCP client mode)
// Local source port 9001 avoids colliding with any server bound to 9000.
//

class ChatWindow
{
    static const int SOCK_ESTABLISHED = 3;
    static const int SOCK_SYN_SENT = 2;
    static const int SOCK_CLOSED = 0;
    static const unsigned short LOCAL_PORT = 9001; // client source port

    static const int MSG_W = 44;
    static const int MAX_MSGS = 50;
    // Same arithmetic as the IRC window: the bottom 24 units are two rules,
    // the nick line and the input line, and a thirteenth row of messages lands
    // on top of the nick.
    static const int VIS_ROWS = 12;
    static const int IN_CAP = 60;

    // Connection phase
    enum Phase
    {
        PH_IP,
        PH_PORT,
        PH_CHAT
    } phase = PH_IP;

    // Configured server address (filled during PH_IP / PH_PORT)
    unsigned char peer_ip[4] = {10, 3, 3, 1};
    unsigned short server_port = 9000;
    unsigned char my_ip[4] = {10, 3, 3, 2};

    TcpSocket_C sockets[MAX_SOCKETS_C];
    TcpSocket_C *chat_sock = nullptr;
    bool netInit = false;
    bool connected = false;
    bool have_nick = false;

    char msgs[MAX_MSGS][MSG_W + 1];
    int msgTotal = 0;

    char inputBuf[IN_CAP + 1] = {};
    int inputLen = 0;
    char nick[17] = {};

    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    bool imEnabled = false;
    int synRetries = 0;

    // ── helpers ──────────────────────────────────────────────────────────────

    void addLine(const char *line)
    {
        int slot = msgTotal % MAX_MSGS;
        int i = 0;
        while (line[i] && i < MSG_W)
        {
            msgs[slot][i] = line[i];
            i++;
        }
        msgs[slot][i] = '\0';
        msgTotal++;
    }

    static bool parseIP(const char *s, unsigned char ip[4])
    {
        for (int o = 0; o < 4; o++)
        {
            if (*s < '0' || *s > '9')
                return false;
            int v = 0;
            while (*s >= '0' && *s <= '9')
                v = v * 10 + (*s++ - '0');
            if (v > 255)
                return false;
            ip[o] = (unsigned char)v;
            if (o < 3 && *s++ != '.')
                return false;
        }
        return true;
    }

    static unsigned short parsePort(const char *s)
    {
        int v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        return (v >= 1 && v <= 65535) ? (unsigned short)v : (unsigned short)9000;
    }

    void initNet()
    {
        if (netInit)
            return;
        netInit = true;

        // netDriverInit persists across ChatWindow instances — the kernel's
        // net_register and RTL8139 init must only happen once per process.
        static bool netDriverInit = false;
        if (!netDriverInit)
        {
            static const unsigned char eth[] = {'e', 't', 'h', 0};
            if (net_driver_bind_port(eth, LOCAL_PORT) < 0)
            {
                addLine("[net: driver init failed]");
                return;
            }
            net_get_local_ip(my_ip);

            // Send a broadcast ARP request so the ARP cache gets the server's
            // MAC before the first SYN is sent.  Non-blocking: we fire and
            // forget; pollNet drains the ARP reply on the next idle tick.
            unsigned char f[42];
            for (int i = 0; i < 6; i++)
                f[i] = 0xFF;
            f[6] = 0x52;
            f[7] = 0x54;
            f[8] = 0x00;
            f[9] = 0x12;
            f[10] = 0x34;
            f[11] = 0x56;
            f[12] = 0x08;
            f[13] = 0x06;
            f[14] = 0x00;
            f[15] = 0x01;
            f[16] = 0x08;
            f[17] = 0x00;
            f[18] = 0x06;
            f[19] = 0x04;
            f[20] = 0x00;
            f[21] = 0x01;
            f[22] = 0x52;
            f[23] = 0x54;
            f[24] = 0x00;
            f[25] = 0x12;
            f[26] = 0x34;
            f[27] = 0x56;
            f[28] = my_ip[0];
            f[29] = my_ip[1];
            f[30] = my_ip[2];
            f[31] = my_ip[3];
            for (int i = 32; i < 38; i++)
                f[i] = 0;
            f[38] = peer_ip[0];
            f[39] = peer_ip[1];
            f[40] = peer_ip[2];
            f[41] = peer_ip[3];
            send_eth_frame(f, 42);

            netDriverInit = true;
        }
        else
        {
            // Driver already running; ARP cache is still valid from previous
            // connection — just refresh our local IP copy.
            net_get_local_ip(my_ip);
        }

        // Always start with a clean socket table for each new connection.
        memset(sockets, 0, sizeof(sockets));
        chat_sock = tcp_connect(sockets, peer_ip, server_port, LOCAL_PORT, my_ip);
        if (!chat_sock)
        {
            addLine("[net: socket alloc failed]");
        }
        else
        {
            char msg[MSG_W + 1];
            int i = 0;
            const char *pfx = "[connecting to ";
            while (*pfx && i < MSG_W)
                msg[i++] = *pfx++;
            for (int o = 0; o < 4 && i < MSG_W - 1; o++)
            {
                unsigned char b = peer_ip[o];
                if (b >= 100 && i < MSG_W)
                    msg[i++] = '0' + b / 100;
                if (b >= 10 && i < MSG_W)
                    msg[i++] = '0' + (b / 10) % 10;
                if (i < MSG_W)
                    msg[i++] = '0' + b % 10;
                if (o < 3 && i < MSG_W)
                    msg[i++] = '.';
            }
            if (i < MSG_W)
                msg[i++] = ':';
            char tbuf[6];
            int ti = 0;
            unsigned short tp = server_port;
            if (tp == 0)
            {
                tbuf[ti++] = '0';
            }
            else
            {
                while (tp)
                {
                    tbuf[ti++] = '0' + tp % 10;
                    tp /= 10;
                }
            }
            for (int k = ti - 1; k >= 0 && i < MSG_W; k--)
                msg[i++] = tbuf[k];
            if (i < MSG_W)
                msg[i++] = ']';
            msg[i > MSG_W ? MSG_W : i] = '\0';
            addLine(msg);
        }
    }

    void pollNet()
    {
        if (!chat_sock)
            return;
        // Static buffers — avoids ~4.5 KB of stack per call, which overflows r2's
        // fixed-size user stack when called from within the Memento event loop.
        static unsigned char pkt[2048];
        int len = net_recv_nb(pkt, sizeof(pkt));
        if (len > 0)
        {
            Ipv4Header_C ipv4;
            unsigned short hlen = parse_ipv4_packet(pkt, &ipv4);
            if (hlen && ipv4.protocol == 6)
            {
                static unsigned char tcp_raw[1500];
                int tlen = len - (int)hlen;
                if (tlen > 1500)
                    tlen = 1500;
                for (int i = 0; i < tlen; i++)
                    tcp_raw[i] = pkt[hlen + i];
                TcpHeader_C thdr;
                parse_tcp_packet(tcp_raw, &thdr);
                on_tcp_packet(ipv4.source_addr, ipv4.destination_addr,
                              &thdr, tcp_raw, (unsigned int)tlen, sockets);
                if (chat_sock->state == SOCK_ESTABLISHED)
                {
                    static unsigned char rx[1024];
                    unsigned int n = chat_sock_read(chat_sock, rx, sizeof(rx) - 1);
                    if (n > 0)
                    {
                        unsigned int off = 0;
                        while (off < n)
                        {
                            unsigned char lb[MSG_W + 1];
                            int li = 0;
                            while (off < n && li < MSG_W)
                            {
                                unsigned char c = rx[off++];
                                if (c == '\n')
                                    break;
                                if (c == '\r')
                                    continue;
                                lb[li++] = c;
                            }
                            if (li > 0)
                            {
                                lb[li] = '\0';
                                addLine((const char *)lb);
                            }
                        }
                        wnd->Repaint();
                    }
                }
            }
        }
        // If the socket was freed by an RST (server rejected our SYN),
        // reallocate and retry the connection immediately.
        if (!connected && chat_sock && !chat_sock->used)
        {
            chat_sock = tcp_connect(sockets, peer_ip, server_port, LOCAL_PORT, my_ip);
            synRetries = 0;
        }
        // SYN retry: resend every 5 ticks (~50 ms) until SYN-ACK arrives.
        // Reset seq_num to 0 before each retry so ISN=0 matches what
        // tcp_connect set up, avoiding server-side sequence mismatches.
        if (!connected && chat_sock && chat_sock->state == SOCK_SYN_SENT)
        {
            if (++synRetries % 5 == 0)
            {
                chat_sock->seq_num = 0;
                send_tcp_packet_c(chat_sock, nullptr, 0, 0x02); // SYN(seq=0)
                chat_sock->seq_num = 1;
            }
        }
        if (!connected && chat_sock && chat_sock->state == SOCK_ESTABLISHED)
        {
            connected = true;
            synRetries = 0;
            addLine("[connected]");
            if (!have_nick)
                addLine("[enter your nick]");
            wnd->Repaint();
        }
        if (connected && chat_sock && chat_sock->state == SOCK_CLOSED)
        {
            connected = false;
            addLine("[disconnected]");
            wnd->Repaint();
        }
    }

    // ── event handler ─────────────────────────────────────────────────────────

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            pollNet();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;

        if (key->isEscape)
        {
            if (phase == PH_PORT)
            {
                // back to IP entry
                phase = PH_IP;
                inputLen = 0;
                inputBuf[0] = '\0';
                wnd->Repaint();
                return;
            }
            if (imEnabled)
            {
                wnd->SetImmediateMode(false);
                imEnabled = false;
            }
            if (chat_sock)
                chat_sock_close(chat_sock);
            wnd->Close();
            return;
        }
        if (key->isBackspace)
        {
            if (inputLen > 0)
            {
                inputLen--;
                inputBuf[inputLen] = '\0';
                wnd->Repaint();
            }
            return;
        }
        if (key->isEnter)
        {
            if (phase == PH_IP)
            {
                unsigned char tmp[4];
                if (inputLen > 0 && parseIP(inputBuf, tmp))
                {
                    for (int i = 0; i < 4; i++)
                        peer_ip[i] = tmp[i];
                }
                // move to port entry; pre-fill with "9000"
                phase = PH_PORT;
                inputBuf[0] = '9';
                inputBuf[1] = '0';
                inputBuf[2] = '0';
                inputBuf[3] = '0';
                inputBuf[4] = '\0';
                inputLen = 4;
                wnd->Repaint();
                return;
            }
            if (phase == PH_PORT)
            {
                if (inputLen > 0)
                    server_port = parsePort(inputBuf);
                inputLen = 0;
                inputBuf[0] = '\0';
                phase = PH_CHAT;
                initNet();
                wnd->SetImmediateMode(true);
                imEnabled = true;
                wnd->Repaint();
                return;
            }
            // PH_CHAT: send nick or message
            if (inputLen == 0)
                return;
            if (!have_nick)
            {
                have_nick = true;
                int ni = 0;
                while (ni < inputLen && ni < 16)
                {
                    nick[ni] = inputBuf[ni];
                    ni++;
                }
                nick[ni] = '\0';
                char echo[MSG_W + 1];
                int ei = 0;
                const char *ep = "[you] nick: ";
                while (*ep && ei < MSG_W)
                    echo[ei++] = *ep++;
                for (int i = 0; i < ni && ei < MSG_W; i++)
                    echo[ei++] = nick[i];
                echo[ei] = '\0';
                addLine(echo);
                unsigned char sb[18];
                int si2 = 0;
                for (int i = 0; i < inputLen && si2 < 16; i++)
                    sb[si2++] = (unsigned char)inputBuf[i];
                sb[si2++] = '\n';
                if (chat_sock && chat_sock->state == SOCK_ESTABLISHED)
                    chat_sock_write(chat_sock, sb, (unsigned int)si2);
            }
            else
            {
                char echo[MSG_W + 1];
                int ei = 0;
                const char *ep = "[me] ";
                while (*ep && ei < MSG_W)
                    echo[ei++] = *ep++;
                for (int i = 0; i < inputLen && ei < MSG_W; i++)
                    echo[ei++] = inputBuf[i];
                echo[ei] = '\0';
                addLine(echo);
                unsigned char sb[IN_CAP + 2];
                int si2 = 0;
                for (int i = 0; i < inputLen; i++)
                    sb[si2++] = (unsigned char)inputBuf[i];
                sb[si2++] = '\n';
                if (chat_sock && chat_sock->state == SOCK_ESTABLISHED)
                    chat_sock_write(chat_sock, sb, (unsigned int)si2);
            }
            inputLen = 0;
            inputBuf[0] = '\0';
            wnd->Repaint();
            return;
        }
        if (key->isChar && inputLen < IN_CAP)
        {
            inputBuf[inputLen++] = (char)key->theChar;
            inputBuf[inputLen] = '\0';
            wnd->Repaint();
        }
    }

    // ── paint ─────────────────────────────────────────────────────────────────

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *target)
    {
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


        // Dialog chrome
        // Client area only: the root frames the window and names it.
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        opts.horizontalAlign = PlatformAlign::Begin;

        if (phase == PH_IP || phase == PH_PORT)
        {
            // ── Connection setup form ────────────────────────────────────────
            const char *prompt = (phase == PH_IP) ? "Server IP:" : "Server port:";
            const char *hint = (phase == PH_IP)
                                   ? "e.g. 10.3.3.1  [Enter] next  [Esc] cancel"
                                   : "[Enter] connect  [Esc] back";

            target->DrawText(6, 6, W - 12, 10, prompt, &opts, false);

            // Input box — inset 30 px each side for comfortable margin
            target->FillRect(20, 20, W - 40, 14, dark, false);
            target->FillRect(22, 22, W - 44, 10, light, false);
            char display[IN_CAP + 3] = {};
            int i = 0;
            while (inputBuf[i])
            {
                display[i] = inputBuf[i];
                i++;
            }
            display[i++] = '_';
            display[i] = '\0';
            target->DrawText(26, 22, W - 52, 10, display, &opts, false);

            target->DrawText(6, 38, W - 12, 10, hint, &opts, false);

            // Show current peer IP when on port screen
            if (phase == PH_PORT)
            {
                char ipstr[24] = "IP: ";
                int k = 4;
                for (int o = 0; o < 4 && k < 22; o++)
                {
                    unsigned char b = peer_ip[o];
                    if (b >= 100)
                        ipstr[k++] = '0' + b / 100;
                    if (b >= 10)
                        ipstr[k++] = '0' + (b / 10) % 10;
                    ipstr[k++] = '0' + b % 10;
                    if (o < 3)
                        ipstr[k++] = '.';
                }
                ipstr[k] = '\0';
                target->DrawText(6, 50, W - 12, 10, ipstr, &opts, false);
            }
        }
        else
        {
            // ── Chat view ───────────────────────────────────────────────────
            target->FillRect(2, H - 24, W - 4, 1, dark, false);

            // Nick / status strip
            char nickLine[36] = {};
            int ni = 0;
            if (have_nick)
            {
                const char *s = "nick: [";
                while (*s)
                    nickLine[ni++] = *s++;
                for (int i = 0; nick[i] && ni < 32; i++)
                    nickLine[ni++] = nick[i];
                if (ni < 34)
                    nickLine[ni++] = ']';
            }
            else if (connected)
            {
                const char *s = "enter nick:";
                while (*s && ni < 34)
                    nickLine[ni++] = *s++;
            }
            else
            {
                const char *s = "connecting...";
                while (*s && ni < 34)
                    nickLine[ni++] = *s++;
            }
            nickLine[ni] = '\0';
            target->DrawText(4, H - 23, W - 8, 9, nickLine, &opts, false);

            // Input field
            target->FillRect(2, H - 13, W - 4, 1, dark, false);
            target->FillRect(2, H - 12, W - 4, 11, light, false);
            char display[IN_CAP + 3] = {};
            int i = 0;
            while (inputBuf[i])
            {
                display[i] = inputBuf[i];
                i++;
            }
            display[i++] = '_';
            display[i] = '\0';
            target->DrawText(4, H - 12, W - 8, 11, display, &opts, false);

            // Message area — last VIS_ROWS lines
            int startMsg = msgTotal - VIS_ROWS;
            if (startMsg < 0)
                startMsg = 0;
            for (int i = 0; i < VIS_ROWS; i++)
            {
                int idx = startMsg + i;
                if (idx >= msgTotal)
                    break;
                target->DrawText(4, 4 + i * 10, W - 8, 10, msgs[idx % MAX_MSGS], &opts, false);
            }
        }
    }

public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<ChatWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }
};
