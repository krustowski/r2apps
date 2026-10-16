#include "metrics.h"
#include "syscall.h"

extern int puts(const char *);
extern void _Exit(int);
static R2Metrics db;
static uint32_t calls;
static uint8_t available = 1;
static int64_t task_count = 3;
static uint64_t fixture_ms;

static void check(uint8_t ok, const char *message) {
    if (!ok) { puts(message); _Exit(1); }
}
int64_t get_net_stats(NetStats_T *net) {
    calls++; *net = (NetStats_T){0}; net->timestamp_ms = fixture_ms;
    net->rx_bytes = fixture_ms * 2; net->tx_bytes = fixture_ms * 3;
    return available ? 0 : 0xfc;
}
int64_t read_meminfo(MemInfo_T *mem) {
    calls++; *mem = (MemInfo_T){0}; mem->heap_used = 1024; mem->heap_size = 4096;
    return available;
}
int64_t list_tasks(TaskInfo_T *tasks, uint8_t max) {
    calls++; (void)max;
    tasks[0] = (TaskInfo_T){0}; tasks[0].status = 0;
    tasks[1] = (TaskInfo_T){0}; tasks[1].mode = 1; tasks[1].status = 1;
    tasks[2] = (TaskInfo_T){0}; tasks[2].mode = 1; tasks[2].status = 3;
    return available ? task_count : 255;
}
static const R2MetricsRow *last(uint32_t archive) {
    return r2_metrics_row(&db, archive, db.archive[archive].count - 1);
}
static R2MetricsInput input(uint64_t ms) {
    R2MetricsInput in = {0};
    in.timestamp_ms = in.net_timestamp_ms = ms;
    in.rx_bytes = 0x100000000UL + ms * 2;
    in.tx_bytes = 0x200000000UL + ms * 3;
    in.heap_used = (uint64_t)-1; in.heap_size = (uint64_t)-1;
    in.tasks = 3; in.user_tasks = 2; in.runnable = 2; in.blocked = 1;
    in.available = R2_METRICS_NET | R2_METRICS_MEMORY | R2_METRICS_TASKS;
    return in;
}

