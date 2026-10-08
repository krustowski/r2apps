//
// Window 2 — Login dialog (username + password), and the lock screen
//

#include "../web/tls.h"

//
//  The session's credentials.  The first login since the machine started
//  that gives any sets them: a salted hash of the login and the password goes
//  to SESSION.CFG in the RAM disk, which lasts until the next restart, and to
//  g_session.  Any Memento started after that --- this one after a logout, or
//  another --- has to be given the same pair, and so does the lock screen
//  (Esc on the desktop, or Alt+L) to give the session back.
//
//  Until then the system is root with no password.  Empty fields, or "root"
//  and no password, are that and set nothing: no SESSION.CFG, so tnt asks a
//  new connection nothing either, and the next login --- the lock screen's
//  too --- can still give the session its credentials.  Empty fields were a
//  pair like any other once, and the lock screen of a session begun with
//  them took nothing but Enter.
//
//  The hash is SHA-256 run SESSION_ROUNDS times over the salt, the login and
//  the password, so that trying passwords against the file is slow; the
//  salt is the cycle counter and the time since boot, which is all a salt
//  needs to be.
//
static const char SESSION_PATH[] = "/mnt/tmp/SESSION.CFG";
static const int SESSION_ROUNDS = 4096;

static struct
{
    bool set;       // a login has been accepted
    char salt[33];  // hex
    char hash[65];  // hex
} g_session = {};

static void sessionHex(const unsigned char *in, int n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < n; i++)
    {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

static void sessionDigest(const char *salt, const char *login, const char *pass, char hex[65])
{
    unsigned char buf[32 + 32 + 64 + 1 + 64];
    unsigned char h[32] = {};
    size_t sl = strlen(salt), ll = strlen(login), pl = strlen(pass);
    for (int round = 0; round < SESSION_ROUNDS; round++)
    {
        size_t n = 0;
        memcpy(buf + n, h, 32), n += 32;
        memcpy(buf + n, salt, sl), n += sl;
        memcpy(buf + n, login, ll), n += ll;
        buf[n++] = '\n';
        memcpy(buf + n, pass, pl), n += pl;
        web_sha256(buf, (unsigned long)n, h);
    }
    memset(buf, 0, sizeof(buf));
    sessionHex(h, 32, hex);
}

//  `key=` from SESSION.CFG, `len` hex digits of it, into `out`.
static bool sessionField(const char *text, const char *key, char *out, size_t len)
{
    const char *p = strstr(text, key);
    if (!p)
        return false;
    p += strlen(key);
    for (size_t i = 0; i < len; i++)
    {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        out[i] = c;
    }
    out[len] = 0;
    return true;
}

//  Root with no password, the system before anyone gives credentials.
static bool sessionIsDefault(const char *login, const char *pass)
{
    return !pass[0] && (!login[0] || !strcmp(login, "root"));
}

//  The session's hash, from memory or else from SESSION.CFG.
enum SessionState
{
    SESSION_NONE, // nobody has given credentials since boot
    SESSION_OK,   // g_session holds it
    SESSION_BAD   // SESSION.CFG is there but makes no sense: nobody gets in
                  // (tnt's auth.c says the same), rather than the next pair
                  // typed becoming the session's
};

static SessionState sessionLoad()
{
    if (g_session.set)
        return SESSION_OK;
    auto text = r2::fs::read_text(SESSION_PATH, 512);
    if (!text || !text->c_str()[0])
        return SESSION_NONE;
    if (!sessionField(text->c_str(), "salt=", g_session.salt, 32) ||
        !sessionField(text->c_str(), "hash=", g_session.hash, 64))
        return SESSION_BAD;
    //  The default credentials, kept by a Memento from before they set
    //  nothing: still no session (tnt's auth.c reads the file the same way).
    char hex[65];
    sessionDigest(g_session.salt, "", "", hex);
    bool none = !strcmp(hex, g_session.hash);
    if (!none)
    {
        sessionDigest(g_session.salt, "root", "", hex);
        none = !strcmp(hex, g_session.hash);
    }
    if (none)
        return SESSION_NONE;
    g_session.set = true;
    return SESSION_OK;
}

//  The first login: these become the session's credentials.  Kept in memory
//  even when the RAM disk will not take the file.
static void sessionCreate(const char *login, const char *pass)
{
    unsigned char seed[16];
    uint64_t t = r2::ticks();
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    for (int i = 0; i < 8; i++)
    {
        seed[i] = (unsigned char)(tsc >> (8 * i));
        seed[8 + i] = (unsigned char)(t >> (8 * i));
    }
    unsigned char mixed[32];
    web_sha256(seed, sizeof(seed), mixed);
    sessionHex(mixed, 16, g_session.salt);
    sessionDigest(g_session.salt, login, pass, g_session.hash);
    g_session.set = true;

    char text[256]; // 162 bytes of it, and the hash must not be cut short
    web::scopy(text, "# Memento session: the first login since boot, hashed\nsalt=", sizeof(text));
    web::scat(text, g_session.salt, sizeof(text));
    web::scat(text, "\nhash=", sizeof(text));
    web::scat(text, g_session.hash, sizeof(text));
    web::scat(text, "\n", sizeof(text));
    r2::string_view path(SESSION_PATH, strlen(SESSION_PATH));
    (void)r2::fs::remove(path);
    (void)r2::fs::write_at(path, r2::const_byte_span((const unsigned char *)text, strlen(text)), 0);
}

//  Whether these are the session's credentials.  Before there are any,
//  every pair gets in: the default one as root with no password, which sets
//  nothing, and the first other one becomes the session's.
static bool sessionAccepts(const char *login, const char *pass)
{
    SessionState st = sessionLoad();
    if (st == SESSION_NONE)
    {
        if (!sessionIsDefault(login, pass))
            sessionCreate(login, pass);
        return true;
    }
    if (st == SESSION_BAD)
        return false;
    char hex[65];
    sessionDigest(g_session.salt, login, pass, hex);
    return !strcmp(hex, g_session.hash);
}

//  The login as the system's user (syscall 0x01, 0x03), for the shells'
//  prompts and anything else that asks the kernel who is there.  The kernel
//  takes one word of printable ASCII, so a space or anything else becomes
//  '_', and 31 characters of it.  An empty login is root.
static void sessionSetUser(const char *login)
{
    if (!login[0])
        login = "root";
    char name[32];
    size_t n = 0;
    for (; login[n] && n < sizeof(name) - 1; n++)
        name[n] = (login[n] > ' ' && login[n] < 0x7f) ? login[n] : '_';
    name[n] = 0;
    (void)r2::set_user(r2::string_view(name, n));
}

class LoginWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<LoginWindow *>(instance)->onEvent_(data);
    }

    //  As the lock screen: Unlock gives the session back by closing the
    //  window, Restart and Power off end it with the computer, and Esc only
    //  clears the fields.
    explicit LoginWindow(bool lockScreen = false) : locking(lockScreen) {}

    void SetWindow(PlatformWindow *w) { wnd = w; }

    bool wantsDesktop = false;

