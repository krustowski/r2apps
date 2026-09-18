#
#  libc++r2 --- C++ runtime and standard library for the r2 kernel.
#
#    make            build libc++r2.a, _crt0.o and libc++r2compat.a
#    make check      run the container and allocator tests on the host
#    make examples   build the example programs
#    make clean
#
#  Sizes that matter, both overridable:
#
#    ARENA_BYTES     the heap arena, in .bss                (default 512 KiB)
#    STACK_BYTES     the private stack _crt0.asm sets up    (default 512 KiB)
#
#  Both come out of the 2 MiB frame the kernel maps privately at 0x600_000.
#  See README.md, "Memory map", before raising either.
#

NAME        := libc++r2
ROOT        := $(abspath .)
INCLUDE     := $(ROOT)/include
BUILD       := $(ROOT)/build
LIBRARY     := $(ROOT)/libc++r2.a
COMPAT_LIB  := $(ROOT)/libc++r2compat.a
CRT0_OBJ    := $(ROOT)/_crt0.o
LINKER      := $(ROOT)/linker.ld

CXX         ?= g++
AR          ?= ar
NASM        ?= nasm
OBJCOPY     ?= llvm-objcopy

#  Language standard.  C++23 by default; the library also builds and works as
#  C++17 (make STD=c++17), which is what cpp/memento-hello needs.  The C++20/23
#  headers --- compare, concepts, coroutine, expected --- are empty below C++20,
#  and R2_REQUIRES erases the constraints, so nothing else changes.
STD      ?= c++23
STD_FLAG := -std=$(STD)

ARENA_BYTES ?= 524288
STACK_BYTES ?= 524288

#  -nostdinc/-nostdinc++  nothing from the host system is in scope; the headers
#                         in include/ are the whole library surface
#  -fno-exceptions/-rtti  no unwinder and no type information on this target
#  -mno-red-zone          the kernel interrupts userland; the red zone is not
#                         safe below rsp when an interrupt frame lands there
#  -fno-pie               the image is linked at a fixed 0x600_000
CXXFLAGS := $(STD_FLAG) -m64 -O2 \
            -ffreestanding -nostdlib -nostdinc -nostdinc++ \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-pie -fcf-protection=none -mno-red-zone -fno-math-errno \
            -ffunction-sections -fdata-sections \
            -Wall -Wextra \
            -DR2CXX_ARENA_BYTES=$(ARENA_BYTES) \
            -I$(INCLUDE)

SOURCES := $(wildcard $(ROOT)/src/*.cpp)
OBJECTS := $(patsubst $(ROOT)/src/%.cpp,$(BUILD)/%.o,$(SOURCES))

COMPAT_SOURCES := $(wildcard $(ROOT)/src/compat/*.cpp)
COMPAT_OBJECTS := $(patsubst $(ROOT)/src/compat/%.cpp,$(BUILD)/compat_%.o,$(COMPAT_SOURCES))

.PHONY: all clean check examples install-examples

all: $(LIBRARY) $(CRT0_OBJ) $(COMPAT_LIB)

$(BUILD):
	@mkdir -p $(BUILD)

#  memcpy and friends must not be compiled into calls to themselves; the
#  source carries the same pragma so a hand-built object stays correct.
$(BUILD)/libc.o: $(ROOT)/src/libc.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -fno-tree-loop-distribute-patterns -c $< -o $@

$(BUILD)/%.o: $(ROOT)/src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/compat_%.o: $(ROOT)/src/compat/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIBRARY): $(OBJECTS)
	@rm -f $@
	$(AR) rcs $@ $(OBJECTS)
	@echo "built $@"

$(COMPAT_LIB): $(COMPAT_OBJECTS)
	@rm -f $@
	$(AR) rcs $@ $(COMPAT_OBJECTS)
	@echo "built $@"

$(CRT0_OBJ): $(ROOT)/src/_crt0.asm
	$(NASM) -f elf64 -DR2_STACK_BYTES=$(STACK_BYTES) $< -o $@
	@echo "built $@"

examples: all
	@$(MAKE) -C examples/hello build
	@$(MAKE) -C examples/gfxdemo build

#  Host-side tests: the containers, the string and the allocator are pure
#  memory and can be exercised natively, where a failure is a line number
#  rather than a triple fault in QEMU.
check:
	@$(MAKE) -C tests/host run

clean:
	@rm -rf $(BUILD) $(LIBRARY) $(COMPAT_LIB) $(CRT0_OBJ)
	@$(MAKE) -C examples/hello clean 2>/dev/null || true
	@$(MAKE) -C examples/gfxdemo clean 2>/dev/null || true
	@$(MAKE) -C tests/host clean 2>/dev/null || true
