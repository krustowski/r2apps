# tcc for r2 (see README.md).
#
#   make build          tcc.elf for r2 and the files it needs there, in build/sysroot
#   make test           the first three below, each in QEMU
#   make test-tcct      step 1's smoke test, built by the host tcc
#   make test-libct     the libc test, built by gcc against this libc
#   make test-tcc       tcc on r2 compiling the libc and libcr2 tests, which then run
#   make test-image     the same against the r2 image as it is, tcc shipped in it
#   make libct-host     the libc test on Linux with glibc: the reference

TINYCC_URL := https://repo.or.cz/tinycc.git
TINYCC_REV := 43c7708b
TINYCC     := tinycc

B          := build
HOST       := $(abspath $(B)/host)
HOST_OBJ   := $(B)/host-obj
HOST_TCC   := $(HOST)/bin/tcc
LIBCR2     := ../libcr2
# laid out as the boot archive is: bin/, lib/, include/ (libcr2 in include/r2/)
SYSROOT    := $(B)/sysroot

GCC_INC    := $(shell gcc -print-file-name=include)
LIBGCC     := $(shell gcc -print-libgcc-file-name)

# libc and tcc.elf, built by gcc for r2.  The loop-pattern option keeps gcc
# from turning memset's own loop into a call to memset.
R2_CFLAGS  := -O2 -m64 -mno-red-zone -ffreestanding -fno-pie -fno-stack-protector \
              -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
              -nostdinc -I libc/include -isystem $(GCC_INC)
R2_LDFLAGS := -nostdlib -static -no-pie -Wl,-T,../linker.ld

# the same libc built by the host tcc, for tcc on r2 to link programs against
TCC_CFLAGS := -nostdinc -I libc/include -I $(TINYCC)/include

