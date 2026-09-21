/*
 *  snake — the game, on r2.
 *
 *  gfxdemo shows the drawing calls; this one shows what a program built out
 *  of them looks like.  A real main loop: input drained at the frame rate but
 *  consumed at the (slower, and changing) rate the snake moves, a board
 *  allocated once up front because the arena is 512 KiB and a 320x200 canvas
 *  already takes half of it, and a high score that outlives the process on the
 *  floppy.
 *
 *  Both display paths, as in gfxdemo: the VESA framebuffer when the kernel has
 *  one, VGA mode 13h otherwise.  Mode 13h gets the frame through
 *  present_reduced(), which is two colours by brightness --- so the food is a
 *  disc and the snake is squares, and the two are still told apart when every
 *  bright pixel comes out the same white.
 *
 *      arrows or WASD   turn
 *      P or space       pause
 *      R                restart
 *      ESC or Q         quit
 *
 *  Build and run:
 *
 *      make build       # produces snake.elf
 *      make install     # copies SNAKE.ELF onto fat.img
 *
 *  and `fg SNAKE` from the r2 shell.
 */

#include <r2.hpp>

using namespace r2;
using namespace r2::gfx;

namespace {

/* ------------------------------------------------------------------------ *
 *  The board
 * ------------------------------------------------------------------------ */

/*  A cell is 8 pixels, which puts a 40x22 board and a 24-pixel status bar in
 *  exactly the 320x200 that mode 13h has and that a framebuffer scales up.  */
constexpr int32_t CELL = 8;
constexpr int32_t GRID_W = 40;
constexpr int32_t GRID_H = 22;
constexpr int32_t HUD_H = 24;

constexpr uint32_t CANVAS_WIDTH = (uint32_t)(GRID_W * CELL);
constexpr uint32_t CANVAS_HEIGHT = (uint32_t)(HUD_H + GRID_H * CELL);

constexpr size_t CELL_COUNT = (size_t)GRID_W * (size_t)GRID_H;

/*  Frames are paced at 50 a second so a turn is picked up promptly; the snake
 *  itself moves once every step_ms(), which is a good deal slower.  */
constexpr uint64_t FRAME_MS = 20;
constexpr uint64_t START_STEP_MS = 140;
constexpr uint64_t FASTEST_STEP_MS = 60;
constexpr uint32_t FOOD_PER_LEVEL = 5;
constexpr uint32_t START_LENGTH = 4;

constexpr string_view HIGH_SCORE_PATH = string_view("SNAKE.HSC");

/*  Every colour the game draws with, picked so the mode-13h reduction still
 *  leaves a playable picture: the background and the grid fall below the
 *  brightness threshold, everything the player has to see sits above it.  */
namespace palette {
inline constexpr Color Background{12, 16, 40};
inline constexpr Color Grid{26, 32, 66};
inline constexpr Color Wall{96, 108, 150};
inline constexpr Color Head{170, 255, 150};
inline constexpr Color Body{56, 200, 110};
inline constexpr Color Tail{70, 160, 200};
inline constexpr Color Food{245, 95, 85};
inline constexpr Color Panel{20, 24, 60};
inline constexpr Color Text{235, 235, 245};
inline constexpr Color Dim{140, 150, 175};
} // namespace palette

/*
 *  Set-1 make codes the library does not name.  The arrow keys arrive as 0xE0
 *  and then the code below; the prefix has its top bit set, so the release
 *  test below drops it and the keypad ends up sending the same turns.
 */
constexpr uint8_t SC_Q = 0x10;
constexpr uint8_t SC_W = 0x11;
constexpr uint8_t SC_R = 0x13;
constexpr uint8_t SC_P = 0x19;
constexpr uint8_t SC_A = 0x1E;
constexpr uint8_t SC_S = 0x1F;
constexpr uint8_t SC_D = 0x20;

struct Point {
    int16_t x, y;