int main(void) {
    r2_metrics_init(&db);
    R2MetricsInput in = input(0);
    check(r2_metrics_push(&db, &in), "first sample accepted at boot zero");
    check(!(last(0)->valid & 3) && last(0)->value[R2_METRIC_TASKS] == 3, "counter baseline is missing, gauges immediate");
    in = input(999); check(!r2_metrics_push(&db, &in), "one snapshot per second");
    in = input(1250); r2_metrics_push(&db, &in);
    check(last(0)->value[R2_METRIC_RX_BPS] == 2000 && last(0)->value[R2_METRIC_TX_BPS] == 3000,
          "rates use counter timestamps, not assumed one-second intervals");
    in = input(2750); r2_metrics_push(&db, &in);
    check(last(0)->value[R2_METRIC_RX_BPS] == 2000, "jitter does not distort rates");
    in = input(4000); r2_metrics_push(&db, &in);
    check(r2_metrics_row(&db, 0, 3)->timestamp_ms == 3000 && !r2_metrics_row(&db, 0, 3)->valid,
          "skipped second is an explicit gap");
    in = input(5000); in.rx_bytes = 0; r2_metrics_push(&db, &in);
    check(!(last(0)->valid & 1) && (last(0)->valid & 2), "RX reset breaks only RX, without underflow");
    in = input(6000); in.available = 0; r2_metrics_push(&db, &in);
    check(!last(0)->valid, "missing sources are gaps rather than stale or zero values");
    in = input(7000); r2_metrics_push(&db, &in);
    check(!(last(0)->valid & 3), "recovering NIC establishes a fresh baseline");
    in = input(7001); check(!r2_metrics_push(&db, &in), "request reads cannot add duplicate samples");
    in = input(8000); in.net_timestamp_ms = 7000; r2_metrics_push(&db, &in);
    check(!(last(0)->valid & 3), "non-increasing counter timestamp has no rate");
    in = input(15000); r2_metrics_push(&db, &in);
    check(!(last(0)->valid & 3), "heartbeat rejects rates spanning a long pause");

    r2_metrics_init(&db);
    for (uint64_t sec = 0; sec <= 900; sec++) { in = input(sec * 1000); r2_metrics_push(&db, &in); }
    check(db.archive[0].count == R2_METRICS_ROWS && r2_metrics_row(&db, 0, 0)->timestamp_ms == 601000,
          "bounded one-second ring overwrites oldest samples in chronological order");
    check(last(1)->timestamp_ms == 890000 && last(1)->valid == 255 && last(1)->value[R2_METRIC_RX_BPS] == 2000,
          "ten-second archive averages complete intervals");
    check(last(2)->timestamp_ms == 840000 && last(2)->value[R2_METRIC_HEAP_USED] == (uint64_t)-1,
          "one-minute averaging preserves maximum 64-bit values without overflow");
    check(!r2_metrics_row(&db, 3, 0) && !r2_metrics_row(&db, 0, 300), "out-of-range reads are rejected");
    in = input(1000000000UL); r2_metrics_push(&db, &in);
    check(db.archive[0].count == 300 && db.archive[1].count == 300 && db.archive[2].count == 300,
          "multi-day pause bounds gap work and storage in all archives");
    check(last(0)->valid == 252 && !last(1)->valid && !last(2)->valid, "pause does not fill historical zeros");
    in = input(1000); r2_metrics_push(&db, &in);
    check(db.archive[0].count == 1 && db.archive[1].count == 0 && !(last(0)->valid & 3),
          "clock rollback clears history and baseline");

    r2_metrics_init(&db);
    for (uint64_t sec = 0; sec <= 20; sec++) {
        in = input(sec * 1000);
        if (sec == 12) in.available &= ~R2_METRICS_MEMORY;
        r2_metrics_push(&db, &in);
    }
    check(!(last(1)->valid & (1U << R2_METRIC_HEAP_USED)) && (last(1)->valid & 1),
          "incomplete averages break only the unavailable series");

    r2_metrics_init(&db);
    for (uint64_t sec = 0; sec <= 20; sec++) {
        in = input(sec * 1000); in.heap_used = sec * sec; in.tasks = sec;
        r2_metrics_push(&db, &in);
    }
    check(last(1)->value[R2_METRIC_HEAP_USED] == 218 && last(1)->value[R2_METRIC_TASKS] == 14,
          "varying gauges average with integer truncation at aligned boundaries");
    r2_metrics_init(&db);
    for (uint64_t sec = 5; sec <= 10; sec++) { in = input(sec * 1000); r2_metrics_push(&db, &in); }
    check(!last(1)->valid, "startup in the middle of an interval remains incomplete");
    r2_metrics_init(&db);
    in = input(0); in.rx_bytes = 0; r2_metrics_push(&db, &in);
    in = input(1000); in.net_timestamp_ms = 1; in.rx_bytes = (uint64_t)-1; r2_metrics_push(&db, &in);
    check(last(0)->value[R2_METRIC_RX_BPS] == (uint64_t)-1, "unrepresentable rates saturate without wrapping");

    r2_metrics_init(&db); calls = 0; available = 1; fixture_ms = 0;
    r2_metrics_poll(&db, 0); r2_metrics_poll(&db, 10); r2_metrics_poll(&db, 999);
    check(calls == 3, "poll touches each source only once per second");
    fixture_ms = 1000; r2_metrics_poll(&db, fixture_ms);
    check(calls == 6 && last(0)->valid == 255 && last(0)->value[R2_METRIC_RUNNABLE] == 2 &&
          last(0)->value[R2_METRIC_BLOCKED] == 1 && last(0)->value[R2_METRIC_USER_TASKS] == 2,
          "production collector counts scheduler statuses and samples kernel NIC counters");
    fixture_ms = 2000; available = 0; r2_metrics_poll(&db, fixture_ms);
    check(!last(0)->valid, "busy and unsupported kernel calls stay unavailable");
    fixture_ms = 3000; available = 1; task_count = -1; r2_metrics_poll(&db, fixture_ms);
    check(!(last(0)->valid & (1U << R2_METRIC_TASKS)), "invalid task count cannot read unwritten entries");
    puts("Metrics: elapsed rates, resets, gaps, averaging, bounded history and kernel polling passed");
    return 0;
}
