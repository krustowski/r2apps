//
// Window 2 — Login dialog (username + password)
//

class LoginWindow
{
public:
    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<LoginWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w) { wnd = w; }

    bool wantsDesktop = false;

private:
    PlatformWindow *wnd = nullptr;
    PlatformColor *dark = nullptr;
    PlatformColor *light = nullptr;
    PlatformFont *font = nullptr;
    int focus = 0; // 0=login field, 1=password field, 2=OK, 3=Cancel

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
    static const int PAN_W = 116, PAN_H = 60;
    static const int TITLE_H = 10;
    static const int CLOSE_W = 8, CLOSE_H = 6;
    static const int LABEL_X = 6, LABEL_W = 38;
    static const int FIELD_X = 46, FIELD_W = 64, FIELD_H = 10;
    static const int ROW1_Y = 15, ROW2_Y = 28;
    static const int BTN_Y = 44, BTN_H = 11;
    static const int OK_X = 18, OK_W = 32;
    static const int CANCEL_X = 60, CANCEL_W = 40;

    Coord panX = 102, panY = 70; // panel origin; drag title bar to reposition
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
            if (nx > 320 - PAN_W)
                nx = 320 - PAN_W;
            if (ny < 0)
                ny = 0;
            if (ny > 200 - PAN_H)
                ny = 200 - PAN_H;
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
            if (my >= panY && my < panY + TITLE_H && mx >= panX && mx < panX + PAN_W - CLOSE_W - 4)
            {
                dragging = true;
                dragMX0 = mx;
                dragMY0 = my;
                dragPX0 = panX;
                dragPY0 = panY;
                return;
            }
            if (hit(panX + PAN_W - CLOSE_W - 3, panY + 3, CLOSE_W, CLOSE_H))
            {
                wnd->Close();
                return;
            }
            if (hit(panX + FIELD_X, panY + ROW1_Y, FIELD_W, FIELD_H))
            {
                focus = 0;
                wnd->Repaint();
                return;
            }
            if (hit(panX + FIELD_X, panY + ROW2_Y, FIELD_W, FIELD_H))
            {
                focus = 1;
                wnd->Repaint();
                return;
            }
            if (hit(panX + OK_X, panY + BTN_Y, OK_W, BTN_H))
            {
                wantsDesktop = true;
                wnd->Close();
                return;
            }
            if (hit(panX + CANCEL_X, panY + BTN_Y, CANCEL_W, BTN_H))
            {
                wnd->Close();
                return;
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
                focus = (focus == 2) ? 3 : 2;
                wnd->Repaint();
            }
            return;
        }
        if (key->isArrowDown)
        {
            focus = (focus + 1) % 4;
            wnd->Repaint();
            return;
        }
        if (key->isArrowUp)
        {
            focus = (focus + 3) % 4;
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
            else if (focus == 2)
            {
                wantsDesktop = true;
                wnd->Close();
            } // OK
            else
            {
                wnd->Close();
            } // Cancel
        }
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

        target->FillRect(0, 0, W, H, dark, false);
        drawWallpaper(dc, target);

        // Dialog panel — panX/panY set initial position, draggable via title bar
        target->FillRect(panX, panY, PAN_W, PAN_H, dark, false);
        target->FillRect(panX + 2, panY + 2, PAN_W - 4, PAN_H - 4, light, false);
        target->FillRect(panX + 2, panY + TITLE_H, PAN_W - 4, 1, dark, false); // title separator
        target->FillRect(panX + PAN_W - CLOSE_W - 3, panY + 3, CLOSE_W, CLOSE_H, dark, false); // close button

        PlatformDrawTextOptions opts{};
        opts.font = font;
        opts.foreground = dark;
        opts.horizontalAlign = PlatformAlign::Middle;
        opts.verticalAlign = PlatformAlign::Middle;

        target->DrawText(panX + 2, panY + 2, PAN_W - CLOSE_W - 8, TITLE_H - 2, "Login", &opts, false);

        opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(panX + LABEL_X, panY + ROW1_Y, LABEL_W, FIELD_H, "Login:", &opts, false);
        target->DrawText(panX + LABEL_X, panY + ROW2_Y, LABEL_W, FIELD_H, "Password:", &opts, false);

        DrawInputField(target, panX + FIELD_X, panY + ROW1_Y, FIELD_W, FIELD_H, loginBuf, loginLen, focus == 0, false);
        DrawInputField(target, panX + FIELD_X, panY + ROW2_Y, FIELD_W, FIELD_H, passBuf, passLen, focus == 1, true);

        DrawButton(target, panX + OK_X, panY + BTN_Y, OK_W, BTN_H, "OK", focus == 2);
        DrawButton(target, panX + CANCEL_X, panY + BTN_Y, CANCEL_W, BTN_H, "Cancel", focus == 3);
    }
};
