#include "metrics.h"

static const uint32_t steps[R2_METRICS_ARCHIVES] = {1, 10, 60};

void r2_metrics_init(R2Metrics *db) {
    uint8_t *p = (uint8_t *)db;
    for (uint32_t i = 0; i < sizeof(*db); i++) p[i] = 0;
    for (uint32_t i = 0; i < R2_METRICS_ARCHIVES; i++) db->archive[i].step_seconds = steps[i];
}

const char *r2_metrics_name(uint32_t metric) {
    static const char *const names[R2_METRIC_COUNT] = {
        "rx_bytes_per_second", "tx_bytes_per_second", "heap_used_bytes", "heap_size_bytes",
        "tasks", "user_tasks", "runnable_tasks", "blocked_tasks"
    };
    return metric < R2_METRIC_COUNT ? names[metric] : "";
}

uint8_t r2_metrics_due(const R2Metrics *db, uint64_t now_ms) {
    return !db->started || now_ms < db->last_ms || now_ms / 1000 > db->last_ms / 1000;
}

const R2MetricsRow *r2_metrics_row(const R2Metrics *db, uint32_t archive, uint32_t index) {
    if (archive >= R2_METRICS_ARCHIVES) return 0;
    const R2MetricsArchive *a = &db->archive[archive];
    if (index >= a->count) return 0;
    return &a->rows[(a->next + R2_METRICS_ROWS - a->count + index) % R2_METRICS_ROWS];
}

static void store(R2MetricsArchive *a, const R2MetricsRow *row) {
    a->rows[a->next] = *row;
    a->next = (a->next + 1) % R2_METRICS_ROWS;
    if (a->count < R2_METRICS_ROWS) a->count++;
}

/* Fill only the gaps that can still be retained, even after days asleep. */
static void gaps(R2MetricsArchive *a, uint64_t begin, uint64_t end) {
    uint64_t step = (uint64_t)a->step_seconds * 1000;
    if (end <= begin) return;
    if ((end - begin) / step >= R2_METRICS_ROWS) {
        a->next = a->count = 0;
        begin = end - (uint64_t)R2_METRICS_ROWS * step;
    }
    for (; begin < end; begin += step) {
        R2MetricsRow empty = {0}; empty.timestamp_ms = begin; store(a, &empty);
    }
}

static void consolidate(R2MetricsArchive *a, const R2MetricsRow *row, uint8_t first) {
    uint64_t step = (uint64_t)a->step_seconds * 1000;
    uint64_t bucket = row->timestamp_ms / step * step;
    if (first) a->bucket_ms = bucket;
    if (bucket != a->bucket_ms) {
        R2MetricsRow average = {0}; average.timestamp_ms = a->bucket_ms;
        for (uint32_t i = 0; i < R2_METRIC_COUNT; i++) {
            /* Require the whole interval, including at startup. */
            if (a->seen[i] == a->step_seconds) {
                average.valid |= 1U << i;
                average.value[i] = a->quotient[i] + a->remainder[i] / a->step_seconds;
            }
            a->quotient[i] = a->remainder[i] = a->seen[i] = 0;
        }
        store(a, &average);
        gaps(a, a->bucket_ms + step, bucket);
        a->bucket_ms = bucket;
    }
    for (uint32_t i = 0; i < R2_METRIC_COUNT; i++) if (row->valid & (1U << i)) {
        /* Dividing before summing preserves 64-bit gauges without overflow. */
        a->quotient[i] += row->value[i] / a->step_seconds;
        a->remainder[i] += row->value[i] % a->step_seconds;
        a->seen[i]++;
    }
}

static uint64_t rate(uint64_t delta, uint64_t elapsed) {
    uint64_t whole = delta / elapsed, fraction = (delta % elapsed) * 1000 / elapsed;
    const uint64_t max = (uint64_t)-1;
    if (whole > max / 1000 || whole * 1000 > max - fraction) return max;
    return whole * 1000 + fraction;
}

uint8_t r2_metrics_push(R2Metrics *db, const R2MetricsInput *input) {
    if (!r2_metrics_due(db, input->timestamp_ms)) return 0;
    if (db->started && input->timestamp_ms < db->last_ms) r2_metrics_init(db);
    R2MetricsRow row = {0};
    row.timestamp_ms = input->timestamp_ms / 1000 * 1000;
    if ((input->available & R2_METRICS_NET) && db->have_net &&
        input->net_timestamp_ms > db->net_ms &&
        input->net_timestamp_ms - db->net_ms <= R2_METRICS_HEARTBEAT_MS) {
        uint64_t elapsed = input->net_timestamp_ms - db->net_ms;
        if (input->rx_bytes >= db->rx_bytes) {
            row.valid |= 1U << R2_METRIC_RX_BPS;
            row.value[R2_METRIC_RX_BPS] = rate(input->rx_bytes - db->rx_bytes, elapsed);
        }
        if (input->tx_bytes >= db->tx_bytes) {
            row.valid |= 1U << R2_METRIC_TX_BPS;
            row.value[R2_METRIC_TX_BPS] = rate(input->tx_bytes - db->tx_bytes, elapsed);
        }
    }
    db->have_net = (input->available & R2_METRICS_NET) != 0;
    db->net_ms = input->net_timestamp_ms; db->rx_bytes = input->rx_bytes; db->tx_bytes = input->tx_bytes;
    if (input->available & R2_METRICS_MEMORY) {
        row.valid |= (1U << R2_METRIC_HEAP_USED) | (1U << R2_METRIC_HEAP_SIZE);
        row.value[R2_METRIC_HEAP_USED] = input->heap_used;
        row.value[R2_METRIC_HEAP_SIZE] = input->heap_size;
    }
    if (input->available & R2_METRICS_TASKS) {
        row.valid |= (1U << R2_METRIC_TASKS) | (1U << R2_METRIC_USER_TASKS) |
                     (1U << R2_METRIC_RUNNABLE) | (1U << R2_METRIC_BLOCKED);
        row.value[R2_METRIC_TASKS] = input->tasks; row.value[R2_METRIC_USER_TASKS] = input->user_tasks;
        row.value[R2_METRIC_RUNNABLE] = input->runnable; row.value[R2_METRIC_BLOCKED] = input->blocked;
    }
    if (db->started) gaps(&db->archive[0], db->last_ms / 1000 * 1000 + 1000, row.timestamp_ms);
    store(&db->archive[0], &row);
    for (uint32_t a = 1; a < R2_METRICS_ARCHIVES; a++) consolidate(&db->archive[a], &row, !db->started);
    db->last_ms = input->timestamp_ms; db->started = 1;
    return 1;
}