    constexpr bool operator==(Point other) const { return x == other.x && y == other.y; }
};

enum class Direction : uint8_t { Up, Down, Left, Right };

constexpr Point step_of(Direction d) {
    switch (d) {
    case Direction::Up:
        return Point{0, -1};
    case Direction::Down:
        return Point{0, 1};
    case Direction::Left:
        return Point{-1, 0};
    default:
        return Point{1, 0};
    }
}

constexpr bool opposite(Direction a, Direction b) {
    return (a == Direction::Up && b == Direction::Down) ||
           (a == Direction::Down && b == Direction::Up) ||
           (a == Direction::Left && b == Direction::Right) ||
           (a == Direction::Right && b == Direction::Left);
}

/*
 *  xorshift64: the library has no rand(), and the food has to land somewhere.
 *  Seeded from the clock, so two runs differ.
 */
class Random {
  public:
    explicit Random(uint64_t seed) noexcept : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    uint64_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    uint32_t below(uint32_t bound) noexcept { return bound ? (uint32_t)(next() % bound) : 0; }

  private:
    uint64_t state_;
};

/* ------------------------------------------------------------------------ *
 *  Game — the rules, with no drawing in them
 * ------------------------------------------------------------------------ */

/*
 *  The snake is a ring buffer over one allocation the size of the board: a
 *  step writes the new head and leaves the tail behind by arithmetic, so it
 *  costs the same whether the snake is four cells long or four hundred.
 *  `cells_` is the same board as a byte per cell, which turns "did I bite
 *  myself" and "where may the food go" into array lookups rather than a walk
 *  of the body.
 */
class Game {
  public:
    enum class State : uint8_t { Running, Paused, Over };

    /*  Allocates both boards; nullopt when the arena has no room left.  */
    static optional<Game> create(uint64_t seed) {
        Game game(seed);
        if (!game.cells_.resize(CELL_COUNT, 0))
            return nullopt;
        if (!game.body_.resize(CELL_COUNT + 1))
            return nullopt;

        game.restart();
        return game;
    }

    void restart() {
        for (uint8_t &cell : cells_)
            cell = 0;

        direction_ = Direction::Right;
        pending_len_ = 0;
        length_ = START_LENGTH;
        head_ = 0;
        score_ = 0;
        eaten_ = 0;
        state_ = State::Running;
        won_ = false;

        /*  Laid out to the left of the middle, facing right, so the first
         *  seconds are not a scramble away from a wall.  */
        const int16_t y = (int16_t)(GRID_H / 2);
        for (uint32_t i = 0; i < length_; i++) {
            Point cell{(int16_t)(GRID_W / 4 + (int32_t)(length_ - 1 - i)), y};
            body_[slot(i)] = cell;
            occupy(cell, true);
        }

        place_food();
    }

    /*
     *  Queues a turn.  Two of them may be outstanding, so a quick corner ---
     *  up then right, both inside one step --- is not swallowed the way it
     *  would be if the direction were simply overwritten.  A reversal onto the
     *  snake's own neck is refused rather than queued.
     */
    void turn(Direction d) {
        Direction last = pending_len_ ? pending_[pending_len_ - 1] : direction_;
        if (d == last || opposite(d, last))
            return;
        if (pending_len_ < 2)
            pending_[pending_len_++] = d;
    }

    void toggle_pause() {
        if (state_ == State::Running)
            state_ = State::Paused;
        else if (state_ == State::Paused)
            state_ = State::Running;
    }

    /*  One move.  Returns true when this step ate something.  */
    bool step() {
        if (state_ != State::Running)
            return false;

        if (pending_len_) {
            direction_ = pending_[0];
            pending_[0] = pending_[1];
            pending_len_--;
        }

        const Point delta = step_of(direction_);
        const Point head = segment(0);
        const Point next{(int16_t)(head.x + delta.x), (int16_t)(head.y + delta.y)};

        if (next.x < 0 || next.y < 0 || next.x >= GRID_W || next.y >= GRID_H) {
            state_ = State::Over;
            return false;
        }

        const bool eats = next == food_;

        /*  The tail cell is free by the time the head lands on it --- unless
         *  this step also grows the snake, and the tail stays where it is.  */
        const Point tail = segment(length_ - 1);
        if (occupied(next) && !(next == tail && !eats)) {
            state_ = State::Over;
            return false;
        }

        if (!eats)
            occupy(tail, false);
        else
            length_++;

        head_ = (head_ + 1) % body_.size();
        body_[head_] = next;
        occupy(next, true);

        if (!eats)
            return false;

        eaten_++;
        score_ += 10 * level();

        /*  The board is full: there is nowhere left to put food, and that is
         *  as won as this game gets.  */
        if ((size_t)length_ >= CELL_COUNT) {
            won_ = true;
            state_ = State::Over;
            return true;
        }

        place_food();
        return true;
    }

