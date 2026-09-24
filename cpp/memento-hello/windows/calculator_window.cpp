//
// Window 6 — Calculator
//

class CalculatorWindow
{
    // ── arithmetic state ──────────────────────────────────────────────────────
    int64_t lhs = 0;    // committed left-hand side
    char pendingOp = 0; // '+', '-', '*', '/', 0 = none
    char inp[20] = {'0', '\0'};
    int inpLen = 1;
    bool fresh = false;       // next digit replaces current input
    bool resultSt = false;    // showing result of last =
    bool err = false;         // division by zero
    Coord panX = 0, panY = 0; // the client area starts at the corner

    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;

    // ── helpers ───────────────────────────────────────────────────────────────

    static int slen(const char *s)
    {
        int n = 0;
        while (s[n])
            ++n;
        return n;
    }

    static int64_t parseInp(const char *s)
    {
        int64_t v = 0;
        int i = 0;
        bool neg = (s[i] == '-');
        if (neg)
            ++i;
        while (s[i] >= '0' && s[i] <= '9')
            v = v * 10 + (s[i++] - '0');
        return neg ? -v : v;
    }

    static void fmtNum(int64_t v, char *buf, int max)
    {
        if (max < 2)
            return;
        if (v == 0)
        {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        bool neg = v < 0;
        if (neg)
            v = -v;
        char tmp[20];
        int ti = 0;
        while (v > 0 && ti < 19)
        {
            tmp[ti++] = '0' + (int)(v % 10);
            v /= 10;
        }
        int i = 0;
        if (neg)
            buf[i++] = '-';
        for (int j = ti - 1; j >= 0 && i < max - 1; j--)
            buf[i++] = tmp[j];
        buf[i] = '\0';
    }

    static int64_t compute(int64_t a, char op, int64_t b, bool &outErr)
    {
        if (op == '+')
            return a + b;
        if (op == '-')
            return a - b;
        if (op == '*')
            return a * b;
        if (op == '/')
        {
            if (b == 0)
            {
                outErr = true;
                return 0;
            }
            return a / b;
        }
        return a;
    }

    void commitOp(char newOp)
    {
        // Apply any pending operation, store result in lhs, set new pending op.
        int64_t cur = parseInp(inp);
        if (pendingOp != 0 && !fresh)
        {
            int64_t result = compute(lhs, pendingOp, cur, err);
            if (!err)
            {
                fmtNum(result, inp, 20);
                inpLen = slen(inp);
                lhs = result;
            }
            else
            {
                inp[0] = 'E';
                inp[1] = 'r';
                inp[2] = 'r';
                inp[3] = '\0';
                inpLen = 3;
            }
        }
        else
        {
            lhs = cur;
        }
        pendingOp = newOp;
        fresh = true;
        resultSt = false;
    }

    void doEqual()
    {
        if (err)
        {
            doClear();
            return;
        }
        int64_t cur = parseInp(inp);
        int64_t result = (pendingOp != 0) ? compute(lhs, pendingOp, cur, err) : cur;
        if (!err)
        {
            fmtNum(result, inp, 20);
            inpLen = slen(inp);
            lhs = result;
        }
        else
        {
            inp[0] = 'E';
            inp[1] = 'r';
            inp[2] = 'r';
            inp[3] = '\0';
            inpLen = 3;
        }
        pendingOp = 0;
        fresh = true;
        resultSt = true;
    }

    void doClear()
    {
        lhs = 0;
        pendingOp = 0;
        inp[0] = '0';
        inp[1] = '\0';
        inpLen = 1;
        fresh = false;
        resultSt = false;
        err = false;
    }

    void appendDigit(char d)
    {
        if (err)
        {
            doClear();
        }
        if (fresh || resultSt)
        {
            inp[0] = d;
            inp[1] = '\0';
            inpLen = 1;
            fresh = false;
            resultSt = false;
        }
        else if (inpLen < 18)
        {
            inp[inpLen++] = d;
            inp[inpLen] = '\0';
        }
    }

    void delDigit()
    {
        if (err || resultSt)
        {
            doClear();
            return;
        }
        if (inpLen > 1)
        {
            inp[--inpLen] = '\0';
        }
        else
        {
            inp[0] = '0';
            inp[1] = '\0';
            inpLen = 1;
        }
    }

    // ── event + paint ─────────────────────────────────────────────────────────

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        }
        if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state != PlatformWindowButtonState::Pressed)
                return;
            Coord mx = data->Data.OnMouseClick.mouseX;
            Coord my = data->Data.OnMouseClick.mouseY;
            // Keypad hit test — coords relative to panX/panY
            const int CX[4] = {F_COORD(panX) + 3, F_COORD(panX) + 42, F_COORD(panX) + 81, F_COORD(panX) + 120};
            const int CW = 36;
            const int RY[4] = {F_COORD(panY) + 21, F_COORD(panY) + 35, F_COORD(panY) + 49, F_COORD(panY) + 63};
            const int RH = 11;
            for (int r = 0; r < 4; r++)
            {
                if (my < RY[r] || my >= RY[r] + RH)
                    continue;
                for (int c = 0; c < 4; c++)
                {
                    if (mx < CX[c] || mx >= CX[c] + CW)
                        continue;
                    if (c < 3)
                    {
                        char d = (char)('7' - r * 3 + c);
                        if (r == 3)
                            d = (c == 0) ? '0' : 0;
                        if (d)
                            appendDigit(d);
                        else if (r == 3 && c == 1)
                            doClear();
                        else if (r == 3 && c == 2)
                            doEqual();
                    }
                    else
                    {
                        const char ops[4] = {'*', '/', '-', '+'};
                        commitOp(ops[r]);
                    }
                    wnd->Repaint();
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
            wnd->Close();
            return;
        }
        if (key->isEnter)
        {
            doEqual();
            wnd->Repaint();
            return;
        }
        if (key->isBackspace)
        {
            delDigit();
            wnd->Repaint();
            return;
        }

        if (key->isChar)
        {
            char c = (char)key->theChar;
            if (c >= '0' && c <= '9')
            {
                appendDigit(c);
                wnd->Repaint();
                return;
            }
            if (c == '+')
            {
                commitOp('+');
                wnd->Repaint();
                return;
            }
            if (c == '-')
            {
                commitOp('-');
                wnd->Repaint();
                return;
            }
            if (c == '*')
            {
                commitOp('*');
                wnd->Repaint();
                return;
            }
            if (c == '/')
            {
                commitOp('/');
                wnd->Repaint();
                return;
            }
            if (c == '=')
            {
                doEqual();
                wnd->Repaint();
                return;
            }
            if (c == 'c' || c == 'C')
            {
                doClear();
                wnd->Repaint();
                return;
            }
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

        Coord W = target->GetWidth();
        Coord H = target->GetHeight();

        // The client area is the calculator itself; the root draws the frame,
        // the title bar and the line on the taskbar.
        target->FillRect(0, 0, W, H, light, false);

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        // ── Display area ──
        target->FillRect(panX + 3, panY + 2, 154, 14, dark, false);

        // left: "lhs op" context
        if (pendingOp != 0)
        {
            char ctx[24];
            int ci = 0;
            char lhsBuf[20];
            fmtNum(lhs, lhsBuf, 20);
            for (int i = 0; lhsBuf[i] && ci < 20; i++)
                ctx[ci++] = lhsBuf[i];
            ctx[ci++] = ' ';
            ctx[ci++] = pendingOp;
            ctx[ci] = '\0';
            PlatformDrawTextOptions lo{};
            lo.font = font;
            lo.foreground = light;
            lo.horizontalAlign = PlatformAlign::Begin;
            lo.verticalAlign = PlatformAlign::Middle;
            target->DrawText(panX + 5, panY + 2, 80, 14, ctx, &lo, false);
        }

        // right: current number or "Error"
        opts.horizontalAlign = PlatformAlign::End;
        opts.foreground = light;
        target->DrawText(panX + 3, panY + 2, 150, 14, err ? "Error" : inp, &opts, false);

        target->FillRect(panX + 2, panY + 18, 156, 1, dark, false); // divider below display

        // ── Keypad (4 columns × 4 rows) ──
        // Offsets: col x = {panX+3, panX+54, panX+105, panX+156}, w=46; row y = {panY+44..+89}, h=13
        const int CX[4] = {F_COORD(panX) + 3, F_COORD(panX) + 42, F_COORD(panX) + 81, F_COORD(panX) + 120};
        const int CW = 36;
        const int RY[4] = {F_COORD(panY) + 21, F_COORD(panY) + 35, F_COORD(panY) + 49, F_COORD(panY) + 63};
        const int RH = 11;

        const char *labels[4][4] = {
            {"7", "8", "9", "*"},
            {"4", "5", "6", "/"},
            {"1", "2", "3", "-"},
            {"0", "C", "=", "+"},
        };
        const char ops[4] = {'*', '/', '-', '+'}; // operator for col 3 per row

        for (int r = 0; r < 4; r++)
        {
            for (int c = 0; c < 4; c++)
            {
                // highlight the active operator button (col 3 only)
                bool hi = (c == 3) && (pendingOp == ops[r]) && !resultSt;
                PlatformColor *bg = hi ? light : dark;
                target->FillRect(CX[c], RY[r], CW, RH, bg, false);

                PlatformDrawTextOptions to{};
                to.font = font;
                to.foreground = hi ? dark : light;
                to.horizontalAlign = PlatformAlign::Middle;
                to.verticalAlign = PlatformAlign::Middle;
                target->DrawText(CX[c], RY[r], CW, RH, labels[r][c], &to, false);
            }
        }

        // Hint line
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.foreground = dark;
        target->DrawText(panX + 2, panY + 77, 156, 9, "Bcsp del   C clear   Esc close", &opts, false);
    }

public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<CalculatorWindow *>(instance)->onEvent_(data);
    }
    void SetWindow(PlatformWindow *w) { wnd = w; }
};
