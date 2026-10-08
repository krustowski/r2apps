LIBCXXR2 ?= $(abspath ../../libc++r2)
CXXFLAGS := -std=c++17 -m64 -O1 -g -nostdinc++ -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -Wall -Wextra -DR2CXX_ARENA_BYTES=8388608 \
            -Iinclude -I$(LIBCXXR2)/include
SOURCES := test_main.cpp ../lists.cpp ../sha256.cpp ../store.cpp \
           $(LIBCXXR2)/src/heap.cpp $(LIBCXXR2)/src/heap_arena.cpp \
           $(LIBCXXR2)/src/new.cpp $(LIBCXXR2)/src/string.cpp \
           $(LIBCXXR2)/tests/host/panic_host.cpp
.PHONY: run target clean
run: host_test
	./host_test
	python3 test_mksums.py
host_test: $(SOURCES) ../jug.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@
clean:
	rm -f host_test jugtest.elf shipped.elf

TARGET_FLAGS := -std=c++17 -m64 -Os -ffreestanding -nostdlib -nostdinc -nostdinc++ \
                -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
                -fno-asynchronous-unwind-tables -fno-pie -fcf-protection=none -mno-red-zone \
                -ffunction-sections -fdata-sections -I$(LIBCXXR2)/include
TARGET_SOURCES := target.cpp ../lists.cpp ../sha256.cpp ../store.cpp
TARGET_LIBS := $(LIBCXXR2)/_crt0.o $(LIBCXXR2)/libc++r2.a
target: jugtest.elf shipped.elf
jugtest.elf: $(TARGET_SOURCES) ../jug.h $(TARGET_LIBS)
	$(CXX) $(TARGET_FLAGS) $(TARGET_SOURCES) $(TARGET_LIBS) -static -no-pie \
        -Wl,--gc-sections,-T,$(LIBCXXR2)/linker.ld -lgcc -o $@
shipped.elf: $(TARGET_SOURCES) ../jug.h $(TARGET_LIBS)
	$(CXX) $(TARGET_FLAGS) -DJUG_SHIPPED_PROBE $(TARGET_SOURCES) $(TARGET_LIBS) -static -no-pie \
        -Wl,--gc-sections,-T,$(LIBCXXR2)/linker.ld -lgcc -o $@
