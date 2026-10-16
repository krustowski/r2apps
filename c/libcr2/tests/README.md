# Network monitoring ABI test

`net_monitor.c` runs against the real kernel and C SDK. It checks the new
buffer sizes and pointer bounds, empty tables, port lookups, unbinding,
process exit and scheduler slot reuse, the global driver's full PID, physical
ARP RX/TX counters and loopback exclusion. Each test VM uses only this program,
so its driver registration and port changes cannot affect a running r2 system.

Build the text kernel in `r2_main`, then run:

```sh
CCACHE_DISABLE=1 make -C c libcr2
CCACHE_DISABLE=1 python3 c/libcr2/tests/smoke.py /path/to/kernel.elf
```

Run these commands from `r2_app`. The runner creates an ISO and FAT floppy in
a temporary directory, boots once with RTL8139 and once with E1000 using
QEMU's user networking, waits for `NETWORK ABI PASS` and stops each VM. It
requires GCC, NASM, mtools, `grub2-mkrescue` and `qemu-system-x86_64`.

`make -C c/libcr2/tests build` builds `NETMON.ELF` alone in `/tmp/r2-net-monitor`.

## Native metrics tests

`make -C c/libcr2/tests metrics-check metrics-ui-check` tests the native
collector and Memento's production chart renderer on the host. These tests
need no VM or network. They cover timing, counter resets, missing sources,
bounded retention, complete-interval averages, kernel polling, chart gaps,
history controls, labeled axes, checkerboard grid cells and resize bounds.
See [METRICS.md](../METRICS.md).
