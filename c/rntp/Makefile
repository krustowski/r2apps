.DEFAULT_GOAL := build
CC ?= gcc
HOST_CC ?= cc
LIBCR2 ?= ../libcr2
LIBCR2_A ?= ../libcr2.a
LINKER ?= ../linker.ld
BUILD ?= build
R2_MAIN ?= $(abspath ../../../r2_main)

CFLAGS = -m64 -O2 -static -nostdlib -nostdinc -mno-red-zone -ffreestanding \
         -fno-pie -fno-stack-protector -Wall -Wextra -Werror -I$(LIBCR2) -MMD -MP
HEADERS = protocol.h bridge.h sessions.h read_ahead.h

.PHONY: build install test linux test-linux test-fuse clean FORCE
FORCE:
build: rntp.elf

$(BUILD)/.dir:
	mkdir -p $(@D)
	touch $@
$(BUILD)/crt0.o: $(LIBCR2)/_crt0.asm Makefile | $(BUILD)/.dir
	nasm -f elf64 -DR2_STACK_KB=512 $< -o $@
$(BUILD)/%.o: %.c $(HEADERS) Makefile | $(BUILD)/.dir
	$(CC) $(CFLAGS) -c $< -o $@
$(LIBCR2_A): FORCE
	$(MAKE) -C .. libcr2
rntp.elf: $(BUILD)/main.o $(BUILD)/protocol.o $(BUILD)/sessions.o $(BUILD)/read_ahead.o $(BUILD)/crt0.o $(LIBCR2_A) $(LINKER)
	$(CC) -nostdlib -nostartfiles -static -no-pie \
	    $(BUILD)/main.o $(BUILD)/protocol.o $(BUILD)/sessions.o $(BUILD)/read_ahead.o $(BUILD)/crt0.o $(LIBCR2_A) \
	    -Wl,-T,$(LINKER),--defsym,__r2_image_end=_end -Wl,--build-id=none -o $@
	python3 ../../cpp/r2web/check_image.py $@

install: build
	mkdir -p $(R2_MAIN)/iso/bin
	cp rntp.elf $(R2_MAIN)/iso/bin/rntp.elf

test: $(BUILD)/read-ahead-test $(BUILD)/protocol-test $(BUILD)/sessions-test $(BUILD)/fat-test $(BUILD)/tcp-window-test $(BUILD)/tcp-packet-test
	./$(BUILD)/read-ahead-test
	./$(BUILD)/protocol-test
	./$(BUILD)/sessions-test
	./$(BUILD)/fat-test
	./$(BUILD)/tcp-window-test
	./$(BUILD)/tcp-packet-test
$(BUILD)/protocol-test: tests/protocol.c protocol.c protocol.h Makefile | $(BUILD)/.dir
	$(HOST_CC) -DRNTP_HOST -I. -Wall -Wextra -Werror tests/protocol.c protocol.c -o $@

clean:
	rm -rf $(BUILD) rntp.elf rntp-fuse

-include $(BUILD)/main.d $(BUILD)/protocol.d $(BUILD)/sessions.d $(BUILD)/read_ahead.d

$(BUILD)/tcp-window-test: tests/tcp_window.c tests/packet_shim.h $(LIBCR2)/net.c $(wildcard $(LIBCR2)/*.h) Makefile | $(BUILD)/.dir
	$(HOST_CC) -DR2_LIBC -O2 -ffreestanding -ffunction-sections -fdata-sections \
	    -I$(LIBCR2) -Wall -Wextra -Wno-pointer-sign tests/tcp_window.c $(LIBCR2)/net.c \
	    -Wl,--gc-sections -o $@

$(BUILD)/tcp-packet-test: tests/tcp_packets.c tests/packet_shim.h $(LIBCR2)/net.c $(wildcard $(LIBCR2)/*.h) Makefile | $(BUILD)/.dir
	$(HOST_CC) -DR2_LIBC -DRNTP_MSS_TEST -O2 -ffreestanding -ffunction-sections -fdata-sections \
	    -I$(LIBCR2) -Wall -Wextra -Wno-pointer-sign tests/tcp_packets.c $(LIBCR2)/net.c \
	    -Wl,--gc-sections -o $@

# Linux host tools do not use the R2 freestanding SDK. Install libfuse3's
# development package (fuse3-devel / libfuse3-dev) for the mount executable.
FUSE_CFLAGS ?= $(shell pkg-config --cflags fuse3)
FUSE_LIBS ?= $(shell pkg-config --libs fuse3)
HOST_FLAGS = -DRNTP_HOST -I. -std=c11 -O2 -g -Wall -Wextra -Werror
LINUX_SOURCES = linux/main.c linux/client.c linux/fat.c protocol.c
linux: rntp-fuse
rntp-fuse: $(LINUX_SOURCES) linux/client.h linux/fat.h protocol.h Makefile
	$(HOST_CC) $(HOST_FLAGS) $(FUSE_CFLAGS) $(LINUX_SOURCES) $(FUSE_LIBS) -pthread -o $@
$(BUILD)/sessions-test: tests/sessions.c tests/fixture.h sessions.c sessions.h protocol.c protocol.h | $(BUILD)/.dir
	$(HOST_CC) $(HOST_FLAGS) tests/sessions.c sessions.c protocol.c -o $@
$(BUILD)/fat-test: tests/fat.c tests/fixture.h linux/fat.c linux/fat.h | $(BUILD)/.dir
	$(HOST_CC) $(HOST_FLAGS) tests/fat.c linux/fat.c -o $@
$(BUILD)/server-test: tests/server.c tests/fixture.h sessions.c sessions.h protocol.c protocol.h | $(BUILD)/.dir
	$(HOST_CC) $(HOST_FLAGS) tests/server.c sessions.c protocol.c -o $@
$(BUILD)/client-test: tests/client.c linux/client.c linux/fat.c linux/client.h linux/fat.h protocol.c protocol.h | $(BUILD)/.dir
	$(HOST_CC) $(HOST_FLAGS) tests/client.c linux/client.c linux/fat.c protocol.c -pthread -o $@
test-linux: $(BUILD)/server-test $(BUILD)/client-test
	python3 tests/linux.py $(BUILD)
test-fuse: linux $(BUILD)/server-test
	python3 tests/fuse.py $(BUILD) ./rntp-fuse

$(BUILD)/read-ahead-test: tests/read_ahead.c read_ahead.c read_ahead.h protocol.h | $(BUILD)/.dir
	$(HOST_CC) $(HOST_FLAGS) tests/read_ahead.c read_ahead.c -o $@
