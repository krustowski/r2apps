//
// A row of tabs along the top of a window's client area: the Tasks window's
// and the Network window's.  The tab showing is taller and open into the page
// under it; the others are shorter and closed off by the strip's line.  A
// note after them, against the window's right edge, says something about the
// page.  The tabs keep their place whatever the window's width: only the note
// moves.
//

struct TabStrip
{
    static const int X = 4, Y = 2; // the first tab
    static const int W = 44, H = 11, GAP = 2;
    static const int LINE_Y = Y + H;     // the strip's bottom line
    static const int BELOW = LINE_Y + 3; // where the page under it starts

    static void draw(PlatformBitmap *target, PlatformDrawTextOptions &opts, PlatformColor *dark, PlatformColor *light,
                     int winW, const char *const *labels, int n, int active, const char *note)
    {
        target->FillRect(2, LINE_Y, winW - 4, 1, dark, false);
        opts.foreground = dark;
        opts.verticalAlign = PlatformAlign::Middle;
        opts.horizontalAlign = PlatformAlign::Middle;
        int x = X;
        for (int i = 0; i < n; i++, x += W + GAP)
        {
            bool on = i == active;
            int y = on ? Y : Y + 2;
            target->FillRect(x, y, W, 1, dark, false);
            target->FillRect(x, y, 1, LINE_Y - y, dark, false);
            target->FillRect(x + W - 1, y, 1, LINE_Y - y, dark, false);
            if (on)
                target->FillRect(x + 1, LINE_Y, W - 2, 1, light, false);
            target->DrawText(x, y + 1, W, LINE_Y - y - 1, (const mchar *)labels[i], &opts, false);
        }
        int from = x + 4;
        if (note && note[0] && winW - 6 > from)
        {
            opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(from, Y + 1, winW - 6 - from, LINE_Y - Y - 1, (const mchar *)note, &opts, false);
        }
        opts.horizontalAlign = PlatformAlign::Begin;
    }

    // The tab a click at (mx, my) landed on, or -1.
    static int at(int mx, int my, int n)
    {
        if (my < Y || my > LINE_Y)
            return -1;
        for (int i = 0; i < n; i++)
        {
            int x = X + i * (W + GAP);
            if (mx >= x && mx < x + W)
                return i;
        }
        return -1;
    }
};
