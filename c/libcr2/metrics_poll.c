#include "metrics.h"
#include "syscall.h"

uint8_t r2_metrics_poll(R2Metrics *db, uint64_t now_ms) {
    if (!r2_metrics_due(db, now_ms)) return 0;
    R2MetricsInput input = {0}; input.timestamp_ms = now_ms;
    NetStats_T net = {0};
    if (get_net_stats(&net) == 0) {
        input.available |= R2_METRICS_NET;
        input.net_timestamp_ms = net.timestamp_ms;
        input.rx_bytes = net.rx_bytes; input.tx_bytes = net.tx_bytes;
    }
    MemInfo_T mem = {0};
    if (read_meminfo(&mem)) {
        input.available |= R2_METRICS_MEMORY;
        input.heap_used = mem.heap_used; input.heap_size = mem.heap_size;
    }
    TaskInfo_T tasks[R2_MAX_SLOTS];
    int64_t count = list_tasks(tasks, R2_MAX_SLOTS);
    if (count >= 0 && count <= R2_MAX_SLOTS) {
        input.available |= R2_METRICS_TASKS; input.tasks = (uint64_t)count;
        for (int64_t i = 0; i < count; i++) {
            input.user_tasks += tasks[i].mode != 0;
            input.runnable += tasks[i].status == 0 || tasks[i].status == 1;
            input.blocked += tasks[i].status == 3;
        }
    }
    return r2_metrics_push(db, &input);
}
