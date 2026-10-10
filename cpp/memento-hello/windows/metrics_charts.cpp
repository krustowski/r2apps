// Shared native history; the desktop's background hook samples even when
// these windows are closed, hidden, or showing another tab.
#include "../../../c/libcr2/metrics.h"

static R2Metrics mementoMetrics;
static bool mementoMetricsReady = false;
static void sampleMementoMetrics(uint64_t now)
{
    if (!mementoMetricsReady) { r2_metrics_init(&mementoMetrics); mementoMetricsReady = true; }
    r2_metrics_poll(&mementoMetrics, now);
}

class MetricsCharts
{
public:
    bool click(int x, int y)
    {
        if (y < TabStrip::BELOW || y >= TabStrip::BELOW + 11) return false;
        for (int i = 0; i < 3; i++)
            if (x >= 48 + i * 40 && x < 86 + i * 40) { archive = i; return true; }
        return false;
    }
    bool key(char c)
    {
        if (c < '1' || c > '3') return false;
        archive = c - '1'; return true;
    }
    void draw(PlatformDrawingContext *dc, PlatformBitmap *target, PlatformDrawTextOptions &opts,
              PlatformColor *dark, PlatformColor *light, int width, int bottom, bool network)
    {
        if (!accent) accent = dc->CreateColor(0xFF008800, nullptr, nullptr);
        if (!paper) paper = dc->CreateColor(0xFFFFFFFF, nullptr, nullptr);
        if (!shade) shade = dc->CreateColor(0xFFE0E0E0, nullptr, nullptr);
        if (!grid) grid = dc->CreateColor(0xFFBBBBBB, nullptr, nullptr);
        opts.foreground = dark; opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(6, TabStrip::BELOW, 40, 11, "History:", &opts, false);
        const char *labels[3] = {"5 min", "50 min", "5 hour"};
        for (int i = 0; i < 3; i++)
        {
            int x = 48 + i * 40;
            target->FillRect(x, TabStrip::BELOW, 38, 11, dark, false);
            if (archive != i) target->FillRect(x + 1, TabStrip::BELOW + 1, 36, 9, light, false);
            opts.foreground = archive == i ? light : dark;
            opts.horizontalAlign = PlatformAlign::Middle;
            target->DrawText(x, TabStrip::BELOW, 38, 11, labels[i], &opts, false);
        }
        opts.foreground = dark; opts.horizontalAlign = PlatformAlign::Begin;
        int y = TabStrip::BELOW + 14, height = (bottom - 12 - y) / 2;
        if (height >= 44 && width >= 120)
        {
            if (network)
            {
                graph(target, opts, dark, width, y, height, "NIC receive B/s", "B/s", R2_METRIC_RX_BPS, -1, 1);
                graph(target, opts, dark, width, y + height, height, "NIC transmit B/s", "B/s", R2_METRIC_TX_BPS, -1, 1);
            }
            else
            {
                graph(target, opts, dark, width, y, height, "Tasks / runnable (green)", "tasks", R2_METRIC_TASKS, R2_METRIC_RUNNABLE, 1);
                graph(target, opts, dark, width, y + height, height, "Shared user heap KiB", "KiB", R2_METRIC_HEAP_USED, -1, 1024);
            }
        }
        char note[96] = "1 s samples; ";
        addNumber(note, mementoMetrics.archive[archive].step_seconds);
        add(note, " s/point; 1/2/3: range; gaps: unavailable");
        opts.foreground = dark;
        target->DrawText(6, bottom - 11, width - 12, 10, note, &opts, false);
    }

private:
    int archive = 0;
    PlatformColor *accent = nullptr;
    PlatformColor *paper = nullptr, *shade = nullptr, *grid = nullptr;
    static void add(char *out, const char *s) { strcpy(out + strlen(out), s); }
    static void addNumber(char *out, uint64_t n)
    {
        char digits[24]; int count = 0;
        do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
        int at = (int)strlen(out); while (count) out[at++] = digits[--count]; out[at] = 0;
    }
    static int scale(uint64_t value, uint64_t peak, int height)
    {
        // Reduce both operands before the multiply, including UINT64_MAX.
        while (peak > (uint64_t)-1 / (uint64_t)height) { value >>= 1; peak >>= 1; }
        return peak ? (int)(value * (uint64_t)height / peak) : 0;
    }
    static uint64_t niceStep(uint64_t value)
    {
        uint64_t power = 1;
        while (value / power > 10) power *= 10;
        uint64_t units = value / power + (value % power != 0);
        uint64_t rounded = units <= 1 ? 1 : units <= 2 ? 2 : units <= 5 ? 5 : 10;
        return power > (uint64_t)-1 / rounded ? (uint64_t)-1 : power * rounded;
    }
    static void tickLabel(char *out, uint64_t value)
    {
        static const char suffix[] = " kMGTPE";
        uint64_t divisor = 1; int unit = 0;
        while (value / divisor >= 1000 && unit < 6) { divisor *= 1000; unit++; }
        out[0] = 0; addNumber(out, value / divisor);
        if (unit)
        {
            uint64_t tenth = (value % divisor) / (divisor / 10);
            if (tenth) { add(out, "."); addNumber(out, tenth); }
            char ending[2] = {suffix[unit], 0}; add(out, ending);
        }
    }
    static void timeLabel(char *out, uint64_t seconds)
    {
        out[0] = 0;
        if (!seconds) { add(out, "now"); return; }
        add(out, "-");
        if (seconds % 3600 == 0) { addNumber(out, seconds / 3600); add(out, "h"); }
        else if (seconds >= 60)
        {
            addNumber(out, seconds / 60); add(out, "m");
            if (seconds % 60) { addNumber(out, seconds % 60); add(out, "s"); }
        }
        else { addNumber(out, seconds); add(out, "s"); }
    }
    void axes(PlatformBitmap *target, PlatformDrawTextOptions &opts, PlatformColor *dark,
              int left, int top, int w, int h, uint64_t upper, uint64_t spanSeconds, const char *unit)
    {
        int columns = w >= 200 ? 5 : 2, divisions = h >= 60 ? 4 : 2;
        // The alternating cells remain distinct in the graphics kernel's
        // indexed palette; grid lines are lighter than axes and data.
        for (int row = 0; row < divisions; row++)
            for (int column = 0; column < columns; column++)
            {
                int x = left + (w - 1) * column / columns;
                int y = top + (h - 1) * row / divisions;
                int right = left + (w - 1) * (column + 1) / columns;
                int bottom = top + (h - 1) * (row + 1) / divisions;
                PlatformColor *color = (row + column) % 2 ? shade : paper;
                if (color) target->FillRect(x, y, right - x, bottom - y, color, false);
            }
        for (int column = 0; column <= columns; column++)
        {
            int x = left + (w - 1) * column / columns;
            target->FillRect(x, top, 1, h, column == 0 ? dark : grid ? grid : dark, false);
            target->FillRect(x, top + h - 1, 1, 3, dark, false);
            char label[24]; timeLabel(label, spanSeconds * (columns - column) / columns);
            opts.foreground = dark;
            opts.horizontalAlign = column == 0 ? PlatformAlign::Begin : column == columns ? PlatformAlign::End : PlatformAlign::Middle;
            int labelX = column == 0 ? x : column == columns ? x - 35 : x - 18;
            target->DrawText(labelX, top + h + 5, 36, 9, label, &opts, false);
        }
        for (int row = 0; row <= divisions; row++)
        {
            int y = top + (h - 1) * row / divisions;
            target->FillRect(left, y, w, 1, row == divisions ? dark : grid ? grid : dark, false);
            target->FillRect(left - 2, y, 3, 1, dark, false);
            if (h < 24 && row != 0 && row != divisions) continue;
            uint64_t value = upper / divisions * (divisions - row) + upper % divisions * (divisions - row) / divisions;
            char label[24]; tickLabel(label, value);
            opts.foreground = dark; opts.horizontalAlign = PlatformAlign::End;
            target->DrawText(6, y - 4, left - 10, 9, label, &opts, false);
        }
        opts.foreground = dark; opts.horizontalAlign = PlatformAlign::Begin;
        target->DrawText(6, top - 13, left - 10, 10, unit, &opts, false);
        target->DrawText(6, top + h + 5, left - 10, 9, "Time", &opts, false);
    }
    void graph(PlatformBitmap *target, PlatformDrawTextOptions &opts, PlatformColor *dark,
               int width, int y, int height, const char *label, const char *unit, int primary, int secondary, uint64_t divisor)
    {
        const R2MetricsArchive &a = mementoMetrics.archive[archive];
        uint64_t step = (uint64_t)a.step_seconds * 1000;
        uint64_t end = mementoMetrics.last_ms / 1000 * 1000;
        uint64_t spanSeconds = (uint64_t)R2_METRICS_ROWS * a.step_seconds;
        uint64_t peak = 0;
        for (uint32_t i = 0; i < a.count; i++)
        {
            const R2MetricsRow *r = r2_metrics_row(&mementoMetrics, archive, i);
            for (int series = 0; series < 2; series++)
            {
                int m = series ? secondary : primary;
                if (m >= 0 && (r->valid & (1U << m)) && r->value[m] / divisor > peak) peak = r->value[m] / divisor;
            }
        }
        char title[128]; strcpy(title, label); add(title, ": ");
        const R2MetricsRow *last = a.count ? r2_metrics_row(&mementoMetrics, archive, a.count - 1) : nullptr;
        if (last && (last->valid & (1U << primary))) addNumber(title, last->value[primary] / divisor);
        else add(title, "unavailable");
        add(title, "  max "); addNumber(title, peak);
        opts.foreground = dark;
        int left = 42, top = y + 14, w = width - left - 6, h = height - 30;
        target->DrawText(left, y, w, 10, title, &opts, false);
        uint64_t divisions = h >= 60 ? 4 : 2;
        uint64_t interval = peak / divisions + (peak % divisions != 0);
        if (!interval) interval = 1;
        if (primary != R2_METRIC_TASKS) interval = niceStep(interval);
        uint64_t upper = interval > (uint64_t)-1 / divisions ? (uint64_t)-1 : interval * divisions;
        axes(target, opts, dark, left, top, w, h, upper, spanSeconds, unit);
        for (int series = 0; series < 2; series++)
        {
            int m = series ? secondary : primary;
            if (m < 0) continue;
            bool havePrevious = false; int px = 0, py = 0; uint64_t previousMs = 0;
            PlatformColor *color = m == primary || !accent ? dark : accent;
            for (uint32_t i = 0; i < a.count; i++)
            {
                const R2MetricsRow *r = r2_metrics_row(&mementoMetrics, archive, i);
                if (!(r->valid & (1U << m)) || r->timestamp_ms > end || end - r->timestamp_ms > spanSeconds * 1000)
                { havePrevious = false; continue; }
                int x = left + (int)((spanSeconds * 1000 - (end - r->timestamp_ms)) * (uint64_t)(w - 1) / (spanSeconds * 1000));
                int ry = top + h - 1 - scale(r->value[m] / divisor, upper, h - 1);
                if (havePrevious && r->timestamp_ms - previousMs == step)
                    target->DrawLine(px, py, x - px, ry - py, 1, LineType::Solid, color, false);
                target->FillRect(x, ry, 1, 1, color, false);
                havePrevious = true; px = x; py = ry; previousMs = r->timestamp_ms;
            }
        }
    }
};
