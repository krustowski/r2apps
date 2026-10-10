//
// Window 1 — the landing screen
//
//  What Memento shows first: its name under a pocket watch whose hands keep
//  the time, and along the bottom the three ways on.  Enter (or a click)
//  goes to the login, Esc restarts the machine and Shift+Esc switches it
//  off.  The last two are main()'s to do once the window has closed
//  (`choice`), so that on a kernel without the calls Memento ends as it
//  always did.
//
//  Everything is drawn a pixel at a time into the window's bitmap, as the
//  wallpaper picture is, in the palette's own indices.  On the graphics
//  kernel's framebuffer the blues come from the 6x6x6 cube; on the VGA there
//  are only the EGA two (#0000AA and #5555FF), and the shades between them
//  and black or white are checkerboards, a pixel to a square.  The watch is
//  laid out in a 480x560 box of its own units and scaled to the screen; the
//  text is the Terminus face at whole multiples of its size.
//
#include <r2/math.hpp>
#include "ui/platform/impl/r2/R2_BitmapImpl.h"
#include "ui/platform/impl/r2/R2_TerminusFont.h"

class HelloWindow
{
public:
    enum Choice
    {
        None,     // the window was closed some other way
        Login,    // Enter, or a click
        Reboot,   // Esc
        PowerOff, // Shift+Esc
    };
    Choice choice = None;

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<HelloWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w) { wnd = w; }

