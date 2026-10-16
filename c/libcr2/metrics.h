#ifndef R2_METRICS_H
#define R2_METRICS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native, process-local round-robin history. Not the upstream .rrd format.
 * No allocation, disk I/O, floating point, or platform calls in push/read. */
#define R2_METRICS_ROWS 300
#define R2_METRICS_ARCHIVES 3
#define R2_METRICS_HEARTBEAT_MS 3000

enum {
    R2_METRIC_RX_BPS, R2_METRIC_TX_BPS,
    R2_METRIC_HEAP_USED, R2_METRIC_HEAP_SIZE,
    R2_METRIC_TASKS, R2_METRIC_USER_TASKS,
    R2_METRIC_RUNNABLE, R2_METRIC_BLOCKED,
    R2_METRIC_COUNT
};
enum { R2_METRICS_NET = 1, R2_METRICS_MEMORY = 2, R2_METRICS_TASKS = 4 };

typedef struct {
    uint64_t timestamp_ms;
    uint64_t value[R2_METRIC_COUNT];
    uint32_t valid; /* bit per series; missing values are not zero */
} R2MetricsRow;

typedef struct {
    R2MetricsRow rows[R2_METRICS_ROWS];
    uint32_t next, count, step_seconds;
    uint64_t bucket_ms;
    uint64_t quotient[R2_METRIC_COUNT], remainder[R2_METRIC_COUNT];
    uint32_t seen[R2_METRIC_COUNT];
} R2MetricsArchive;

typedef struct {
    uint64_t timestamp_ms, net_timestamp_ms, rx_bytes, tx_bytes;
    uint64_t heap_used, heap_size, tasks, user_tasks, runnable, blocked;
    uint32_t available;
} R2MetricsInput;

typedef struct {
    R2MetricsArchive archive[R2_METRICS_ARCHIVES];
    uint64_t last_ms, net_ms, rx_bytes, tx_bytes;
    uint8_t started, have_net;
} R2Metrics;

void r2_metrics_init(R2Metrics *db);
/* Accept at most one snapshot per monotonic second. Clock rollback resets
 * history. Long pauses insert bounded gaps rather than inventing samples. */
uint8_t r2_metrics_push(R2Metrics *db, const R2MetricsInput *input);
uint8_t r2_metrics_due(const R2Metrics *db, uint64_t now_ms);
/* Read each kernel source once when due; busy/unsupported sources are gaps. */
uint8_t r2_metrics_poll(R2Metrics *db, uint64_t now_ms);
/* Index 0 is the oldest retained row. Pointer lives until the next push. */
const R2MetricsRow *r2_metrics_row(const R2Metrics *db, uint32_t archive, uint32_t index);
const char *r2_metrics_name(uint32_t metric);

#ifdef __cplusplus
}
#endif
#endif
