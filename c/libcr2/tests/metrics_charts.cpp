/* Exercise the production chart drawer with a recording bitmap. No Memento
 * runtime is needed; coordinates, series gaps and range controls are real. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#define _R2_TYPES_INCLUDED_

struct PlatformColor { uint32_t rgb; };
struct PlatformDrawingContext {
    std::deque<PlatformColor> colors;
    PlatformColor *CreateColor(uint32_t rgb, void *, void *) {
        colors.push_back({rgb & 0xffffff}); return &colors.back();
    }
};
enum class PlatformAlign { Begin, Middle, End };
enum class LineType { Solid };
struct PlatformDrawTextOptions { PlatformColor *foreground; PlatformAlign horizontalAlign; };
struct PlatformBitmap {
    int width, height;
    struct Line { int x1, y1, x2, y2; };
    struct Rect { int x, y, w, h; uint32_t color; };
    struct Text { int x, y, w, h; std::string value; };
    std::vector<Line> lines;
    std::vector<Rect> rects;
    std::vector<Text> texts;
    std::vector<std::string> labels;
    std::vector<uint32_t> pixels;
    PlatformBitmap(int w, int h) : width(w), height(h), pixels(w * h, 0xe0e0ff) {}
    void pixel(int x, int y, PlatformColor *c) {
        if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = c->rgb;
    }
    void FillRect(int x, int y, int w, int h, PlatformColor *c, bool) {
        if (w < 0 || h < 0) { std::puts("negative chart rectangle"); std::exit(1); }
        rects.push_back({x, y, w, h, c->rgb});
        for (int py = y; py < y + h; py++) for (int px = x; px < x + w; px++) pixel(px, py, c);
    }
    void DrawText(int x, int y, int w, int h, const char *s, PlatformDrawTextOptions *, bool) {
        labels.emplace_back(s); texts.push_back({x, y, w, h, s});
    }
    void DrawLine(int x, int y, int w, int h, int, LineType, PlatformColor *c, bool) {
        lines.push_back({x, y, x + w, y + h});
        if (x < 0 || x >= width || y < 0 || y >= height || x + w < 0 || x + w >= width || y + h < 0 || y + h >= height)
        { std::puts("chart line outside bitmap"); std::exit(1); }
        int dx = std::abs(w), dy = -std::abs(h), sx = w < 0 ? -1 : 1, sy = h < 0 ? -1 : 1, error = dx + dy;
        int endX = x + w, endY = y + h;
        for (;;) {
            pixel(x, y, c); if (x == endX && y == endY) break;
            int twice = error * 2;
            if (twice >= dy) { error += dy; x += sx; }
            if (twice <= dx) { error += dx; y += sy; }
        }
    }
};
struct TabStrip { static const int BELOW = 16; };

#include "../../../cpp/memento-hello/windows/metrics_charts.cpp"
extern "C" uint8_t r2_metrics_poll(R2Metrics *, uint64_t) { return 0; }

static void check(bool ok, const char *message) {
    if (!ok) { std::puts(message); std::exit(1); }
}
static bool label(const PlatformBitmap &b, const std::string &part) {
    for (const auto &s : b.labels) if (s.find(part) != std::string::npos) return true;
    return false;
}
static const PlatformBitmap::Text *exactLabel(const PlatformBitmap &b, const std::string &value) {
    for (const auto &s : b.texts) if (s.value == value) return &s;
    return nullptr;
}

int main(int argc, char **argv) {
    r2_metrics_init(&mementoMetrics);
    PlatformDrawingContext dc;
    PlatformColor dark{0x0000aa}, light{0xe0e0ff};
    PlatformDrawTextOptions opts{&dark, PlatformAlign::Begin};
    MetricsCharts charts;
    PlatformBitmap empty(306, 170);
    charts.draw(&dc, &empty, opts, &dark, &light, 306, 164, true);
    check(empty.lines.empty() && label(empty, "unavailable"), "empty history never invents chart lines");
    uint64_t rx = 0, tx = 0;
    for (uint64_t sec = 0; sec <= 300; sec++) {
        R2MetricsInput in{};
        in.timestamp_ms = in.net_timestamp_ms = sec * 1000;
        rx += (sec % 60) * 128; tx += ((sec + 30) % 60) * 256;
        in.rx_bytes = rx; in.tx_bytes = tx;
        in.heap_used = sec == 299 ? UINT64_MAX : 1024 * (1024 + sec * 4);
        in.heap_size = UINT64_MAX; in.tasks = 5 + sec % 4; in.runnable = sec % 4;
        in.available = R2_METRICS_NET | R2_METRICS_MEMORY | R2_METRICS_TASKS;
        if (sec == 150) in.available = 0;
        r2_metrics_push(&mementoMetrics, &in);
    }
    PlatformBitmap normal(306, 170);
    charts.draw(&dc, &normal, opts, &dark, &light, 306, 164, true);
    check(normal.lines.size() == 592, "missing source and fresh baseline break both network lines");
    check(label(normal, "NIC receive B/s: 0") && label(normal, "1 s/point"), "latest zero is valid and chart units explicit");
    int gapLeft = 42 + 150 * 257 / 300;
    int gapRight = 42 + 151 * 257 / 300;
    for (const auto &line : normal.lines)
        check(!(line.x1 < gapLeft && line.x2 > gapRight), "lines never bridge a missing second");
    check(exactLabel(normal, "B/s") && exactLabel(normal, "Time") && exactLabel(normal, "-5m") && exactLabel(normal, "now"),
          "axes name the value units and actual five-minute time span");
    check(exactLabel(normal, "10k") && exactLabel(normal, "5k") && exactLabel(normal, "20k") && exactLabel(normal, "0"),
          "value ticks use readable rounded ranges and zero baselines");
    int whiteCells = 0, grayCells = 0, gridLines = 0;
    for (const auto &rect : normal.rects) {
        whiteCells += rect.color == 0xffffff && rect.w > 1 && rect.h > 1;
        grayCells += rect.color == 0xe0e0e0 && rect.w > 1 && rect.h > 1;
        gridLines += rect.color == 0xbbbbbb && (rect.w == 1 || rect.h == 1);
    }
    check(whiteCells == 10 && grayCells == 10 && gridLines >= 12, "both plots contain checkerboard cells and horizontal/vertical grid lines");
    check(normal.pixels[47 * 306 + 45] == 0xffffff && normal.pixels[47 * 306 + 100] == 0xe0e0e0 &&
          normal.pixels[62 * 306 + 45] == 0xe0e0e0, "neighboring grid cells alternate in both directions");
    const auto *upperLabel = exactLabel(normal, "10k"), *timeLabel = exactLabel(normal, "Time");
    check(upperLabel->x + upperLabel->w < 42 && timeLabel->y > normal.lines.front().y1,
          "axis text leaves the plotted data clear");
    check(!charts.click(10, 30) && charts.click(90, 20), "range control hit test");
    PlatformBitmap coarse(306, 170);
    charts.draw(&dc, &coarse, opts, &dark, &light, 306, 164, true);
    check(label(coarse, "10 s/point"), "50-minute range uses ten-second archive");
    check(exactLabel(coarse, "-50m") && exactLabel(coarse, "now"), "coarse time axis labels the full selected range");
    check(!coarse.lines.empty() && coarse.lines.back().x2 < 299, "completed intervals remain behind the true now tick");
    check(charts.key('3') && !charts.key('9'), "range shortcut bounds");
    PlatformBitmap tasks(290, 160);
    charts.draw(&dc, &tasks, opts, &dark, &light, 290, 138, false);
    check(label(tasks, "Shared user heap KiB") && label(tasks, "Tasks / runnable"), "task charts use measured gauges");
    check(exactLabel(tasks, "tasks") && exactLabel(tasks, "KiB") && exactLabel(tasks, "-5h"), "task and heap axes identify units and five-hour range");
    check(charts.key('1'), "return to live range");
    PlatformBitmap large(640, 400);
    charts.draw(&dc, &large, opts, &dark, &light, 640, 375, false);
    check(!large.lines.empty(), "maximum 64-bit heap gauge draws safely at enlarged size");
    bool compactLargeNumber = false;
    for (const auto &s : large.texts)
        if (s.x == 6 && s.w == 32 && s.value.size() <= 7 && s.value.find('P') != std::string::npos) compactLargeNumber = true;
    check(compactLargeNumber, "large gauge ticks fit the value gutter with magnitude suffixes");
    PlatformBitmap narrow(180, 170);
    charts.draw(&dc, &narrow, opts, &dark, &light, 180, 164, true);
    check(exactLabel(narrow, "-2m30s") && exactLabel(narrow, "now"), "narrow plots reduce time tick density without rounding ages");
    PlatformBitmap compact(200, 90);
    charts.draw(&dc, &compact, opts, &dark, &light, 200, 78, false);
    check(compact.lines.empty(), "small window omits graphs without invalid coordinates");
    if (argc > 1) {
        FILE *f = std::fopen(argv[1], "wb"); check(f != nullptr, "preview file opens");
        std::fprintf(f, "P6\n306 170\n255\n");
        for (uint32_t p : normal.pixels) {
            unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    std::puts("Memento charts: axes, checkerboard grid, tick labels, gaps, ranges and resize bounds passed");
}
