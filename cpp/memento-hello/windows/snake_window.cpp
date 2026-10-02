//
// Window — Snake
//
// The game from libc++r2's examples/snake, in a window.  The rules are that
// example's own (examples/snake/game.hpp, shared by both); this file is only
// the drawing, the keys and the clock, which there go to the framebuffer and
// the keyboard and here go through Memento.  The high score is the same file
// too, /mnt/fat/SNAKE.HSC, so the two keep one record between them.
//
//      arrows or WASD   turn (the first one starts the game)
//      P or Space       pause
//      R or Enter       play again once it is over
//      Esc              close
//
// The window has no way to know it has lost the focus, so a game left running
// behind another window runs on: pause it first.
//

#include "../../libc++r2/examples/snake/game.hpp"

class SnakeWindow
{
public:
    //  Cells are 6 units square: 12 pixels on the 640x400 screen.
    static const int CS = 6;
    static const int HUD = 12;
    static const int W = snake::GRID_W * CS + 4;
    static const int H = HUD + snake::GRID_H * CS + 4;

    SnakeWindow()
    {
        game = snake::Game::create(r2::ticks() ^ 0xA5A5A5A5A5A5A5A5ull);
        high = loadHigh();
    }

    static void onEvent(void *instance, struct PlatformWindowInterfaceInputEvent *data)
    {
        reinterpret_cast<SnakeWindow *>(instance)->onEvent_(data);
    }

    void SetWindow(PlatformWindow *w)
    {
        wnd = w;
        //  The idle loop is the game's clock.
        wnd->SetImmediateMode(true);
    }

private:
    static constexpr const char *HIGH_PATH = "/mnt/fat/SNAKE.HSC";

    PlatformWindow *wnd = nullptr;
    r2::optional<snake::Game> game;
    bool started = false; // waiting for the first turn
    uint32_t high = 0;
    uint64_t nextStep = 0;
    uint64_t lastPulse = 0;
    bool pulse = false; // the food's size, which alternates

    PlatformColor *col[16] = {};
    PlatformFont *font = nullptr;

    // ── Rules around the rules ──────────────────────────────────────────────

