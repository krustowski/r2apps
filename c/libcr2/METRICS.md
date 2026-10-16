# Native sampled metrics

`metrics.h` provides a bounded, allocation-free round-robin collector for
r2. `metrics.c` is platform independent; `metrics_poll.c` reads each kernel
source once when a sample is due. No disk writes, networking, threads or
floating point are needed. This is a native format, not an upstream
RRDtool port or `.rrd` reader.

| Archive | Point interval | Retained history |
| --- | --- | --- |
| 0 | 1 second | 5 minutes |
| 1 | 10 seconds | 50 minutes |
| 2 | 60 seconds | 5 hours |

Each archive holds 300 points. Coarse points are integer averages of a full
interval, published after it ends. A missing observation invalidates only
its series in that interval. At startup, incomplete intervals remain
unknown. Gaps after long pauses take bounded work, even after days asleep.

The eight series are physical NIC RX/TX bytes per second, shared user heap
used/capacity in bytes, total/user tasks, runnable tasks (Ready or Running),
and blocked tasks. The NIC totals exclude loopback and include Ethernet
headers and padding; TX records submissions to the NIC. Unsupported NIC
calls remain unknown. Task counts are scheduler snapshots and provide no
CPU utilization measurement.

Initialize a static `R2Metrics` explicitly, then poll from a paced main loop:

```c
#include "metrics.h"
#include "syscall.h"

static R2Metrics history;

/* At startup */
r2_metrics_init(&history);

/* In the main loop; a second call within the same second does no work */
r2_metrics_poll(&history, get_ticks());

/* Read archive 0, oldest retained point first */
const R2MetricsRow *row = r2_metrics_row(&history, 0, 0);
if (row && (row->valid & (1U << R2_METRIC_RX_BPS))) {
    /* row->value[R2_METRIC_RX_BPS] is a measured rate */
}
```

`r2_metrics_push()` accepts an `R2MetricsInput` for another data source or a
test fixture. Each source has an availability flag; each output series has
a validity bit. The initial NIC snapshot establishes a baseline. Subsequent
rates use elapsed counter timestamps, with a 3-second heartbeat. Missing
sources, non-increasing timestamps and counter decreases break the series.
A clock rollback clears the history and establishes new baselines.

The collector occupies about 71 KiB per process. Reads are synchronous:
row pointers remain valid until the next push, and calls must run serially
or be protected by the caller. History is process-local and volatile.

GARN polls from its HTTP loop and exposes history through
[`/metrics`](../garn/README.md). Memento polls from its existing background
hook, sharing one history across Network and Tasks. The Charts tabs redraw
once per second, show missing values as gaps, and offer 5-minute, 50-minute
and 5-hour ranges. Click a range or press 1/2/3. Task charts show total tasks
and runnable tasks in green, plus the shared user heap in KiB; Network
charts show physical receive and transmit rates. Closing/reopening a window
preserves the desktop's history. Restarting the process clears it.

Plots have labeled value/time axes, a checkerboard grid, rounded value
ticks with magnitude suffixes, and time labels from the selected range to
`now`. The value axis identifies B/s, task counts or KiB. Labels become
sparser in small plots, and missing samples still break the plotted lines.

Build and verify from `r2_app/c`:

```sh
make libcr2
make -C garn test build
make -C libcr2/tests metrics-check metrics-ui-check
make -C ../cpp/memento-hello build
```

The collector tests cover elapsed rates, jitter, skipped samples, resets,
unavailable syscalls, complete-interval averages, 64-bit overflow, ring
overwrite and long pauses. GARN tests parse the actual JSON after simulated
TCP delivery, including retransmission, query bounds and maximum responses.
The chart tests run the production drawer with a recording bitmap, checking
line gaps, range controls, resizing and maximum 64-bit gauges. Both ELF
builds check r2's 2 MiB process image limit.