# libcr2's syscall.c goes into libc.a as r2sys.o.  Built with the libc's
# headers on the path, libcr2 leaves the C names to the libc (libcr2/types.h).
LIBC_NAMES := $(basename $(notdir $(wildcard libc/*.c))) setjmp r2sys
LIBC_DEPS  := libc/r2sys.h $(wildcard libc/include/*.h libc/include/sys/*.h $(LIBCR2)/*.h)
# The rest of libcr2 that is not the libc's already, as libcr2.a (-lcr2):
# its TCP/IP stack, u32_to_str and print_char.
CR2_NAMES  := net string printf

.PHONY: build tinycc host libc tcc sysroot test test-tcct test-libct test-tcc test-image \
	libct-host clean

build: sysroot

# ---- sources and the host compiler ----

tinycc: $(TINYCC)/tcc.c

$(TINYCC)/tcc.c:
	git clone -q $(TINYCC_URL) $(TINYCC)
	git -C $(TINYCC) checkout -q $(TINYCC_REV)

host: $(HOST_TCC)

# tcc labels itself from `git` run in its build directory, which is inside
# this repository: give it the label of its own checkout instead.
GITLABEL = GITHASH="$(shell git -C $(TINYCC) log -1 --date=short --pretty='format:%cd mob@%h')" GITMODF=

$(HOST_TCC): $(TINYCC)/tcc.c
	mkdir -p $(HOST_OBJ)
	cd $(HOST_OBJ) && $(abspath $(TINYCC))/configure --prefix=$(HOST) \
		--config-bcheck=no --config-backtrace=no >configure.log
	$(MAKE) -C $(HOST_OBJ) $(GITLABEL) >$(HOST_OBJ)/make.log
	$(MAKE) -C $(HOST_OBJ) $(GITLABEL) install >$(HOST_OBJ)/install.log

# ---- libc and tcc.elf, by gcc ----

libc: $(B)/gcc/libc.a

$(B)/gcc/%.o: libc/%.c $(LIBC_DEPS)
	@mkdir -p $(@D)
	gcc $(R2_CFLAGS) -c $< -o $@

$(B)/gcc/%.o: libc/%.S
	@mkdir -p $(@D)
	gcc -c $< -o $@

$(B)/gcc/crt1-512.o: libc/crt1.S
	@mkdir -p $(@D)
	gcc -DR2_STACK_KB=512 -c $< -o $@

$(B)/gcc/r2sys.o: $(LIBCR2)/syscall.c $(LIBC_DEPS)
	@mkdir -p $(@D)
	gcc $(R2_CFLAGS) -c $< -o $@

$(B)/gcc/libc.a: $(addprefix $(B)/gcc/,$(addsuffix .o,$(LIBC_NAMES)))
	rm -f $@
	ar rcs $@ $^

tcc: $(B)/tcc.elf

# tcc keeps its stack in the 2 MiB image as well: 512 KiB of it, next to
# ~350 KiB of code and ~170 KiB of tables.
$(B)/tcc.elf: r2/main.c r2/config.h r2/ide.h $(B)/gcc/crt1-512.o $(B)/gcc/libc.a $(HOST_TCC)
	gcc $(R2_CFLAGS) -I r2 -I $(HOST_OBJ) -I $(TINYCC) \
		-DTCC_VERSION=\"$(shell cat $(TINYCC)/VERSION)\" -c r2/main.c -o $(B)/gcc/tcc.o
	gcc $(R2_LDFLAGS) $(B)/gcc/crt1-512.o $(B)/gcc/tcc.o $(B)/gcc/libc.a $(LIBGCC) -o $@
	size $@

# ---- what tcc needs on r2, by the host tcc ----

$(B)/tcc/%.o: libc/%.c $(LIBC_DEPS) $(HOST_TCC)
	@mkdir -p $(@D)
	$(HOST_TCC) $(TCC_CFLAGS) -c $< -o $@

$(B)/tcc/%.o: libc/%.S $(HOST_TCC)
	@mkdir -p $(@D)
	$(HOST_TCC) -c $< -o $@

$(B)/tcc/r2sys.o: $(LIBCR2)/syscall.c $(LIBC_DEPS) $(HOST_TCC)
	@mkdir -p $(@D)
	$(HOST_TCC) $(TCC_CFLAGS) -c $< -o $@

$(B)/tcc/libc.a: $(addprefix $(B)/tcc/,$(addsuffix .o,$(LIBC_NAMES)))
	rm -f $@
	$(HOST_TCC) -ar rcs $@ $^

# No -I for libcr2 itself: its headers find each other, and its string.h would
# hide the libc's.
$(B)/tcc/cr2/%.o: $(LIBCR2)/%.c $(LIBC_DEPS) $(HOST_TCC)
	@mkdir -p $(@D)
	$(HOST_TCC) $(TCC_CFLAGS) -c $< -o $@

$(B)/tcc/libcr2.a: $(addprefix $(B)/tcc/cr2/,$(addsuffix .o,$(CR2_NAMES)))
	rm -f $@
	$(HOST_TCC) -ar rcs $@ $^

sysroot: $(SYSROOT)/.done

$(SYSROOT)/.done: $(B)/tcc.elf $(B)/tcc/libc.a $(B)/tcc/libcr2.a $(B)/tcc/crt1.o $(B)/tcc/crti.o \
		$(HOST_TCC)
	rm -rf $(SYSROOT)
	mkdir -p $(SYSROOT)/bin $(SYSROOT)/lib $(SYSROOT)/include/r2
	cp $(B)/tcc.elf $(SYSROOT)/bin/
	cp $(TINYCC)/include/*.h $(SYSROOT)/include/
	cp -r libc/include/. $(SYSROOT)/include/
	cp $(LIBCR2)/*.h $(SYSROOT)/include/r2/
	cp $(B)/tcc/libc.a $(B)/tcc/libcr2.a $(B)/tcc/crt1.o $(SYSROOT)/lib/
	cp $(B)/tcc/crti.o $(SYSROOT)/lib/crti.o
	cp $(B)/tcc/crti.o $(SYSROOT)/lib/crtn.o
	cp $(HOST)/lib/tcc/libtcc1.a $(SYSROOT)/lib/
	touch $@

# ---- tests ----

test: test-tcct test-libct test-tcc

# Step 1: the host tcc's output, against libcr2 alone.
$(B)/tcct.elf: tests/tcct.c tests/rt.c $(HOST_TCC)
	@mkdir -p $(B)/tcct
	nasm -f elf64 $(LIBCR2)/_crt0.asm -o $(B)/tcct/crt0.o
	$(HOST_TCC) -nostdlib -nostdinc -static -I $(LIBCR2) -Wl,-Ttext=0x600000 $(B)/tcct/crt0.o \
		tests/tcct.c $(LIBCR2)/printf.c $(LIBCR2)/syscall.c $(LIBCR2)/mem.c $(LIBCR2)/string.c \
		tests/rt.c $(HOST)/lib/tcc/libtcc1.a -o $@

test-tcct: $(B)/tcct.elf
	./run-qemu.sh TCCT.TXT $<:TCCT.ELF tests/tcct.rc:INIT.RC

$(B)/libct.elf: tests/libct.c $(B)/gcc/crt1.o $(B)/gcc/libc.a
	gcc $(R2_CFLAGS) -c $< -o $(B)/gcc/libct.o
	gcc $(R2_LDFLAGS) $(B)/gcc/crt1.o $(B)/gcc/libct.o $(B)/gcc/libc.a $(LIBGCC) -o $@

test-libct: $(B)/libct.elf
	./run-qemu.sh LIBCT.TXT $<:LIBCT.ELF tests/libct.rc:INIT.RC

TCC_TEST_FILES := tests/r2ct.c:R2CT.C tests/libct.c:LIBCT.C tests/tcc.rc:INIT.RC

test-tcc: sysroot
	R2_TAR_ADD=$(SYSROOT) ./run-qemu.sh R2CT.TXT,LIBCT.TXT $(TCC_TEST_FILES)

# tcc and its files as the image ships them (r2_main's build_iso): r2.iso as
# it is, graphics kernel and all
R2_MAIN ?= ../../../r2_main

test-image:
	R2_ISO=$(R2_MAIN)/r2.iso ./run-qemu.sh R2CT.TXT,LIBCT.TXT $(TCC_TEST_FILES)

libct-host:
	@mkdir -p $(B)/host-libct
	gcc -O2 -o $(B)/host-libct/libct tests/libct.c
	TZ=UTC $(B)/host-libct/libct $(B)/host-libct

clean:
	rm -rf $(B)