private:
    PlatformWindow *wnd = nullptr;

    // The time the hands show: the RTC read once, then the tick counter,
    // as the taskbar clock keeps it (a read every ten minutes against drift).
    int32 baseSecond = -1;
    uint64 baseTicks = 0;
    int32 shownSecond = -1;

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        if (data->type == PlatformWindowInputEventType::OnCreate)
        {
            // Every pass of the loop, so that the seconds hand moves.
            wnd->SetImmediateMode(true);
        }
        else if (data->type == PlatformWindowInputEventType::OnImmediateModeIdleLoop)
        {
            if (secondOfDay() != shownSecond)
                wnd->Repaint();
        }
        else if (data->type == PlatformWindowInputEventType::OnPaint)
        {
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
        }
        else if (data->type == PlatformWindowInputEventType::OnKeyEvent)
        {
            auto *key = data->Data.OnKeyEvent.key;
            if (!key->isKeyDown)
                return;
            if (key->isEnter)
                leave(Login);
            else if (key->isEscape)
                leave(key->isLeftShift || key->isRightShift ? PowerOff : Reboot);
        }
        else if (data->type == PlatformWindowInputEventType::OnMouseClick)
        {
            if (data->Data.OnMouseClick.state == PlatformWindowButtonState::Pressed)
                leave(Login);
        }
    }

    void leave(Choice c)
    {
        choice = c;
        wnd->SetImmediateMode(false);
        wnd->Close();
    }

    int32 secondOfDay()
    {
        uint64 now = r2::ticks();
        if (baseSecond < 0 || now - baseTicks > 10ull * 60 * 1000)
        {
            r2::optional<r2::RtcTime> t = r2::clock_now();
            if (t)
                baseSecond = (int32)t->hours * 3600 + (int32)t->minutes * 60 + (int32)t->seconds;
            else if (baseSecond < 0)
                baseSecond = 10 * 3600 + 10 * 60; // no clock: the hands as watches are photographed
            baseTicks = now;
        }
        return (int32)((baseSecond + (now - baseTicks) / 1000) % 86400);
    }

    // ── Colours ─────────────────────────────────────────────────────────────

    enum : uint8
    {
        Black = 0,
        Blue = 1, // #0000AA, Memento's own
        LightBlue = 9,
        White = 15,
    };

    //  A palette index on every pixel (Solid), or two of them in a
    //  checkerboard (Half), or the second on one pixel in four (Quarter).
    enum : uint8
    {
        Solid,
        Half,
        Quarter
    };
    struct Ink
    {
        uint8 a, b, mode;

        uint8 at(int x, int y) const
        {
            if (mode == Half)
                return ((x ^ y) & 1) ? b : a;
            if (mode == Quarter)
                return ((x | y) & 1) ? a : b;
            return a;
        }
    };
    static Ink solid(uint8 i) { return Ink{i, i, Solid}; }
    static Ink half(uint8 a, uint8 b) { return Ink{a, b, Half}; }
    static Ink quarter(uint8 a, uint8 b) { return Ink{a, b, Quarter}; }

    //  Entry of the framebuffer's colour cube, each channel 0..5 standing for
    //  0, 51, 102, 153, 204, 255 (see R2_Palette).
    static uint8 cube(int r, int g, int b) { return (uint8)(16 + 36 * r + 6 * g + b); }

    struct Look
    {
        Ink ground, shadow, outline, shade, base, mid, pale;
        Ink bezelDark, bezel, bezelLit, dialEdge, dial, sheen;
        Ink ink, hand, handLit, glint, chain;
        uint8 title, titleShadow, text, keyEdge;
    };

    //  256 colours: a navy ground and the watch in eight steps of blue.
    static Look framebufferLook()
    {
        Look l;
        l.ground = solid(cube(0, 0, 1)); // #000033
        l.shadow = solid(Black);
        l.outline = solid(cube(0, 0, 1));
        l.shade = solid(cube(0, 0, 2)); // #000066
        l.base = solid(Blue);
        l.mid = solid(LightBlue);
        l.pale = solid(cube(3, 3, 5)); // #9999FF
        l.bezelDark = solid(cube(0, 0, 2));
        l.bezel = solid(cube(1, 1, 4));    // #3333CC
        l.bezelLit = solid(cube(2, 2, 5)); // #6666FF
        l.dialEdge = solid(cube(3, 3, 5));
        l.dial = solid(cube(4, 4, 5)); // #CCCCFF
        l.sheen = solid(White);
        l.ink = solid(Blue);
        l.hand = solid(cube(0, 0, 2));
        l.handLit = solid(cube(0, 0, 4)); // #0000CC
        l.glint = solid(White);
        l.chain = solid(LightBlue);
        l.title = cube(4, 4, 5);
        l.titleShadow = cube(0, 0, 2);
        l.text = White;
        l.keyEdge = cube(4, 4, 5);
        return l;
    }

    //  16 colours: Memento's blue for the ground, and the shades the cube
    //  would give dithered from black, the two blues and white.
    static Look vgaLook()
    {
        Look l;
        l.ground = solid(Blue);
        l.shadow = half(Black, Blue);
        l.outline = solid(Black);
        l.shade = half(Black, Blue);
        l.base = solid(Blue);
        l.mid = solid(LightBlue);
        l.pale = half(White, LightBlue);
        l.bezelDark = half(Black, Blue);
        l.bezel = half(Blue, LightBlue);
        l.bezelLit = solid(LightBlue);
        l.dialEdge = half(White, LightBlue);
        l.dial = quarter(White, LightBlue);
        l.sheen = solid(White);
        l.ink = solid(Blue);
        l.hand = solid(Black);
        l.handLit = solid(Blue);
        l.glint = solid(White);
        l.chain = solid(LightBlue);
        l.title = White;
        l.titleShadow = Black;
        l.text = White;
        l.keyEdge = White;
        return l;
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

    //  The bitmap, and where the watch's units land on it: unit (x, y) is
    //  pixel (ox + x*k, oy + y*k).  A pixel belongs to a shape when its
    //  centre is inside.
    struct Painter
    {
        uint8 *px;
        int w, h;
        double ox, oy, k;

        void span(int y, int x0, int x1, const Ink &c)
        {
            if (y < 0 || y >= h)
                return;
            if (x0 < 0)
                x0 = 0;
            if (x1 >= w)
                x1 = w - 1;
            uint8 *row = px + (size_t)y * w;
            for (int x = x0; x <= x1; x++)
                row[x] = c.at(x, y);
        }

        void put(int x, int y, const Ink &c)
        {
            if (x >= 0 && y >= 0 && x < w && y < h)
                px[(size_t)y * w + x] = c.at(x, y);
        }

        //  In pixels, not units.
        void box(int x, int y, int bw, int bh, uint8 colour)
        {
            for (int j = 0; j < bh; j++)
                span(y + j, x, x + bw - 1, solid(colour));
        }

        void disc(double cx, double cy, double r, const Ink &c)
        {
            double pcx = ox + cx * k, pcy = oy + cy * k, pr = r * k;
            for (int y = (int)floor(pcy - pr); y <= (int)ceil(pcy + pr); y++)
            {
                double dy = y + 0.5 - pcy;
                if (dy * dy > pr * pr)
                    continue;
                double dx = sqrt(pr * pr - dy * dy);
                span(y, (int)ceil(pcx - dx - 0.5), (int)floor(pcx + dx - 0.5), c);
            }
        }

        //  A polygon of up to 24 points, turned `deg` clockwise about
        //  (pivX, pivY) first.  Filled by the even-odd rule, so the stars'
        //  points need nothing special.
        void poly(const double *pts, int n, const Ink &c, double deg = 0, double pivX = 240, double pivY = 340)
        {
            if (n > 24)
                return;
            double xs[24], ys[24];
            double top = 1e9, bottom = -1e9;
            for (int i = 0; i < n; i++)
            {
                double x = pts[2 * i], y = pts[2 * i + 1];
                turn(x, y, deg, pivX, pivY);
                xs[i] = ox + x * k;
                ys[i] = oy + y * k;
                top = ys[i] < top ? ys[i] : top;
                bottom = ys[i] > bottom ? ys[i] : bottom;
            }
            for (int y = (int)floor(top); y <= (int)ceil(bottom); y++)
            {
                double yc = y + 0.5, cross[24];
                int nc = 0;
                for (int i = 0, j = n - 1; i < n; j = i++)
                    if ((ys[i] > yc) != (ys[j] > yc))
                    {
                        double x = xs[i] + (yc - ys[i]) * (xs[j] - xs[i]) / (ys[j] - ys[i]);
                        int at = nc++;
                        for (; at > 0 && cross[at - 1] > x; at--)
                            cross[at] = cross[at - 1];
                        cross[at] = x;
                    }
                for (int i = 0; i + 1 < nc; i += 2)
                    span(y, (int)ceil(cross[i] - 0.5), (int)floor(cross[i + 1] - 0.5), c);
            }
        }

        void rect(double x, double y, double rw, double rh, const Ink &c,
                  double deg = 0, double pivX = 240, double pivY = 340)
        {
            const double pts[8] = {x, y, x + rw, y, x + rw, y + rh, x, y + rh};
            poly(pts, 4, c, deg, pivX, pivY);
        }

        //  An ellipse turned `deg` about its centre; with inRx and inRy, only
        //  the ring outside that smaller one.  The chain's links.
        void ellipse(double cx, double cy, double rx, double ry, double deg, const Ink &c,
                     double inRx = 0, double inRy = 0)
        {
            double pcx = ox + cx * k, pcy = oy + cy * k;
            double prx = rx * k, pry = ry * k, irx = inRx * k, iry = inRy * k;
            double s = sin(-deg * r2::DEG_TO_RAD), co = cos(-deg * r2::DEG_TO_RAD);
            double reach = prx > pry ? prx : pry;
            for (int y = (int)floor(pcy - reach); y <= (int)ceil(pcy + reach); y++)
                for (int x = (int)floor(pcx - reach); x <= (int)ceil(pcx + reach); x++)
                {
                    double dx = x + 0.5 - pcx, dy = y + 0.5 - pcy;
                    double u = dx * co - dy * s, v = dx * s + dy * co;
                    if (u * u / (prx * prx) + v * v / (pry * pry) > 1)
                        continue;
                    if (irx > 0 && u * u / (irx * irx) + v * v / (iry * iry) < 1)
                        continue;
                    put(x, y, c);
                }
        }

        //  A mark `len` units long down from (x, y), turned `deg` about the
        //  pivot, two units wide.  Below a pixel and a half that would come
        //  and go with the angle, so it is drawn a pixel wide instead.
        void mark(double x, double y, double len, const Ink &c, double deg, double pivX, double pivY)
        {
            if (2 * k >= 1.5)
            {
                rect(x - 1, y, 2, len, c, deg, pivX, pivY);
                return;
            }
            double x0 = x, y0 = y, x1 = x, y1 = y + len;
            turn(x0, y0, deg, pivX, pivY);
            turn(x1, y1, deg, pivX, pivY);
            double ax = ox + x0 * k, ay = oy + y0 * k, bx = ox + x1 * k, by = oy + y1 * k;
            double run = fabs(bx - ax) > fabs(by - ay) ? fabs(bx - ax) : fabs(by - ay);
            int steps = (int)(run * 2) + 1;
            for (int i = 0; i <= steps; i++)
                put((int)floor(ax + (bx - ax) * i / steps), (int)floor(ay + (by - ay) * i / steps), c);
        }

        //  Terminus at `scale`, its first cell's top left at pixel (x, y),
        //  `extra` pixels more between the letters.
        void text(int x, int y, const char *s, int scale, uint8 colour, int extra = 0)
        {
            for (; *s; s++, x += (int)r2_font_char_width * scale + extra)
            {
                const uint8 *g = r2_terminus_glyphs + (uint8)*s * r2_font_char_height;
                for (int row = 0; row < (int)r2_font_char_height; row++)
                    for (int col = 0; col < (int)r2_font_char_width; col++)
                        if (g[row] & (0x80 >> col))
                            box(x + col * scale, y + row * scale, scale, scale, colour);
            }
        }

        //  Text centred on unit (x, y), at the face's own size.
        void label(double x, double y, const char *s, uint8 colour)
        {
            text((int)floor(ox + x * k - textWidth(s, 1) / 2.0 + 0.5),
                 (int)floor(oy + y * k - r2_font_char_height / 2.0 + 0.5), s, 1, colour);
        }
    };

    //  Clockwise by `deg` about (pivX, pivY), y pointing down as on screen.
    static void turn(double &x, double &y, double deg, double pivX, double pivY)
    {
        if (deg == 0)
            return;
        double s = sin(deg * r2::DEG_TO_RAD), co = cos(deg * r2::DEG_TO_RAD);
        double dx = x - pivX, dy = y - pivY;
        x = pivX + dx * co - dy * s;
        y = pivY + dx * s + dy * co;
    }

    //  Visible width: a cell is six pixels, the letter in it five.
    static int textWidth(const char *s, int scale, int extra = 0)
    {
        int n = (int)strlen(s);
        return n ? (n * (int)r2_font_char_width - 1) * scale + (n - 1) * extra : 0;
    }

    //  The watch, back to front.  Its face is centred at (240, 340), the case
    //  192 units round; the ring is on top and the chain hangs to the right.
    //  The light comes from the top left: each rounded part is a stack of
    //  discs, each a step lighter or darker and shifted a little, so a
    //  crescent of every step shows on its lit and its shaded side.
    static void drawWatch(Painter &p, const Look &l, double hourDeg, double minuteDeg, double secondDeg)
    {
        p.disc(252, 354, 192, l.shadow);

        // The ring (bow) at the top.
        p.disc(240, 92, 46, l.outline);
        p.disc(240, 92, 42, l.shade);
        p.disc(237, 89, 39, l.mid);
        p.disc(239, 91, 36, l.base);
        p.disc(240, 92, 26, l.outline);
        p.disc(240, 92, 22, l.ground);

        // The chain from it: links face on, then edge on, and the bar.
        static const double links[6][3] = {
            {294, 62, 20}, {324, 76, 32}, {352, 96, 44}, {375, 121, 56}, {392, 150, 66}, {403, 181, 76}};
        for (int i = 0; i < 6; i++)
        {
            double x = links[i][0], y = links[i][1], deg = links[i][2];
            if (i % 2 == 0)
            {
                p.ellipse(x, y, 19, 12, deg, l.outline, 11, 4);
                p.ellipse(x, y, 17, 10, deg, l.chain, 13, 6);
            }
            else
            {
                p.ellipse(x, y, 17, 5, deg, l.outline);
                p.ellipse(x, y, 15, 3, deg, l.chain);
            }
        }
        p.rect(386, 200, 44, 12, l.outline);
        p.rect(388, 202, 40, 8, l.chain);
        p.rect(388, 202, 40, 3, l.pale);

        // The neck, and the knurled crown on it.
        p.rect(220, 136, 40, 22, l.outline);
        p.rect(224, 138, 32, 18, l.base);
        p.rect(226, 138, 7, 18, l.mid);
        p.rect(250, 138, 6, 18, l.shade);
        p.rect(200, 108, 80, 36, l.outline);
        p.rect(204, 112, 72, 28, l.base);
        for (int x = 208; x <= 264; x += 8)
            p.rect(x, 112, 4, 28, l.mid);
        p.rect(204, 112, 72, 4, l.pale);
        p.rect(204, 134, 72, 6, l.shade);

        // The case.
        p.disc(240, 340, 192, l.outline);
        p.disc(240, 340, 186, l.shade);
        p.disc(234, 334, 180, l.pale);
        p.disc(237, 337, 177, l.mid);
        p.disc(243, 343, 171, l.base);

        // Light catching it.
        static const double glint[16] = {112, 194, 115, 209, 130, 212, 115, 215, 112, 230, 109, 215, 94, 212, 109, 209};
        static const double spark[16] = {146, 176, 148, 183, 155, 185, 148, 187, 146, 194, 144, 187, 137, 185, 144, 183};
        p.poly(glint, 8, l.glint);
        p.poly(spark, 8, l.glint);

        // The bezel round the glass.
        p.disc(240, 340, 158, l.outline);
        p.disc(240, 340, 154, l.bezelDark);
        p.disc(237, 337, 151, l.bezelLit);
        p.disc(240, 340, 148, l.bezel);

        // The dial, sunk below it: the shade on the top left this time.
        p.disc(240, 340, 140, l.outline);
        p.disc(240, 340, 137, l.dialEdge);
        p.disc(242, 342, 135, l.dial);

        // A sheen across the glass, upper left: an arc of a band, thinner at
        // its ends.
        double sheen[36];
        for (int i = 0; i < 9; i++)
        {
            double a = (290 + 50.0 * i / 8) * r2::DEG_TO_RAD; // 290..340 degrees, from 12 clockwise
            double b = (334 - 38.0 * i / 8) * r2::DEG_TO_RAD; // 334..296, back the other way
            sheen[2 * i] = 240 + 128 * sin(a);
            sheen[2 * i + 1] = 340 - 128 * cos(a);
            sheen[18 + 2 * i] = 240 + 112 * sin(b);
            sheen[18 + 2 * i + 1] = 340 - 112 * cos(b);
        }
        p.poly(sheen, 18, l.sheen);

        // The minute track and the hours.
        for (int i = 0; i < 60; i++)
            if (i % 5)
                p.mark(240, 212, 7, l.ink, i * 6, 240, 340);
            else
                p.rect(237, 211, 6, 13, l.hand, i * 6);
        static const char *const numerals[12] = {"12", "1", "2", "3", "4", "5", "", "7", "8", "9", "10", "11"};
        for (int i = 0; i < 12; i++)
            if (numerals[i][0])
            {
                double a = i * 30 * r2::DEG_TO_RAD;
                p.label(240 + 100 * sin(a), 340 - 100 * cos(a), numerals[i], l.ink.a);
            }
        // The maker's name, where it clears the 10 and the 2: not on the
        // VGA's 400 lines, where the face's smallest size is too wide for it.
        if (textWidth("MEMENTO", 1) <= 90 * p.k)
            p.label(240, 290, "MEMENTO", l.ink.a);

        // The small seconds, where the 6 would be.
        p.disc(240, 412, 31, l.dialEdge);
        p.disc(241, 413, 28, l.dial);
        for (int i = 0; i < 12; i++)
            p.mark(240, 384, i % 3 ? 3 : 6, l.ink, i * 30, 240, 412);
        p.mark(240, 390, 28, l.hand, secondDeg, 240, 412);
        p.disc(240, 412, 3, l.hand);

        // The hours hand, with a moon at its tip, and the minutes hand: the
        // half towards the light a step lighter.
        static const double hourHand[8] = {240, 268, 247, 330, 240, 354, 233, 330};
        static const double hourLit[6] = {240, 268, 240, 354, 233, 330};
        p.poly(hourHand, 4, l.hand, hourDeg);
        p.poly(hourLit, 3, l.handLit, hourDeg);
        double mx = 240, my = 284;
        turn(mx, my, hourDeg, 240, 340);
        p.disc(mx, my, 9, l.hand);
        p.disc(mx, my, 5, l.dial);
        static const double minuteHand[8] = {240, 222, 245, 330, 240, 356, 235, 330};
        static const double minuteLit[6] = {240, 222, 240, 356, 235, 330};
        p.poly(minuteHand, 4, l.hand, minuteDeg);
        p.poly(minuteLit, 3, l.handLit, minuteDeg);

        p.disc(240, 340, 10, l.outline);
        p.disc(240, 340, 6, l.mid);
        p.disc(239, 339, 3, l.pale);
    }

    //  The hints along the bottom: keys in a keycap, the words after them.
    //  A null text starts the next group.  Draws when given a painter, and
    //  answers the width either way.
    static int hints(Painter *p, const Look &l, int scale, int x, int y)
    {
        struct Item
        {
            const char *text;
            bool key;
        };
        static const Item items[] = {
            {"Enter", true}, {"to login", false}, {nullptr, false},
            {"Esc", true}, {"to reboot", false}, {nullptr, false},
            {"Shift", true}, {"+", false}, {"Esc", true}, {"to power off", false}};
        const int padX = 3 * scale + 1, near = 2 * scale + 4, far = 12 * scale + 12;
        const int keyH = 2 + 15 * scale, textY = y + 1 + scale;
        int at = x;
        bool first = true;
        for (const Item &it : items)
        {
            if (!it.text)
            {
                at += far - near;
                continue;
            }
            if (!first)
                at += near;
            first = false;
            int tw = textWidth(it.text, scale);
            if (!it.key)
            {
                if (p)
                    p->text(at, textY, it.text, scale, l.text);
                at += tw;
                continue;
            }
            int kw = tw + 2 * padX + 2;
            if (p)
            {
                p->box(at, y, kw, 1, l.keyEdge);
                p->box(at, y, 1, keyH, l.keyEdge);
                p->box(at + kw - 1, y, 1, keyH, l.keyEdge);
                p->box(at, y + keyH - 1 - scale, kw, 1 + scale, l.keyEdge);
                p->text(at + 1 + padX, textY, it.text, scale, l.text);
            }
            at += kw;
        }
        return at - x;
    }

    void OnPaint(PlatformDrawingContext *, PlatformBitmap *target)
    {
        auto *bm = static_cast<MementoR2Impl::R2_BitmapImpl *>(target);
        uint8 *px = bm->GetPixels();
        int W = bm->GetRealWidth().intValue(), H = bm->GetRealHeight().intValue();
        if (!px || W <= 0 || H <= 0)
            return;
        const Look l = MementoR2Impl::R2_Palette::Count() > 16 ? framebufferLook() : vgaLook();
        Painter p{px, W, H, 0, 0, 1};
        p.box(0, 0, W, H, l.ground.a);

        // Sized from the screen's height: the watch a little over half of
        // it, the name Terminus at a hundredth of it, the hints at twice the
        // face's size from 500 lines up, if they fit across.
        int ts = H / 100 < 2 ? 2 : H / 100;
        int fs = H >= 500 && hints(nullptr, l, 2, 0, 0) <= W - 32 ? 2 : 1;
        int watchH = H * 21 / 40;
        double k = watchH / 560.0;
        int watchW = (int)(480 * k + 0.5);
        int gap = H / 50;
        int titleH = (int)r2_font_char_height * ts;
        int keyH = 2 + 15 * fs;
        int footerTop = H - H * 11 / 200 - keyH;
        int padTop = H / 27;
        int top = padTop + (footerTop - padTop - (watchH + gap + titleH)) / 2;

        int32 s = secondOfDay();
        shownSecond = s;
        int hh = s / 3600 % 12, mm = s / 60 % 60, ss = s % 60;
        p.ox = (W - watchW) / 2;
        p.oy = top;
        p.k = k;
        drawWatch(p, l, hh * 30 + mm * 0.5, mm * 6 + ss * 0.1, ss * 6);

        const char *name = "Memento";
        int extra = 2 * ts, drop = (ts + 1) / 2;
        int nx = (W - textWidth(name, ts, extra)) / 2, ny = top + watchH + gap;
        p.text(nx + drop, ny + drop, name, ts, l.titleShadow, extra);
        p.text(nx, ny, name, ts, l.title, extra);

        hints(&p, l, fs, (W - hints(nullptr, l, fs, 0, 0)) / 2, footerTop);
    }
};
