//
// Window 5 — IRC client
//

class IRCWindow
{
    static const int SOCK_ESTABLISHED = 3;
    static const int SOCK_SYN_SENT = 2;
    static const int SOCK_CLOSED = 0;
    static const unsigned short LOCAL_PORT = 6668;

    // How much of a line is kept. This used to be 44, which was the width of
    // the window when it filled a 320x200 screen; it has been a 290-unit
    // window over a desktop for a while now, and at four units to the glyph
    // that is seventy columns. A PRIVMSG spends up to fourteen of them on
    // "<nick> ", so 44 left about twenty-seven for what someone actually
    // said — which is why every second line from the server came back cut in
    // half. This is the message column with the user list taken off it.
    static const int USERS_W = 60;      // the user list, fifteen characters
    static const int MSG_W = 54;        // and what is left for the messages
    static const int MAX_MSGS = 50;
    // A nick may not eat more than this much of a message line.
    static const int NICK_SHOW = 12;
    // What is left of the client area once the two rules, the status line and
    // the input line have had the bottom 24 units of it: thirteen rows fit the
    // space only if the status line is not there, and the thirteenth was being
    // drawn straight through it.
    static const int VIS_ROWS = 12;
    static const int IN_CAP = 60;

    enum Phase
    {
        PH_IP,
        PH_NICK,
        PH_CHAN,
        PH_CHAT
    } phase = PH_IP;

    unsigned char server_ip[4] = {10, 3, 4, 1};
    unsigned short server_port = 6667;
    unsigned char my_ip[4] = {10, 3, 4, 2};

    char nick[17] = {};
    char channel[33] = {};
    bool registered = false;
    bool imEnabled = false;
    bool netInit = false;
    bool connected = false;
    int synRetries = 0;

    TcpSocket_C sockets[MAX_SOCKETS_C];
    TcpSocket_C *sock = nullptr;

    char msgs[MAX_MSGS][MSG_W + 1];
    int msgTotal = 0;
    int scrollOffset = 0;
    int regCount = 0;

    // ── Who is in the channel ───────────────────────────────────────────────
    //
    // Kept from the NAMES reply the server sends on join (353, ended by 366)
    // and then from the JOIN, PART, QUIT, KICK and NICK messages as they
    // arrive. Ops sort first, then everyone else, each alphabetically, so the
    // list does not reshuffle under the eye every time somebody joins.
    static const int MAX_USERS = 48;
    static const int USER_LEN = 17;
    char users[MAX_USERS][USER_LEN]; // [0] is '@' for an op, or the first letter
    int nUsers = 0;
    int userScroll = 0;      // first name shown, when the channel outgrows the pane
    int userRowsShown = 1;   // how many fit; the paint works it out and leaves it here
    int userPaneX = 0;       // and where the pane starts, for the hit test

    char ircPartial[514] = {};
    int ircPartialLen = 0;

    char inputBuf[IN_CAP + 1] = {};
    int inputLen = 0;

    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;

    // ── helpers ──────────────────────────────────────────────────────────────

    void addLine(const char *s)
    {
        int slot = msgTotal % MAX_MSGS;
        int i = 0;
        while (s[i] && i < MSG_W)
        {
            msgs[slot][i] = s[i];
            i++;
        }
        msgs[slot][i] = '\0';
        msgTotal++;
    }

    // ── The user list ───────────────────────────────────────────────────────
    //
    // An entry is the nick with its op marker still on the front, so the
    // marker survives a round trip through the list and the sort can see it.

    static bool isOpMark(char c) { return c == '@' || c == '~' || c == '&' || c == '%'; }

    static char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

    // Case-insensitive compare of the names, ignoring any op marker.
    static int nickCmp(const char *a, const char *b)
    {
        if (isOpMark(*a))
            a++;
        if (*a == '+')
            a++;
        if (isOpMark(*b))
            b++;
        if (*b == '+')
            b++;
        while (*a && *b)
        {
            char ca = lowerc(*a), cb = lowerc(*b);
            if (ca != cb)
                return ca < cb ? -1 : 1;
            a++;
            b++;
        }
        if (*a == *b)
            return 0;
        return *a ? 1 : -1;
    }

    // Ops first, then everyone else: the rank is what the sort keys on before
    // the name itself.
    static int userRank(const char *s) { return isOpMark(*s) ? 0 : 1; }

    void userClear()
    {
        nUsers = 0;
        userScroll = 0;
    }