    State state() const noexcept { return state_; }
    bool won() const noexcept { return won_; }
    uint32_t score() const noexcept { return score_; }
    uint32_t length() const noexcept { return length_; }
    uint32_t level() const noexcept { return 1 + eaten_ / FOOD_PER_LEVEL; }
    Direction direction() const noexcept { return direction_; }
    Point food() const noexcept { return food_; }

    /*  Milliseconds between moves: every level takes 10 ms off, down to a
     *  floor that is still playable.  */
    uint64_t step_ms() const noexcept {
        const uint64_t taken = (uint64_t)(level() - 1) * 10;
        return taken >= START_STEP_MS - FASTEST_STEP_MS ? FASTEST_STEP_MS : START_STEP_MS - taken;
    }

    /*  Segment 0 is the head, length() - 1 the tail.  */
    Point segment(uint32_t index) const { return body_[slot(index)]; }

  private:
    explicit Game(uint64_t seed) : rng_(seed) {}

    size_t slot(uint32_t index) const {
        return (head_ + body_.size() - (size_t)index) % body_.size();
    }

    bool occupied(Point p) const { return cells_[(size_t)p.y * GRID_W + (size_t)p.x] != 0; }
    void occupy(Point p, bool taken) { cells_[(size_t)p.y * GRID_W + (size_t)p.x] = taken; }

    /*  Picks uniformly among the free cells: draw one of them by number, then
     *  walk the board until that many have gone by.  No retry loop, so a board
     *  that is nearly full does not turn into an unbounded search.  */
    void place_food() {
        const uint32_t free_cells = (uint32_t)CELL_COUNT - length_;
        if (free_cells == 0)
            return;

        uint32_t wanted = rng_.below(free_cells);
        for (size_t i = 0; i < CELL_COUNT; i++) {
            if (cells_[i])
                continue;
            if (wanted-- == 0) {
                food_ = Point{(int16_t)(i % GRID_W), (int16_t)(i / GRID_W)};
                return;
            }
        }
    }

