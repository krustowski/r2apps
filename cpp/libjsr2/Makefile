#
#  libjsr2 --- QuickJS for r2, with an event loop and the Web APIs that need
#  no document.  See README.md.
#
#    make            libjsr2.a for r2 programs (and the host compiler, jsr2c)
#    make host       obj/host/libjsr2.a, the same library for host tests
#    make test       the host tests (tests/*.js)
#    make test-r2    most of them again, on r2 in QEMU (tests/run-qemu.sh)
#    make cli        js.elf: scripts on r2, without a window
#    make clean
#
#  Embedders link libjsr2.a, add -I$(LIBJSR2)/include -I$(LIBJSR2)/port
#  -I$(QUICKJS) to their r2 flags, and compile their own scripts with
#  $(LIBJSR2)/obj/host/jsr2c (see JSR2C below).
#
.DEFAULT_GOAL := all
ROOT     := $(abspath ..)
QJS      := $(ROOT)/third_party/quickjs
LIBM     := $(ROOT)/third_party/musl-libm
LIBCXXR2 := $(ROOT)/libc++r2
BDIR     := obj
GCC_INC  := $(shell gcc -print-file-name=include)
QJS_VERSION := $(shell cat $(QJS)/VERSION)
QJS_SRCS := quickjs libregexp libunicode cutils dtoa
LIBM_SRCS := $(basename $(notdir $(wildcard $(LIBM)/src/*.c)))
FLOPPY   ?= $(abspath ../../fat.img)

#  ── r2 ────────────────────────────────────────────────────────────────────
#  -Os like the rest of the r2 programs: the engine shares the 2 MiB image
#  with its embedder.  The C library comes from libc++r2 and port/.
R2_COMMON := -m64 -Os -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INC) -fno-pie -mno-red-zone \
 -fno-stack-protector -fno-asynchronous-unwind-tables -fcf-protection=none -ffunction-sections \
 -fdata-sections -fno-math-errno -MMD -MP
QJS_R2_CFLAGS := -std=gnu11 $(R2_COMMON) -Iport -I$(QJS) -DCONFIG_R2 -DNDEBUG \
 -DCONFIG_VERSION='"$(QJS_VERSION)"' -w
#  musl builds its libm with these two; they keep rounding and excess
#  precision as C99 has them.
LIBM_CFLAGS := -std=c99 $(R2_COMMON) -I$(LIBM)/include -I$(LIBM)/src -include $(LIBM)/names.h \
 -fexcess-precision=standard -frounding-math -w
JSR2_R2_CFLAGS := -std=gnu11 $(R2_COMMON) -Iport -Wall
JSR2_R2_CXXFLAGS := -std=c++17 $(R2_COMMON) -nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics \
 -Iport -Iinclude -isystem $(QJS) -DCONFIG_R2 -Wall -Wextra

R2_OBJS := $(QJS_SRCS:%=$(BDIR)/r2/qjs/%.o) $(LIBM_SRCS:%=$(BDIR)/r2/libm/%.o) \
 $(addprefix $(BDIR)/r2/,libc.o heap.o engine.o prelude.o)

#  ── host ──────────────────────────────────────────────────────────────────
HOST_QJS_CFLAGS := -std=gnu11 -O2 -g -I$(QJS) -D_GNU_SOURCE -DCONFIG_VERSION='"$(QJS_VERSION)"' -w -MMD -MP
HOST_CXXFLAGS := -std=c++17 -O1 -g -Iinclude -isystem $(QJS) -Wall -Wextra -fno-exceptions -fno-rtti -MMD -MP
HOST_QJS_OBJS := $(QJS_SRCS:%=$(BDIR)/host/qjs/%.o)
HOST_OBJS := $(HOST_QJS_OBJS) $(addprefix $(BDIR)/host/,heap.o engine.o prelude.o)
HOST_LIBS := -lm -lpthread
JSR2C := $(BDIR)/host/jsr2c

.PHONY: all host test test-r2 cli clean install
all: libjsr2.a
host: $(BDIR)/host/libjsr2.a

libjsr2.a: $(R2_OBJS)
	@rm -f $@
	ar rcs $@ $^
	@size -t $^ | tail -1 | awk '{printf "libjsr2.a: %d bytes of code and data\n", $$1+$$2}'

$(BDIR)/r2/qjs/%.o: $(QJS)/%.c
	@mkdir -p $(dir $@)
	gcc -c $(QJS_R2_CFLAGS) $< -o $@
$(BDIR)/r2/libm/%.o: $(LIBM)/src/%.c
	@mkdir -p $(dir $@)
	@gcc -c $(LIBM_CFLAGS) $< -o $@
$(BDIR)/r2/libc.o: src/libc.c
	@mkdir -p $(dir $@)
	gcc -c $(JSR2_R2_CFLAGS) $< -o $@
$(BDIR)/r2/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	g++ -c $(JSR2_R2_CXXFLAGS) $< -o $@
$(BDIR)/r2/prelude.o: $(BDIR)/gen/prelude.c
	@mkdir -p $(dir $@)
	gcc -c $(JSR2_R2_CFLAGS) $< -o $@

$(BDIR)/host/qjs/%.o: $(QJS)/%.c
	@mkdir -p $(dir $@)
	gcc -c $(HOST_QJS_CFLAGS) $< -o $@
$(BDIR)/host/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	g++ -c $(HOST_CXXFLAGS) $< -o $@
$(BDIR)/host/prelude.o: $(BDIR)/gen/prelude.c
	@mkdir -p $(dir $@)
	gcc -c -O1 $< -o $@
$(BDIR)/host/libjsr2.a: $(HOST_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(JSR2C): tools/jsr2c.c $(HOST_QJS_OBJS)
	@mkdir -p $(dir $@)
	gcc -O1 -g -I$(QJS) $^ $(HOST_LIBS) -o $@
$(BDIR)/gen/prelude.c: src/prelude.js $(JSR2C)
	@mkdir -p $(dir $@)
	$(JSR2C) $< $@ jsr2_prelude

#  ── tests ─────────────────────────────────────────────────────────────────
TESTS := $(wildcard tests/*.test.js)
$(BDIR)/host/run: tests/run.cpp $(BDIR)/host/libjsr2.a
	g++ $(HOST_CXXFLAGS) $< $(BDIR)/host/libjsr2.a $(HOST_LIBS) -o $@
test: $(BDIR)/host/run
	$(BDIR)/host/run tests/harness.js $(TESTS)
#  The same tests (less the scripted network's) in js.elf on r2, in QEMU.
test-r2: js.elf
	tests/run-qemu.sh

#  ── js.elf ────────────────────────────────────────────────────────────────
#  A libc++r2 program like the rest; 256 KiB of stack, as r2web has, so a
#  script that runs here has the same room in the browser.
CLI_CXXFLAGS := -std=c++17 -m64 -Os -ffreestanding -nostdlib -nostdinc -nostdinc++ -fno-exceptions -fno-rtti \
 -fno-threadsafe-statics -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pie -fcf-protection=none \
 -mno-red-zone -fno-math-errno -ffunction-sections -fdata-sections -I$(LIBCXXR2)/include -Iinclude -Iport \
 -isystem $(QJS) -isystem $(GCC_INC) -Wall -Wextra -MMD -MP
cli: js.elf
$(BDIR)/cli/_crt0.o: $(LIBCXXR2)/src/_crt0.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 -DR2_STACK_BYTES=262144 $< -o $@
$(BDIR)/cli/main.o: cli/main.cpp
	@mkdir -p $(dir $@)
	g++ -c $(CLI_CXXFLAGS) $< -o $@
$(LIBCXXR2)/libc++r2.a $(LIBCXXR2)/libc++r2compat.a:
	$(MAKE) -C $(LIBCXXR2) all
js.elf: $(BDIR)/cli/_crt0.o $(BDIR)/cli/main.o libjsr2.a $(LIBCXXR2)/libc++r2.a $(LIBCXXR2)/libc++r2compat.a
	g++ -nostdlib -nostartfiles -static -no-pie -Wl,--gc-sections -Wl,-T,$(LIBCXXR2)/linker.ld $^ -lgcc -o $@
	@python3 ../r2web/check_image.py $@
install: js.elf
	mcopy -o -i $(FLOPPY) js.elf ::JS.ELF

clean:
	rm -rf $(BDIR) libjsr2.a js.elf

-include $(shell find $(BDIR) -name '*.d' 2>/dev/null)