    // How far down the list may be pushed before the last name sits at the
    // bottom of the pane. The row count comes from the paint, which is the
    // only place that knows how tall the window is.
    int userMaxScroll() const
    {
        int over = nUsers - userRowsShown;
        return over > 0 ? over : 0;
    }

    void clampUserScroll()
    {
        int max = userMaxScroll();
        if (userScroll > max)
            userScroll = max;
        if (userScroll < 0)
            userScroll = 0;
    }

    int userIndex(const char *name) const
    {
        for (int i = 0; i < nUsers; i++)
            if (nickCmp(users[i], name) == 0)
                return i;
        return -1;
    }

    void userAdd(const char *name)
    {
        if (!name || !name[0] || nUsers >= MAX_USERS)
            return;
        // A voice marker says nothing this list shows, and keeping it would
        // make the same person look like two.
        if (*name == '+')
            name++;
        if (!name[0] || userIndex(name) >= 0)
            return;

        int at = nUsers;
        while (at > 0)
        {
            const char *prev = users[at - 1];
            int pr = userRank(prev), nr = userRank(name);
            if (pr < nr || (pr == nr && nickCmp(prev, name) <= 0))
                break;
            scopy(users[at], users[at - 1], USER_LEN);
            at--;
        }
        scopy(users[at], name, USER_LEN);
        nUsers++;
    }

    void userRemove(const char *name)
    {
        int at = userIndex(name);
        if (at < 0)
            return;
        for (int i = at; i < nUsers - 1; i++)
            scopy(users[i], users[i + 1], USER_LEN);
        nUsers--;
    }

    void userRename(const char *from, const char *to)
    {
        int at = userIndex(from);
        if (at < 0)
        {
            userAdd(to);
            return;
        }
        bool wasOp = isOpMark(users[at][0]);
        userRemove(from);
        if (wasOp)
        {
            char marked[USER_LEN];
            marked[0] = '@';
            int i = 0;
            while (to[i] && i < USER_LEN - 2)
            {
                marked[i + 1] = to[i];
                i++;
            }
            marked[i + 1] = '\0';
            userAdd(marked);
        }
        else
        {
            userAdd(to);
        }
    }

