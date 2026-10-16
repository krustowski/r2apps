CC ?= gcc
NASM ?= nasm
BUILD ?= /tmp/r2-net-monitor
SDK := $(abspath ..)
ARCHIVE := $(abspath ../../libcr2.a)
LINKER := $(abspath ../../linker.ld)
CFLAGS := -O2 -m64 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
          -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
          -I$(SDK) -Wall -Wextra

.PHONY: metrics-check metrics-ui-check
metrics-check: | $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -Werror -ffreestanding -I$(SDK) \
		metrics_test.c ../metrics.c ../metrics_poll.c -o $(BUILD)/metrics_test
	$(BUILD)/metrics_test

metrics-ui-check: | $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -Werror -ffreestanding -I$(SDK) -c ../metrics.c -o $(BUILD)/metrics_core.o
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-unused-function \
		metrics_charts.cpp $(BUILD)/metrics_core.o -o $(BUILD)/metrics_charts_test
	$(BUILD)/metrics_charts_test $(BUILD)/metrics_charts.ppm

.PHONY: build
build: $(BUILD)/NETMON.ELF

$(BUILD):
	mkdir -p $@

$(ARCHIVE):
	$(MAKE) -C ../.. libcr2

$(BUILD)/net_monitor.o: net_monitor.c $(wildcard $(SDK)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/crt0.o: $(SDK)/_crt0.asm | $(BUILD)
	$(NASM) -f elf64 -DR2_STACK_KB=64 $< -o $@

$(BUILD)/NETMON.ELF: $(BUILD)/net_monitor.o $(BUILD)/crt0.o $(ARCHIVE)
	$(CC) -nostdlib -nostartfiles -static -no-pie -Wl,-T,$(LINKER) $^ -o $@
