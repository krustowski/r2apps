BUILD := build
GCC_INC := $(shell gcc -print-file-name=include)
WEB_LIBC := $(abspath ../../../cpp/memento-hello/web/libc)
SRC := mdct block window synthesis info floor1 floor0 res012 mapping0 registry codebook sharedbook
OBJS := $(addprefix $(BUILD)/tremor/,$(addsuffix .o,$(SRC))) $(BUILD)/ogg/framing.o $(BUILD)/ogg/bitwise.o $(BUILD)/decoder.o $(BUILD)/sort.o
export CCACHE_DISABLE := 1
FLAGS := -m64 -Os -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INC) -Ilibc -I$(WEB_LIBC) -Ivendor/ogg/include -Ivendor/tremor -fno-pie -mno-red-zone -mgeneral-regs-only -fno-stack-protector -ffunction-sections -fdata-sections -include libc/stdlib.h -DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321
.PHONY: all test clean
all: $(BUILD)/libvorbisr2.a
$(BUILD)/tremor/%.o: vendor/tremor/%.c libc/stdlib.h
	@mkdir -p $(dir $@)
	$(CC) $(FLAGS) -c $< -o $@
$(BUILD)/ogg/%.o: vendor/ogg/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(FLAGS) -c $< -o $@
$(BUILD)/sort.o: native/sort.c
	@mkdir -p $(BUILD)
	$(CC) $(FLAGS) -c $< -o $@
$(BUILD)/decoder.o: native/decoder.c decoder.h
	@mkdir -p $(BUILD)
	$(CC) $(FLAGS) -c $< -o $@
$(BUILD)/libvorbisr2.a: $(OBJS)
	$(AR) rcs $@ $^
clean:
	rm -rf $(BUILD)

test: $(BUILD)/libvorbisr2.a
	$(CC) -pthread -Ivendor/ogg/include native/tests/codebook.c $(BUILD)/libvorbisr2.a -o $(BUILD)/codebook-test
	$(BUILD)/codebook-test
