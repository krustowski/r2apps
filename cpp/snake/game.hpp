/*
 *  game.hpp --- snake's rules, with no drawing, input or timing in them.
 *
 *  Shared by this example (main.cpp, which draws on the framebuffer or VGA)
 *  and Memento's Snake window (cpp/memento-hello/windows/snake_window.cpp,
 *  which draws in a window): both feed it turns and call step() when the
 *  snake is due to move, and draw what it says.
 *
 *  C++17, so that Memento, which is built as C++17, can include it too.
 */
#pragma once

#include <r2.hpp>

namespace snake {

using r2::nullopt;
using r2::optional;
using r2::vector;

/*  The board: 40x22 cells.  */
constexpr int32_t GRID_W = 40;
constexpr int32_t GRID_H = 22;
constexpr size_t CELL_COUNT = (size_t)GRID_W * (size_t)GRID_H;

constexpr uint64_t START_STEP_MS = 140;
constexpr uint64_t FASTEST_STEP_MS = 60;
constexpr uint32_t FOOD_PER_LEVEL = 5;
constexpr uint32_t START_LENGTH = 4;

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


} // namespace snake