    vector<Point> body_;
    vector<uint8_t> cells_;
    Random rng_;
    Point food_{0, 0};
    size_t head_ = 0;
    uint32_t length_ = 0;
    uint32_t score_ = 0;
    uint32_t eaten_ = 0;
    Direction direction_ = Direction::Right;
    Direction pending_[2] = {Direction::Right, Direction::Right};
    uint8_t pending_len_ = 0;
    State state_ = State::Running;
    bool won_ = false;
};

/* ------------------------------------------------------------------------ *
 *  The high score, on the floppy
 * ------------------------------------------------------------------------ */

/*  fs::write stores exactly one 512-byte sector, zero-padded, so the number
 *  comes back with a tail of NULs behind it --- take the first line.  */
uint32_t load_high_score() {
    auto text = fs::read_text(HIGH_SCORE_PATH, 512);
    if (!text)
        return 0;

    int64_t value = 0;
    if (!parse_int(text->view().split('\n').first, value) || value < 0)
        return 0;

    return (uint32_t)value;
}

bool save_high_score(uint32_t score) {
    return fs::write_text(HIGH_SCORE_PATH, format("{}\n", score).view());
}

/* ------------------------------------------------------------------------ *
 *  Drawing
 * ------------------------------------------------------------------------ */

int32_t cell_x(int32_t grid_x) { return grid_x * CELL; }
int32_t cell_y(int32_t grid_y) { return HUD_H + grid_y * CELL; }

void draw_centered(Canvas &canvas, const Font &font, int32_t y, string_view text, Color color) {
    const int32_t x = ((int32_t)canvas.width() - (int32_t)font.text_width(text)) / 2;
    canvas.draw_text(font, x, y, text, color);
}

void draw_hud(Canvas &canvas, const Font &font, const Game &game, uint32_t high_score) {
    const int32_t y = (HUD_H - (int32_t)font.char_height()) / 2;

    string left = format("SCORE {}  LEN {}  LVL {}", game.score(), game.length(), game.level());
    canvas.draw_text(font, 4, y, left.view(), palette::Text);

    string right = format("HI {}", high_score);
    const int32_t x = (int32_t)canvas.width() - (int32_t)font.text_width(right.view()) - 4;
    canvas.draw_text(font, x, y, right.view(), palette::Dim);
}

void draw_board(Canvas &canvas, const Game &game, uint64_t frame) {
    /*  A dot in the corner of every cell: enough to read distances by, dark
     *  enough to vanish in the mode-13h reduction.  */
    for (int32_t gy = 0; gy < GRID_H; gy++)
        for (int32_t gx = 0; gx < GRID_W; gx++)
            canvas.set_pixel(cell_x(gx), cell_y(gy), palette::Grid);

    canvas.draw_rect(0, HUD_H, (int32_t)CANVAS_WIDTH, GRID_H * CELL, palette::Wall);

    /*  The food pulses, which is the cheapest way to tell the player that the
     *  program is still running while they think about the next turn.  */
    const Point food = game.food();
    canvas.fill_circle(cell_x(food.x) + CELL / 2, cell_y(food.y) + CELL / 2,
                       3 + (int32_t)((frame / 8) % 2), palette::Food);

    /*  Tail first, so the head lands on top of the segment behind it.  */
    for (uint32_t i = game.length(); i-- > 0;) {
        const Point p = game.segment(i);
        const int32_t x = cell_x(p.x);
        const int32_t y = cell_y(p.y);

        if (i > 0) {
            const uint8_t taper =
                game.length() > 1 ? (uint8_t)((uint32_t)i * 255 / (game.length() - 1)) : 0;
            canvas.fill_rect(x + 1, y + 1, CELL - 2, CELL - 2,
                             palette::Body.blend(palette::Tail, taper));
            continue;
        }

        canvas.fill_rect(x, y, CELL, CELL, palette::Head);

        /*  Two eyes, set forward along the direction of travel and to either
         *  side of it.  */
        const Point delta = step_of(game.direction());
        const int32_t ex = x + CELL / 2 + delta.x * 2;
        const int32_t ey = y + CELL / 2 + delta.y * 2;
        const int32_t ox = -delta.y;
        const int32_t oy = delta.x;
        canvas.fill_rect(ex + ox * 2 - 1, ey + oy * 2 - 1, 2, 2, palette::Background);
        canvas.fill_rect(ex - ox * 2 - 1, ey - oy * 2 - 1, 2, 2, palette::Background);
    }
}

void draw_panel(Canvas &canvas, const Font &font, string_view title, string_view line_a,
                string_view line_b) {
    const int32_t line_h = (int32_t)font.char_height() + 4;
    const int32_t width = 248;
    const int32_t height = line_h * 3 + 16;
    const int32_t x = ((int32_t)canvas.width() - width) / 2;
    const int32_t y = ((int32_t)canvas.height() - height) / 2;

    canvas.fill_rect(x, y, width, height, palette::Panel);
    canvas.draw_rect(x, y, width, height, palette::Wall);

    draw_centered(canvas, font, y + 8, title, palette::Text);
    draw_centered(canvas, font, y + 8 + line_h, line_a, palette::Text);
    draw_centered(canvas, font, y + 8 + line_h * 2, line_b, palette::Dim);
}

void draw_frame(Canvas &canvas, const Font *font, const Game &game, uint32_t high_score,
                uint64_t frame) {
    canvas.clear(palette::Background);
    draw_board(canvas, game, frame);

    if (!font)
        return;

    draw_hud(canvas, *font, game, high_score);

    if (game.state() == Game::State::Paused)
        draw_panel(canvas, *font, string_view("PAUSED"), string_view("P or SPACE to go on"),
                   string_view("ESC quits"));
    else if (game.state() == Game::State::Over)
        draw_panel(canvas, *font,
                   game.won() ? string_view("BOARD FULL -- YOU WIN") : string_view("GAME OVER"),
                   format("score {}   best {}", game.score(), high_score).view(),
                   string_view("R plays again, ESC quits"));
}

/* ------------------------------------------------------------------------ *
 *  Sound
 * ------------------------------------------------------------------------ */

/*  audio::beep blocks for its whole duration, which is why these are as short
 *  as they are: a longer note would be a visible stutter in the frame after
 *  the snake eats.  The game-over flourish can afford to be slower.  */
void beep_eat(uint32_t level) {
    audio::beep((uint16_t)(audio::NOTE_C5 + level * 20), 12);
}

void beep_over() {
    audio::beep(audio::NOTE_G4, 90);
    audio::beep(audio::NOTE_C4, 140);
}

} // namespace

