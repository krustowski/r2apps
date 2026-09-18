#
#  Host tests for libc++r2.
#
#  Built natively: our headers (-nostdinc++ keeps the host's C++ library out of
#  scope) plus the sources that touch nothing but memory.  io, fs, gfx and net
#  are left out because they make syscalls, and `int 0x7f` on Linux is a fault.
#

ROOT    := $(abspath ../..)
CXX     ?= g++

#  The older suite is deliberately built as C++17: the library has to keep
#  serving translation units that are not on C++23 yet (cpp/memento-hello is
#  one), and this is what proves it.
CXXFLAGS := -std=c++17 -m64 -O1 -g -nostdinc++ \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -Wall -Wextra \
            -DR2CXX_ARENA_BYTES=1048576 \
            -I$(ROOT)/include

CXX23FLAGS := $(subst -std=c++17,-std=c++23,$(CXXFLAGS))

SOURCES := test_main.cpp $(ROOT)/src/heap.cpp $(ROOT)/src/new.cpp $(ROOT)/src/string.cpp

ARENA_SOURCES := test_arena.cpp $(ROOT)/src/heap.cpp $(ROOT)/src/new.cpp
CXX23_SOURCES := test_cxx23.cpp $(ROOT)/src/heap.cpp $(ROOT)/src/new.cpp $(ROOT)/src/string.cpp \
                 panic_host.cpp

.PHONY: run build clean

run: build
	@./host_test
	@echo
	@./arena_test
	@echo
	@./cxx23_test

build: host_test arena_test cxx23_test

host_test: $(SOURCES)
	@$(CXX) $(CXXFLAGS) $(SOURCES) -o $@

arena_test: $(ARENA_SOURCES)
	@$(CXX) $(CXXFLAGS) $(ARENA_SOURCES) -o $@

cxx23_test: $(CXX23_SOURCES)
	@$(CXX) $(CXX23FLAGS) $(CXX23_SOURCES) -o $@

clean:
	@rm -f host_test arena_test cxx23_test