private:
    const bool locking;
    bool wrong = false; // the last try did not match the session's
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    int focus = 0; // 0=login field, 1=password field, 2.. the buttons

    static const int MAX_LEN = 63;
    char loginBuf[64] = {};
    char passBuf[64] = {};
    int loginLen = 0;
    int passLen = 0;
    // Panel metrics, in the window's 320x200 coordinates and sized around the
    // 4x8 glyph: a title bar of one line, fields and buttons of one line and a
    // border. Nothing here is wider than what it holds — the widest label is
    // "Password:" at nine characters, and a login does not need a field
    // twenty-two characters across. The painter and the hit tests share them.
    static const int PAN_H = 60;
    static const int TITLE_H = 10;
    static const int LABEL_X = 6, LABEL_W = 38;
    static const int FIELD_X = 46, FIELD_H = 10;
    static const int ROW1_Y = 15, ROW2_Y = 28;
    static const int BTN_Y = 44, BTN_H = 11;

    //  The buttons, left to right.  The login has OK and Cancel; the lock
    //  screen has three, in a panel made wider for them.
    struct Button
    {
        int x, w;
        const char *label;
    };
    static constexpr Button LoginButtons[2] = {{18, 32, "OK"}, {60, 40, "Cancel"}};
    static constexpr Button LockButtons[3] = {{8, 36, "Unlock"}, {50, 40, "Restart"}, {96, 46, "Power off"}};
    enum
    {
        BTN_OK,      // OK / Unlock
        BTN_CANCEL,  // Cancel / Restart
        BTN_POWEROFF // the lock screen only
    };

    const Button *buttons(int &n) const
    {
        n = locking ? 3 : 2;
        return locking ? LockButtons : LoginButtons;
    }
    int stops() const // what the arrows go round: the two fields and the buttons
    {
        int n;
        buttons(n);
        return 2 + n;
    }
    int panW() const { return locking ? 150 : 116; }
    int fieldW() const { return panW() - FIELD_X - 6; }

    Coord panX = 102, panY = 70; // panel origin; drag title bar to reposition
    int screenW = 320, screenH = 200; // the window's size, from the last paint
    bool placed = false;              // centred on it once, then where dragged
    bool dragging = false;
    Coord dragMX0 = 0, dragMY0 = 0, dragPX0 = 0, dragPY0 = 0;

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseMove)
        {
            if (!dragging)
                return;
            Coord mx = data->Data.OnMouseMove.mouseX;
            Coord my = data->Data.OnMouseMove.mouseY;
            Coord nx = dragPX0 + (mx - dragMX0);
            Coord ny = dragPY0 + (my - dragMY0);
            if (nx < 0)
                nx = 0;
            if (nx > screenW - panW())
                nx = screenW - panW();
            if (ny < 0)
                ny = 0;
            if (ny > screenH - PAN_H)
                ny = screenH - PAN_H;
            panX = nx;
            panY = ny;
            wnd->Repaint();
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
            {
                dragging = false;
                return;
            }
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            auto hit = [&](Coord bx, Coord by, Coord bw, Coord bh)
            {
                return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
            };
            if (my >= panY && my < panY + TITLE_H && mx >= panX && mx < panX + panW())
            {
                dragging = true;
                dragMX0 = mx;
                dragMY0 = my;
                dragPX0 = panX;
                dragPY0 = panY;
                return;
            }
            if (hit(panX + FIELD_X, panY + ROW1_Y, fieldW(), FIELD_H))
            {
                focus = 0;
                wnd->Repaint();
                return;
            }
            if (hit(panX + FIELD_X, panY + ROW2_Y, fieldW(), FIELD_H))
            {
                focus = 1;
                wnd->Repaint();
                return;
            }
            int nb;
            const Button *b = buttons(nb);
            for (int i = 0; i < nb; i++)
            {
                if (hit(panX + b[i].x, panY + BTN_Y, b[i].w, BTN_H))
                {
                    press(i);
                    return;
                }
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
            if (locking)
                clearFields(true);
            else
                cancel();
            return;
        }

        // Text input — explicit branches to avoid reference-to-ternary aliasing issues
        if (key->isChar)
        {
            if (focus == 0 && loginLen < MAX_LEN)
            {
                loginBuf[loginLen++] = (char)key->theChar;
                loginBuf[loginLen] = 0;
                wnd->Repaint();
            }
            else if (focus == 1 && passLen < MAX_LEN)
            {
                passBuf[passLen++] = (char)key->theChar;
                passBuf[passLen] = 0;
                wnd->Repaint();
            }
            return;
        }
        if (key->isBackspace)
        {
            if (focus == 0 && loginLen > 0)
            {
                loginBuf[--loginLen] = 0;
                wnd->Repaint();
            }
            else if (focus == 1 && passLen > 0)
            {
                passBuf[--passLen] = 0;
                wnd->Repaint();
            }
            return;
        }

        // Navigation (Tab removed — it generates isTab before isKeyDown is checked)
        if (key->isArrowLeft || key->isArrowRight)
        {
            if (focus >= 2)
            {
                //  Round the buttons only.
                int nb = stops() - 2;
                focus = 2 + (focus - 2 + (key->isArrowRight ? 1 : nb - 1)) % nb;
                wnd->Repaint();
            }
            return;
        }
        if (key->isArrowDown)
        {
            focus = (focus + 1) % stops();
            wnd->Repaint();
            return;
        }
        if (key->isArrowUp)
        {
            focus = (focus + stops() - 1) % stops();
            wnd->Repaint();
            return;
        }
        if (key->isEnter)
        {
            if (focus == 0)
            {
                focus = 1;
                wnd->Repaint();
            }
            else if (focus == 1)
            {
                focus = 2;
                wnd->Repaint();
            }
            else
                press(focus - 2);
        }
    }

    void press(int button)
    {
        if (button == BTN_OK)
            submit();
        else if (button == BTN_CANCEL)
            cancel();
        else if (button == BTN_POWEROFF)
            r2::power_off(); // and the session with it; a kernel without the call does nothing
    }

    void submit()
    {
        if (!sessionAccepts(loginBuf, passBuf))
        {
            wrong = true;
            clearFields(false);
            return;
        }
        sessionSetUser(loginBuf);
        clearFields(true);
        wantsDesktop = true;
        wnd->Close();
    }

    //  Leaving the login restarts the computer (main() does it once the
    //  window is closed); the lock screen does it itself, there being no
    //  main() waiting on it.
    void cancel()
    {
        if (locking)
            r2::reboot();
        wnd->Close();
    }

    //  The password always goes; the login too when `all`.  Focus goes to the
    //  first field left empty.
    void clearFields(bool all)
    {
        memset(passBuf, 0, sizeof(passBuf));
        passLen = 0;
        if (all)
        {
            memset(loginBuf, 0, sizeof(loginBuf));
            loginLen = 0;
        }
        focus = loginLen ? 1 : 0;
        wnd->Repaint();
    }

    void DrawInputField(PlatformBitmap *target, Coord bx, Coord by, Coord bw, Coord bh,
                        const char *buf, int len, bool focused, bool isPassword)
    {
        char display[66] = {};
        int i = 0;

        for (; i < len; i++)
            display[i] = isPassword ? '*' : buf[i];

        if (focused)
            display[i++] = '_';
        display[i] = 0;
        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.horizontalAlign = PlatformAlign::Begin;
        opts.verticalAlign = PlatformAlign::Middle;

        if (focused)
        {
            target->FillRect(bx, by, bw, bh, dark, false);
            opts.foreground = light;
        }
        else
        {
            target->FillRect(bx, by, bw, bh, dark, false);
            target->FillRect(bx + 1, by + 1, bw - 2, bh - 2, light, false);
            opts.foreground = dark;
        }

        target->DrawText(bx + 3, by, bw - 6, bh, (const mchar *)display, &opts, false);
    }

    void DrawButton(PlatformBitmap *target, Coord bx, Coord by, Coord bw, Coord bh,
                    const mchar *label, bool focused)
    {
        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        if (focused)
        {
            target->FillRect(bx, by, bw, bh, dark, false);
            opts.foreground = light;
        }
        else
        {
            target->FillRect(bx, by, bw, bh, dark, false);
            target->FillRect(bx + 1, by + 1, bw - 2, bh - 2, light, false);
            opts.foreground = dark;
        }

        target->DrawText(bx, by, bw, bh, label, &opts, false);
    }

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
        screenW = (int)COORD_VAL(W);
        screenH = (int)COORD_VAL(H);
        if (!placed)
        {
            //  In the middle of whatever size the screen is.
            panX = (screenW - panW()) / 2;
            panY = (screenH - PAN_H) / 2;
            placed = true;
        }

        target->FillRect(0, 0, W, H, dark, false);
        drawWallpaper(dc, target);

        // Dialog panel — panX/panY set initial position, draggable via title bar
        const int PAN_W = panW();
        target->FillRect(panX, panY, PAN_W, PAN_H, dark, false);
        target->FillRect(panX + 2, panY + 2, PAN_W - 4, PAN_H - 4, light, false);
        target->FillRect(panX + 2, panY + TITLE_H, PAN_W - 4, 1, dark, false); // title separator

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        target->DrawText(panX + 2, panY + 2, PAN_W - 4, TITLE_H - 2, locking ? "Locked" : "Login", &opts, false);

        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(panX + LABEL_X, panY + ROW1_Y, LABEL_W, FIELD_H, "Login:", &opts, false);
        target->DrawText(panX + LABEL_X, panY + ROW2_Y, LABEL_W, FIELD_H, "Password:", &opts, false);

        DrawInputField(target, panX + FIELD_X, panY + ROW1_Y, fieldW(), FIELD_H, loginBuf, loginLen, focus == 0, false);
        DrawInputField(target, panX + FIELD_X, panY + ROW2_Y, fieldW(), FIELD_H, passBuf, passLen, focus == 1, true);

        int nb;
        const Button *b = buttons(nb);
        for (int i = 0; i < nb; i++)
            DrawButton(target, panX + b[i].x, panY + BTN_Y, b[i].w, BTN_H, b[i].label, focus == 2 + i);

        //  Under the panel, on the wallpaper: what leaving it does, and why
        //  the last try did not get in.
        opts.foreground = light;
        opts.horizontalAlign = PlatformAlign::Middle;
        if (wrong)
            target->DrawText(panX - 30, panY + PAN_H + 3, PAN_W + 60, 9,
                             "Not this session's login or password", &opts, false);
        target->DrawText(panX - 30, panY + PAN_H + (wrong ? 12 : 3), PAN_W + 60, 9,
                         locking ? "Restart and Power off end the session" : "Esc restarts the computer", &opts,
                         false);
    }
};