int main() {
    auto canvas = Canvas::create(CANVAS_WIDTH, CANVAS_HEIGHT);
    if (!canvas) {
        println("snake: no room in the arena for a 320x200 canvas");
        return 1;
    }

    auto game = Game::create(ticks() ^ 0xA5A5A5A5A5A5A5A5ull);
    if (!game) {
        println("snake: no room in the arena for the board");
        return 1;
    }

    auto font = Font::kernel();

    /*  Prefer the framebuffer; fall back to VGA mode 13h.  */
    auto display = Display::open();
    optional<Vga13> vga;

    if (display) {
        printf("snake: framebuffer {}x{} at {} bpp\n", display->width(), display->height(),
               display->bpp());
    } else {
        vga = Vga13::open();
        if (!vga) {
            println("snake: no framebuffer and no VGA; nothing to draw on");
            return 1;
        }
    }

    uint32_t high_score = load_high_score();

    input::Keyboard keyboard;
    keyboard.drain();

    FrameTimer frame_timer(FRAME_MS);
    uint64_t next_step = ticks() + game->step_ms();
    uint64_t frame = 0;
    bool running = true;

    while (running) {
        for (uint8_t scancode = keyboard.next_scancode(); scancode != 0;
             scancode = keyboard.next_scancode()) {
            /*  Releases, and the 0xE0 that prefixes an arrow key, both have
             *  the top bit set and are of no interest here.  */
            if (input::is_release(scancode))
                continue;

            switch (input::make_code(scancode)) {
            case input::SC_UP:
            case SC_W:
                game->turn(Direction::Up);
                break;
            case input::SC_DOWN:
            case SC_S:
                game->turn(Direction::Down);
                break;
            case input::SC_LEFT:
            case SC_A:
                game->turn(Direction::Left);
                break;
            case input::SC_RIGHT:
            case SC_D:
                game->turn(Direction::Right);
                break;
            case SC_P:
            case input::SC_SPACE:
                game->toggle_pause();
                break;
            case SC_R:
                if (game->state() == Game::State::Over)
                    game->restart();
                break;
            case input::SC_ESCAPE:
            case SC_Q:
                running = false;
                break;
            default:
                break;
            }
        }

        const uint64_t now = ticks();
        if (game->state() != Game::State::Running) {
            /*  Paused or finished: keep the deadline ahead of the clock, so
             *  the snake does not take a burst of catch-up steps the moment
             *  the game resumes.  */
            next_step = now + game->step_ms();
        } else if (now >= next_step) {
            const bool ate = game->step();
            next_step = now + game->step_ms();

            if (ate)
                beep_eat(game->level());

            if (game->state() == Game::State::Over) {
                beep_over();
                if (game->score() > high_score) {
                    high_score = game->score();
                    (void)save_high_score(high_score);
                }
            }
        }

        draw_frame(*canvas, font ? &*font : nullptr, *game, high_score, frame);

        if (display)
            display->present(*canvas);
        else
            vga->present_reduced(*canvas);

        frame++;
        frame_timer.wait();
    }

    if (vga)
        vga->close();

    /*  Whatever is still in the keyboard queue was meant for the game, not for
     *  the shell prompt this returns to.  */
    keyboard.drain();

    printf("snake: score {}, best {}\n", game->score(), high_score);
    return 0;
}