    // The trailing parameter of a 353 is the space-separated names.
    void userAddList(const char *names)
    {
        char one[USER_LEN];
        int i = 0;
        while (*names && *names != '\r')
        {
            if (*names == ' ')
            {
                one[i] = '\0';
                if (i)
                    userAdd(one);
                i = 0;
                names++;
                continue;
            }
            if (i < USER_LEN - 1)
                one[i++] = *names;
            names++;
        }
        one[i] = '\0';
        if (i)
            userAdd(one);
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

    static void scopy(char *dst, const char *src, int max)
    {
        int i = 0;
        while (src[i] && i < max - 1)
        {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    // Returns pointer past ':prefix '; fills pnick with nick up to '!'
    static const char *skipPrefix(const char *msg, char *pnick, int pmax)
    {
        if (msg[0] != ':')
        {
            if (pnick && pmax > 0)
                pnick[0] = '\0';
            return msg;
        }
        int i = 1, j = 0;
        while (msg[i] && msg[i] != '!' && msg[i] != ' ' && j < pmax - 1)
            pnick[j++] = msg[i++];
        pnick[j] = '\0';
        while (msg[i] && msg[i] != ' ')
            i++;
        while (msg[i] == ' ')
            i++;
        return msg + i;
    }

    // Returns pointer to text after ' :' (IRC trailing param), or nullptr
    static const char *getTrailing(const char *s)
    {
        while (*s)
        {
            if (s[0] == ' ' && s[1] == ':')
                return s + 2;
            s++;
        }
        return nullptr;
    }

    // True if s starts with tok followed by space, \r, or \0
    static bool cmdEq(const char *s, const char *tok)
    {
        int i = 0;
        while (tok[i] && s[i] == tok[i])
            i++;
        return !tok[i] && (s[i] == ' ' || s[i] == '\r' || s[i] == '\0');
    }

    void ircSend(const char *line)
    {
        if (!sock || sock->state != SOCK_ESTABLISHED)
            return;
        unsigned char buf[514];
        int i = 0;
        while (line[i] && i < 510)
        {
            buf[i] = (unsigned char)line[i];
            i++;
        }
        buf[i++] = '\r';
        buf[i++] = '\n';
        chat_sock_write(sock, buf, (unsigned int)i);
    }

    // ── IRC line processor ────────────────────────────────────────────────────

    void processLine(const char *line)
    {
        char pnick[32] = {};
        const char *cmd = skipPrefix(line, pnick, sizeof(pnick));

        if (cmdEq(cmd, "PING"))
        {
            const char *t = getTrailing(cmd);
            char pong[128];
            int i = 0;
            const char *p = "PONG :";
            while (*p && i < 120)
                pong[i++] = *p++;
            if (t)
                while (*t && *t != '\r' && i < 126)
                    pong[i++] = *t++;
            pong[i] = '\0';
            ircSend(pong);
            addLine("[pong]");
            return;
        }
        if (cmdEq(cmd, "001"))
        {
            if (!registered)
            {
                registered = true;
                regCount++;
                userClear();
                addLine("[registered]");
                wnd->Repaint();
                char jbuf[64];
                int i = 0;
                const char *j = "JOIN ";
                while (*j && i < 60)
                    jbuf[i++] = *j++;
                int ci = 0;
                while (channel[ci] && i < 62)
                    jbuf[i++] = channel[ci++];
                jbuf[i] = '\0';
                ircSend(jbuf);
            }
            return;
        }
        if (cmdEq(cmd, "433"))
        {
            int nl = 0;
            while (nick[nl])
                nl++;
            if (nl < 15)
            {
                nick[nl] = '_';
                nick[nl + 1] = '\0';
            }
            char nbuf[32];
            int i = 0;
            const char *n = "NICK ";
            while (*n)
                nbuf[i++] = *n++;
            int ni = 0;
            while (nick[ni] && i < 30)
                nbuf[i++] = nick[ni++];
            nbuf[i] = '\0';
            ircSend(nbuf);
            addLine("[nick in use, retrying]");
            return;
        }
        if (cmdEq(cmd, "PRIVMSG"))
        {
            const char *t = getTrailing(cmd);
            if (!t)
                return;
            char disp[MSG_W + 1];
            int i = 0;
            disp[i++] = '<';
            int ni = 0;
            while (pnick[ni] && ni < NICK_SHOW)
                disp[i++] = pnick[ni++];
            disp[i++] = '>';
            disp[i++] = ' ';
            while (*t && *t != '\r' && i < MSG_W)
                disp[i++] = *t++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
            return;
        }
        if (cmdEq(cmd, "JOIN"))
        {
            userAdd(pnick);
            char disp[MSG_W + 1];
            int i = 0;
            disp[i++] = '*';
            disp[i++] = ' ';
            int ni = 0;
            while (pnick[ni] && i < MSG_W - 8)
                disp[i++] = pnick[ni++];
            const char *s = " joined";
            while (*s && i < MSG_W)
                disp[i++] = *s++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
            return;
        }
        if (cmdEq(cmd, "PART") || cmdEq(cmd, "QUIT"))
        {
            userRemove(pnick);
            char disp[MSG_W + 1];
            int i = 0;
            disp[i++] = '*';
            disp[i++] = ' ';
            int ni = 0;
            while (pnick[ni] && i < MSG_W - 6)
                disp[i++] = pnick[ni++];
            const char *s = " left";
            while (*s && i < MSG_W)
                disp[i++] = *s++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
            return;
        }
        if (cmdEq(cmd, "NICK"))
        {
            // ":old!user@host NICK :new" — the new nick is the trailing
            // parameter on most servers and a plain one on the rest.
            const char *t = getTrailing(cmd);
            char newNick[32];
            int i = 0;
            if (t)
            {
                while (t[i] && t[i] != '\r' && i < 30)
                {
                    newNick[i] = t[i];
                    i++;
                }
            }
            else
            {
                const char *p2 = cmd + 4;
                while (*p2 == ' ')
                    p2++;
                while (p2[i] && p2[i] != ' ' && p2[i] != '\r' && i < 30)
                {
                    newNick[i] = p2[i];
                    i++;
                }
            }
            newNick[i] = '\0';
            if (i)
            {
                userRename(pnick, newNick);
                // Our own rename has to be followed, or the status line goes
                // on claiming a nick the server no longer knows us by.
                if (nickCmp(nick, pnick) == 0)
                    scopy(nick, newNick, sizeof(nick));
                char disp[MSG_W + 1];
                int j = 0;
                disp[j++] = '*';
                disp[j++] = ' ';
                int k = 0;
                while (pnick[k] && j < MSG_W - 12)
                    disp[j++] = pnick[k++];
                const char *arrow = " -> ";
                while (*arrow && j < MSG_W)
                    disp[j++] = *arrow++;
                k = 0;
                while (newNick[k] && j < MSG_W)
                    disp[j++] = newNick[k++];
                disp[j] = '\0';
                addLine(disp);
                wnd->Repaint();
            }
            return;
        }
        if (cmdEq(cmd, "KICK"))
        {
            // "KICK #channel victim :reason" — the victim is the second
            // parameter, which is the one this list cares about.
            const char *p2 = cmd + 4;
            while (*p2 == ' ')
                p2++;
            while (*p2 && *p2 != ' ')
                p2++; // past the channel
            while (*p2 == ' ')
                p2++;
            char victim[32];
            int i = 0;
            while (p2[i] && p2[i] != ' ' && p2[i] != '\r' && i < 30)
            {
                victim[i] = p2[i];
                i++;
            }
            victim[i] = '\0';
            if (i)
            {
                userRemove(victim);
                char disp[MSG_W + 1];
                int j = 0;
                disp[j++] = '*';
                disp[j++] = ' ';
                int k = 0;
                while (victim[k] && j < MSG_W - 8)
                    disp[j++] = victim[k++];
                const char *s2 = " kicked";
                while (*s2 && j < MSG_W)
                    disp[j++] = *s2++;
                disp[j] = '\0';
                addLine(disp);
                wnd->Repaint();
            }
            return;
        }
        if (cmdEq(cmd, "NOTICE"))
        {
            const char *t = getTrailing(cmd);
            if (!t)
                return;
            char disp[MSG_W + 1];
            int i = 0;
            disp[i++] = '!';
            disp[i++] = ' ';
            while (*t && *t != '\r' && i < MSG_W)
                disp[i++] = *t++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
            return;
        }
        if (cmdEq(cmd, "MODE"))
            return;
        if (cmdEq(cmd, "ERROR"))
        {
            const char *t = getTrailing(cmd);
            char disp[MSG_W + 1];
            int i = 0;
            const char *pfx = "ERR ";
            while (*pfx && i < 4) disp[i++] = *pfx++;
            if (t)
                while (*t && *t != '\r' && i < MSG_W)
                    disp[i++] = *t++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
            return;
        }
        if (cmd[0] >= '0' && cmd[0] <= '9')
        {
            // 353 is the NAMES reply: the trailing parameter is the whole
            // membership of the channel, space separated and marked up with
            // @ for the ops. It arrives in as many lines as the server needs,
            // and 366 says that was the last of them. These used to be thrown
            // away with the rest of the numerics, which is why there was
            // nothing to build a user list from.
            if (cmd[0] == '3' && cmd[1] == '5' && cmd[2] == '3')
            {
                const char *t = getTrailing(cmd);
                if (t)
                {
                    userAddList(t);
                    wnd->Repaint();
                }
                return;
            }
            if (cmd[0] == '3' && cmd[1] == '6' && cmd[2] == '6')
            {
                wnd->Repaint();
                return;
            }
            // Skip MOTD (372, 375, 376) and other noisy numerics
            if (cmd[0] == '3' && cmd[1] == '7' &&
                (cmd[2] == '2' || cmd[2] == '5' || cmd[2] == '6'))
                return;
            if (cmd[0] == '2' && cmd[1] == '5' && cmd[2] >= '1')
                return;
            if (cmd[0] == '2' && cmd[1] == '6' && (cmd[2] == '5' || cmd[2] == '6'))
                return;
            // Skip 002, 003, 004, 005 (server info) and 332/333 (topic)
            if (cmd[0] == '0' && cmd[1] == '0' &&
                (cmd[2] == '2' || cmd[2] == '3' || cmd[2] == '4' || cmd[2] == '5'))
                return;
            if (cmd[0] == '3' && cmd[1] == '3' && (cmd[2] == '2' || cmd[2] == '3'))
                return;
            const char *t = getTrailing(cmd);
            if (t)
            {
                char disp[MSG_W + 1];
                int i = 0;
                disp[i++] = '[';
                while (*t && *t != '\r' && i < MSG_W - 1)
                    disp[i++] = *t++;
                disp[i++] = ']';
                disp[i] = '\0';
                addLine(disp);
                wnd->Repaint();
            }
            return;
        }
        // catch-all: show raw line (truncated) to reveal unhandled commands
        {
            char disp[MSG_W + 1];
            int i = 0;
            disp[i++] = '?';
            const char *p = line;
            while (*p && i < MSG_W)
                disp[i++] = *p++;
            disp[i] = '\0';
            addLine(disp);
            wnd->Repaint();
        }
    }

    // ── networking ────────────────────────────────────────────────────────────

    void initNet()
    {
        if (netInit)
            return;
        netInit = true;

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
            // ARP broadcast to seed the cache before SYN is sent
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
            f[38] = server_ip[0];
            f[39] = server_ip[1];
            f[40] = server_ip[2];
            f[41] = server_ip[3];
            send_eth_frame(f, 42);
            netDriverInit = true;
        }
        else
        {
            net_get_local_ip(my_ip);
        }

        // Pre-seed the ARP cache so the SYN goes out as unicast.
        // tap0's MAC is pinned to 52:54:00:12:34:57 in run_iso_net.
        // Without this, bind_port mode never receives ARP replies (eth.elf
        // handles them), so eth_drv_send falls back to FF:FF:FF:FF:FF:FF and
        // Linux drops the SYN in tcp_v4_rcv before even counting it in InSegs.
        static const unsigned char gw_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
        net_arp_set(server_ip, gw_mac);

        memset(sockets, 0, sizeof(sockets));
        sock = tcp_connect(sockets, server_ip, server_port, LOCAL_PORT, my_ip);
        if (!sock)
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
                unsigned char b = server_ip[o];
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
            char tb[6];
            int ti = 0;
            unsigned short tp = server_port;
            if (!tp)
            {
                tb[ti++] = '0';
            }
            else
            {
                while (tp)
                {
                    tb[ti++] = '0' + tp % 10;
                    tp /= 10;
                }
            }
            for (int k = ti - 1; k >= 0 && i < MSG_W; k--)
                msg[i++] = tb[k];
            if (i < MSG_W)
                msg[i++] = ']';
            msg[i > MSG_W ? MSG_W : i] = '\0';
            addLine(msg);
        }
    }

    void pollNet()
    {
        if (!sock)
            return;
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
                if (sock->state == SOCK_ESTABLISHED || sock->rx_len > 0)
                {
                    static unsigned char rx[1025];
                    unsigned int n = chat_sock_read(sock, rx, 1024);
                    if (n > 0)
                    {
                        rx[n] = '\0';
                        unsigned int off = 0;
                        while (off < n)
                        {
                            static char linebuf[514];
                            // prepend saved partial from previous segment
                            int li = ircPartialLen;
                            for (int pi = 0; pi < ircPartialLen; pi++)
                                linebuf[pi] = ircPartial[pi];
                            ircPartialLen = 0;
                            bool hitNewline = false;
                            while (off < n && li < 512)
                            {
                                unsigned char c = rx[off++];
                                if (c == '\n') { hitNewline = true; break; }
                                if (c == '\r') continue;
                                linebuf[li++] = (char)c;
                            }
                            if (hitNewline)
                            {
                                if (li > 0)
                                {
                                    linebuf[li] = '\0';
                                    processLine(linebuf);
                                }
                            }
                            else if (off < n)
                            {
                                // li >= 512 but data remains: line too long, resync at next \n
                                ircPartialLen = 0;
                                while (off < n && rx[off] != '\n')
                                    off++;
                                if (off < n) off++; // skip the \n
                            }
                            else
                            {
                                // segment ended mid-line — save for next pollNet call
                                ircPartialLen = li;
                                for (int i = 0; i < li; i++)
                                    ircPartial[i] = linebuf[i];
                                if (li < 514) ircPartial[li] = '\0';
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!connected && sock && !sock->used)
        {
            sock = tcp_connect(sockets, server_ip, server_port, LOCAL_PORT, my_ip);
            synRetries = 0;
        }
        if (!connected && sock && sock->state == SOCK_SYN_SENT)
        {
            if (++synRetries % 5 == 0)
            {
                sock->seq_num = 0;
                send_tcp_packet_c(sock, nullptr, 0, 0x02);
                sock->seq_num = 1;
            }
        }
        if (!connected && sock && sock->state == SOCK_ESTABLISHED)
        {
            connected = true;
            synRetries = 0;
            addLine("[connected]");
            char nbuf[32];
            int i = 0;
            const char *n = "NICK ";
            while (*n)
                nbuf[i++] = *n++;
            int ni = 0;
            while (nick[ni] && i < 30)
                nbuf[i++] = nick[ni++];
            nbuf[i] = '\0';
            ircSend(nbuf);
            char ubuf[64];
            i = 0;
            const char *u = "USER ";
            while (*u)
                ubuf[i++] = *u++;
            ni = 0;
            while (nick[ni] && i < 28)
                ubuf[i++] = nick[ni++];
            const char *us = " 0 * :r2 IRC";
            while (*us && i < 62)
                ubuf[i++] = *us++;
            ubuf[i] = '\0';
            ircSend(ubuf);
            wnd->Repaint();
        }
        if (connected && sock && sock->state == SOCK_CLOSED)
        {
            connected = false;
            registered = false;
            ircPartialLen = 0;
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
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (phase != PH_CHAT ||
                data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            if (F_COORD(mx) < userPaneX - 3 || userPaneX == 0)
                return;
            int lastRowY = 17 + (userRowsShown - 1) * 10;
            if (my >= 4 && my < 15)
            {
                userScroll -= userRowsShown;
                clampUserScroll();
                wnd->Repaint();
            }
            else if (my >= lastRowY && my < lastRowY + 10)
            {
                userScroll += userRowsShown;
                clampUserScroll();
                wnd->Repaint();
            }
            return;
        }
        if (data->type != PlatformWindowInputEventType::OnKeyEvent)
            return;
        auto *key = data->Data.OnKeyEvent.key;
        if (!key->isKeyDown)
            return;

        if (key->isEscape)
        {
            if (phase == PH_NICK)
            {
                phase = PH_IP;
                inputLen = 0;
                inputBuf[0] = '\0';
                wnd->Repaint();
                return;
            }
            if (phase == PH_CHAN)
            {
                phase = PH_NICK;
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
            if (sock)
                chat_sock_close(sock);
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
                    for (int i = 0; i < 4; i++)
                        server_ip[i] = tmp[i];
                phase = PH_NICK;
                inputLen = 0;
                inputBuf[0] = '\0';
                wnd->Repaint();
                return;
            }
            if (phase == PH_NICK)
            {
                if (inputLen > 0)
                {
                    int i = 0;
                    while (i < inputLen && i < 16)
                    {
                        nick[i] = inputBuf[i];
                        i++;
                    }
                    nick[i] = '\0';
                }
                else
                {
                    scopy(nick, "r2user", sizeof(nick));
                }
                phase = PH_CHAN;
                inputLen = 0;
                inputBuf[0] = '\0';
                wnd->Repaint();
                return;
            }
            if (phase == PH_CHAN)
            {
                if (inputLen > 0)
                {
                    int i = 0;
                    if (inputBuf[0] != '#')
                        channel[i++] = '#';
                    int j = 0;
                    while (j < inputLen && i < 32)
                    {
                        channel[i++] = inputBuf[j++];
                    }
                    channel[i] = '\0';
                }
                else
                {
                    scopy(channel, "#r2", sizeof(channel));
                }
                inputLen = 0;
                inputBuf[0] = '\0';
                phase = PH_CHAT;
                initNet();
                wnd->SetImmediateMode(true);
                imEnabled = true;
                wnd->Repaint();
                return;
            }
            // PH_CHAT: send message
            if (inputLen == 0)
                return;
            // /quit
            if (inputLen >= 5 && inputBuf[0] == '/' && inputBuf[1] == 'q' &&
                inputBuf[2] == 'u' && inputBuf[3] == 'i' && inputBuf[4] == 't')
            {
                ircSend("QUIT :Bye");
                if (imEnabled)
                {
                    wnd->SetImmediateMode(false);
                    imEnabled = false;
                }
                if (sock)
                    chat_sock_close(sock);
                wnd->Close();
                return;
            }
            if (connected && registered)
            {
                char sbuf[256];
                int i = 0;
                const char *p = "PRIVMSG ";
                while (*p && i < 250)
                    sbuf[i++] = *p++;
                int ci = 0;
                while (channel[ci] && i < 250)
                    sbuf[i++] = channel[ci++];
                if (i < 250)
                {
                    sbuf[i++] = ' ';
                    sbuf[i++] = ':';
                }
                int mi = 0;
                while (mi < inputLen && i < 252)
                    sbuf[i++] = inputBuf[mi++];
                sbuf[i] = '\0';
                ircSend(sbuf);
                // Echo locally
                char disp[MSG_W + 1];
                int di = 0;
                disp[di++] = '<';
                int ni = 0;
                while (nick[ni] && di < 14)
                    disp[di++] = nick[ni++];
                disp[di++] = '>';
                disp[di++] = ' ';
                mi = 0;
                while (mi < inputLen && di < MSG_W)
                    disp[di++] = inputBuf[mi++];
                disp[di] = '\0';
                addLine(disp);
            }
            inputLen = 0;
            inputBuf[0] = '\0';
            wnd->Repaint();
            return;
        }
        // Ctrl and an arrow scroll the user list; the arrows on their own
        // belong to the messages, which is the pane you are usually reading.
        bool ctrlHeld = key->isLeftControl || key->isRightControl;
        if (phase == PH_CHAT && ctrlHeld && (key->isArrowUp || key->isPageUp))
        {
            userScroll -= key->isPageUp ? userRowsShown : 1;
            clampUserScroll();
            wnd->Repaint();
            return;
        }
        if (phase == PH_CHAT && ctrlHeld && (key->isArrowDown || key->isPageDown))
        {
            userScroll += key->isPageDown ? userRowsShown : 1;
            clampUserScroll();
            wnd->Repaint();
            return;
        }
        if (phase == PH_CHAT && key->isArrowUp)
        {
            int maxScroll = msgTotal > VIS_ROWS ? msgTotal - VIS_ROWS : 0;
            if (scrollOffset < maxScroll)
                scrollOffset++;
            wnd->Repaint();
            return;
        }
        if (phase == PH_CHAT && key->isArrowDown)
        {
            if (scrollOffset > 0)
                scrollOffset--;
            wnd->Repaint();
            return;
        }
        if (phase == PH_CHAT && key->isPageUp)
        {
            int maxScroll = msgTotal > VIS_ROWS ? msgTotal - VIS_ROWS : 0;
            scrollOffset += VIS_ROWS;
            if (scrollOffset > maxScroll)
                scrollOffset = maxScroll;
            wnd->Repaint();
            return;
        }
        if (phase == PH_CHAT && key->isPageDown)
        {
            scrollOffset -= VIS_ROWS;
            if (scrollOffset < 0)
                scrollOffset = 0;
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


        // Client area only: the root frames the window and names it.
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        opts.horizontalAlign = PlatformAlign::Begin;

        if (phase != PH_CHAT)
        {
            const char *prompt = "";
            const char *hint = "";
            const char *defval = "";
            if (phase == PH_IP)
            {
                prompt = "IRC server IP:";
                hint = "default 10.3.4.1   [Enter] next   [Esc] close";
                defval = "10.3.4.1";
            }
            else if (phase == PH_NICK)
            {
                prompt = "Nick:";
                hint = "default r2user   [Enter] next   [Esc] back";
                defval = "r2user";
            }
            else
            {
                prompt = "Channel:";
                hint = "default #r2   [Enter] connect   [Esc] back";
                defval = "#r2";
            }
            target->DrawText(6, 6, W - 12, 10, prompt, &opts, false);
            target->FillRect(20, 20, W - 40, 14, dark, false);
            target->FillRect(22, 22, W - 44, 10, light, false);
            char display[IN_CAP + 3] = {};
            int i = 0;
            if (inputLen == 0)
            {
                while (defval[i] && i < IN_CAP)
                {
                    display[i] = defval[i];
                    i++;
                }
            }
            else
            {
                while (inputBuf[i] && i < IN_CAP)
                {
                    display[i] = inputBuf[i];
                    i++;
                }
            }
            display[i++] = '_';
            display[i] = '\0';
            target->DrawText(26, 22, W - 52, 10, display, &opts, false);
            target->DrawText(6, 38, W - 12, 10, hint, &opts, false);
            if (phase == PH_NICK || phase == PH_CHAN)
            {
                char ipstr[26] = "Server: ";
                int k = 8;
                for (int o = 0; o < 4 && k < 24; o++)
                {
                    unsigned char b = server_ip[o];
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
            if (phase == PH_CHAN)
            {
                char nkstr[24] = "Nick: ";
                int k = 6;
                int ni = 0;
                while (nick[ni] && k < 22)
                    nkstr[k++] = nick[ni++];
                nkstr[k] = '\0';
                target->DrawText(6, 62, W - 12, 10, nkstr, &opts, false);
            }
        }
        else
        {
            target->FillRect(2, H - 24, W - 4, 1, dark, false);

            // Status: nick @ channel or connection state
            char status[MSG_W + 4] = {};
            int si = 0;
            if (connected && registered)
            {
                int ni = 0;
                while (nick[ni] && si < 16)
                    status[si++] = nick[ni++];
                status[si++] = ' ';
                status[si++] = '@';
                status[si++] = ' ';
                int ci = 0;
                while (channel[ci] && si < MSG_W)
                    status[si++] = channel[ci++];
            }
            else if (connected)
            {
                const char *s = "logging in...";
                while (*s && si < MSG_W)
                    status[si++] = *s++;
            }
            else
            {
                const char *s = "connecting...";
                while (*s && si < MSG_W)
                    status[si++] = *s++;
            }
            status[si] = '\0';
            // Append register count "(N)" to status
            if (regCount > 0)
            {
                if (si < MSG_W - 4) status[si++] = ' ';
                if (si < MSG_W - 3) status[si++] = '(';
                if (regCount >= 10 && si < MSG_W - 2) status[si++] = '0' + regCount / 10;
                if (si < MSG_W - 1) status[si++] = '0' + regCount % 10;
                if (si < MSG_W) status[si++] = ')';
                status[si] = '\0';
            }
            target->DrawText(4, H - 23, W - 8, 9, status, &opts, false);

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

            // The messages on the left, who is here on the right, with a rule
            // between them that stops where the status line starts.
            const int usersX = F_COORD(W) - USERS_W;
            userPaneX = usersX;
            const int msgW = usersX - 8;
            target->FillRect(usersX - 3, 2, 1, F_COORD(H) - 27, dark, false);

            int startMsg = msgTotal - VIS_ROWS - scrollOffset;
            if (startMsg < 0)
                startMsg = 0;
            for (int r = 0; r < VIS_ROWS; r++)
            {
                int idx = startMsg + r;
                if (idx >= msgTotal)
                    break;
                target->DrawText(4, 4 + r * 10, msgW, 10, msgs[idx % MAX_MSGS], &opts, false);
            }
            // Scroll indicator
            if (scrollOffset > 0)
            {
                char ind[8] = "^ ";
                int k = 2;
                int so = scrollOffset;
                if (so >= 10) ind[k++] = '0' + so / 10;
                ind[k++] = '0' + so % 10;
                ind[k] = '\0';
                opts.horizontalAlign = PlatformAlign::End;
                target->DrawText(4, 4, msgW, 10, ind, &opts, false);
                opts.horizontalAlign = PlatformAlign::Begin;
            }

            // Who is in the channel. The header carries the count, which is
            // the part worth knowing when the list is longer than the window.
            char head[16];
            {
                int k = 0;
                const char *u = "Users ";
                while (*u)
                    head[k++] = *u++;
                int n = nUsers;
                if (n >= 100)
                    head[k++] = (char)('0' + n / 100);
                if (n >= 10)
                    head[k++] = (char)('0' + (n / 10) % 10);
                head[k++] = (char)('0' + n % 10);
                head[k] = '\0';
            }
            target->DrawText(usersX, 4, USERS_W, 10, (const mchar *)head, &opts, false);
            target->FillRect(usersX, 14, USERS_W - 2, 1, dark, false);

            userRowsShown = (F_COORD(H) - 27 - 17) / 10;
            if (userRowsShown < 1)
                userRowsShown = 1;
            clampUserScroll();

            for (int i = 0; i < userRowsShown; i++)
            {
                int at = userScroll + i;
                if (at >= nUsers)
                    break;
                target->DrawText(usersX, 17 + i * 10, USERS_W, 10, (const mchar *)users[at], &opts, false);
            }

            // Where the list runs past the pane, at its two ends: the same
            // marks the file manager uses, and the same click targets — the
            // row they are on scrolls the list by a pane-full.
            opts.horizontalAlign = PlatformAlign::End;
            if (userScroll > 0)
                target->DrawText(usersX, 4, USERS_W - 2, 10, "^", &opts, false);
            if (userScroll < userMaxScroll())
                target->DrawText(usersX, 17 + (userRowsShown - 1) * 10, USERS_W - 2, 10, "v",
                                 &opts, false);
            opts.horizontalAlign = PlatformAlign::Begin;
        }
    }

public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<IRCWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }
};