    //  The file is a number and a newline, in one zero-padded sector.
    static uint32_t loadHigh()
    {
        auto text = r2::fs::read_text(HIGH_PATH, 512);
        if (!text)
            return 0;
        uint32_t v = 0;
        for (const char *p = text->c_str(); *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (uint32_t)(*p - '0');
        return v;
    }

    static void saveHigh(uint32_t v)
    {
        char buf[16];
        int n = 0;
        char tmp[12];
        int k = 0;
        do
        {
            tmp[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (k)
            buf[n++] = tmp[--k];
        buf[n++] = '\n';
        buf[n] = 0;
        (void)r2::fs::write_text(HIGH_PATH, r2::string_view(buf, (size_t)n));
    }

    void turn(snake::Direction d)
    {
        if (!game || game->state() == snake::Game::State::Over)
            return;
        if (!started)
        {
            started = true;
            nextStep = r2::ticks() + game->step_ms();
        }
        game->turn(d);
    }

    void restart()
    {
        game->restart();
        started = false;
        wnd->Repaint();
    }

    void onKey(PlatformKey *key)
    {
        if (!key->isKeyDown || !game)
            return;
        char c = key->isChar ? (char)key->theChar : 0;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (key->isEscape)
        {
            wnd->Close();
            return;
        }
        if (key->isArrowUp || c == 'w')
            turn(snake::Direction::Up);
        else if (key->isArrowDown || c == 's')
            turn(snake::Direction::Down);
        else if (key->isArrowLeft || c == 'a')
            turn(snake::Direction::Left);
        else if (key->isArrowRight || c == 'd')
            turn(snake::Direction::Right);
        else if ((c == 'p' || c == ' ') && started)
        {
            game->toggle_pause();
            nextStep = r2::ticks() + game->step_ms();
            wnd->Repaint();
        }
        else if ((c == 'r' || key->isEnter) && game->state() == snake::Game::State::Over)
            restart();
    }

    void onIdle()
    {
        if (!game)
            return;
        uint64_t now = r2::ticks();
        bool dirty = false;

        if (started && game->state() == snake::Game::State::Running && now >= nextStep)
        {
            bool ate = game->step();
            nextStep = now + game->step_ms();
            dirty = true;
            //  r2::audio::beep blocks for as long as it sounds: a click, not a
            //  note, or the next step would come late.
            if (ate)
                r2::audio::beep((uint16_t)(r2::audio::NOTE_C5 + game->level() * 20), 12);
            if (game->state() == snake::Game::State::Over)
            {
                r2::audio::beep(r2::audio::NOTE_C4, 60);
                if (game->score() > high)
                {
                    high = game->score();
                    saveHigh(high);
                }
            }
        }
        //  The food pulses: the game is alive while the player thinks.
        if (now - lastPulse >= 160)
        {
            lastPulse = now;
            pulse = !pulse;
            dirty = true;
        }
        if (dirty)
            wnd->Repaint();
        else
            r2::sleep(5); // nothing due: give the time back
    }

    void onEvent_(struct PlatformWindowInterfaceInputEvent *data)
    {
        switch (data->type)
        {
        case PlatformWindowInputEventType::OnImmediateModeIdleLoop:
            onIdle();
            return;
        case PlatformWindowInputEventType::OnPaint:
            OnPaint(data->Data.OnPaint.ctx, data->Data.OnPaint.target);
            return;
        case PlatformWindowInputEventType::OnKeyEvent:
            onKey(data->Data.OnKeyEvent.key);
            return;
        default:
            return;
        }
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

    enum
    {
        BLACK = 0,
        BLUE = 1,
        GREEN = 2,
        CYAN = 3,
        DARKGREY = 8,
        LIGHTGREY = 7,
        LIGHTGREEN = 10,
        LIGHTRED = 12,
        YELLOW = 14,
        WHITE = 15
    };

    void makeResources(PlatformDrawingContext *dc)
    {
        if (font)
            return;
        static const uint32_t rgb[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                         0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                         0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
        for (int i = 0; i < 16; i++)
            col[i] = dc->CreateColor(0xFF000000u | rgb[i], nullptr, nullptr);
        font = dc->CreateFont(6, nullptr, false, false, false, nullptr, nullptr);
    }

    static void append(char *out, const char *s)
    {
        size_t at = strlen(out);
        while (*s)
            out[at++] = *s++;
        out[at] = 0;
    }

    static void appendU(char *out, uint32_t v)
    {
        char tmp[12];
        int k = 0;
        do
        {
            tmp[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        size_t at = strlen(out);
        while (k)
            out[at++] = tmp[--k];
        out[at] = 0;
    }

    void text(PlatformBitmap *t, int x, int y, int w, const char *s, int color, PlatformAlign align)
    {
        PlatformDrawTextOptions o{};
        o.font = font;
        o.foreground = col[color];
        o.horizontalAlign = align;
        o.verticalAlign = PlatformAlign::Middle;
        t->DrawText(x, y, w, 10, (const mchar *)s, &o, false);
    }

    //  A box across the top third of the board with three lines in it: high
    //  enough to leave the middle row, where the snake starts, in view.
    void panel(PlatformBitmap *t, const char *a, const char *b, const char *c)
    {
        const int pw = 150, ph = 40;
        const int px = (W - pw) / 2, py = HUD + 2 + 2 * CS;
        t->FillRect(px, py, pw, ph, col[WHITE], false);
        t->FillRect(px + 1, py + 1, pw - 2, ph - 2, col[BLUE], false);
        text(t, px, py + 3, pw, a, WHITE, PlatformAlign::Middle);
        text(t, px, py + 15, pw, b, WHITE, PlatformAlign::Middle);
        text(t, px, py + 27, pw, c, LIGHTGREY, PlatformAlign::Middle);
    }

    void OnPaint(PlatformDrawingContext *dc, PlatformBitmap *t)
    {
        if (!t)
            return;
        makeResources(dc);
        if (!font || !col[WHITE])
            return;
        t->FillRect(0, 0, W, H, col[BLACK], false);
        if (!game)
        {
            text(t, 4, 4, W - 8, "No memory for the board.", WHITE, PlatformAlign::Begin);
            return;
        }

        // The score line.
        char line[64] = "SCORE ";
        appendU(line, game->score());
        append(line, "  LEN ");
        appendU(line, game->length());
        append(line, "  LVL ");
        appendU(line, game->level());
        text(t, 3, 1, W - 6, line, WHITE, PlatformAlign::Begin);
        char hi[24] = "HI ";
        appendU(hi, high);
        text(t, 3, 1, W - 6, hi, LIGHTGREY, PlatformAlign::End);

        // The board: a wall around it, a dot at every cell corner.
        const int bx = 2, by = HUD + 2;
        const int bw = snake::GRID_W * CS, bh = snake::GRID_H * CS;
        t->FillRect(bx - 1, by - 1, bw + 2, bh + 2, col[LIGHTGREY], false);
        t->FillRect(bx, by, bw, bh, col[BLACK], false);
        for (int gy = 1; gy < snake::GRID_H; gy++)
            for (int gx = 1; gx < snake::GRID_W; gx++)
                t->FillRect(bx + gx * CS, by + gy * CS, 1, 1, col[DARKGREY], false);

        // The food: a square that breathes.
        snake::Point f = game->food();
        int inset = pulse ? 1 : 2;
        t->FillRect(bx + f.x * CS + inset, by + f.y * CS + inset, CS - 2 * inset, CS - 2 * inset, col[LIGHTRED],
                    false);

        // The snake, tail first so the head is on top; the back third of the
        // body in the tail's colour.
        uint32_t len = game->length();
        for (uint32_t i = len; i-- > 0;)
        {
            snake::Point p = game->segment(i);
            int x = bx + p.x * CS, y = by + p.y * CS;
            if (i > 0)
            {
                t->FillRect(x + 1, y + 1, CS - 2, CS - 2, col[i * 3 >= len * 2 ? CYAN : GREEN], false);
                continue;
            }
            t->FillRect(x, y, CS, CS, col[LIGHTGREEN], false);
            // Two eyes, forward along the way it is going and to either side.
            snake::Point d = snake::step_of(game->direction());
            int ex = x + CS / 2 + d.x * 1, ey = y + CS / 2 + d.y * 1;
            int ox = -d.y, oy = d.x;
            t->FillRect(ex + ox * 2 - 1 + (ox < 0), ey + oy * 2 - 1 + (oy < 0), 1, 1, col[BLACK], false);
            t->FillRect(ex - ox * 2 - 1 + (ox > 0), ey - oy * 2 - 1 + (oy > 0), 1, 1, col[BLACK], false);
        }

        if (!started && game->state() != snake::Game::State::Over)
            panel(t, "SNAKE", "an arrow key starts", "P pauses, Esc closes");
        else if (game->state() == snake::Game::State::Paused)
            panel(t, "PAUSED", "P or Space goes on", "Esc closes");
        else if (game->state() == snake::Game::State::Over)
        {
            char s[48] = "score ";
            appendU(s, game->score());
            append(s, "   best ");
            appendU(s, high);
            panel(t, game->won() ? "BOARD FULL - YOU WIN" : "GAME OVER", s, "R plays again, Esc closes");
        }
    }
};
