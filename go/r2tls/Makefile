# Portable BearSSL objects; the Go application supplies the socket and clock.
WEB := $(abspath ../../cpp/memento-hello/web)
include $(WEB)/bearssl.mk
BUILD := build
GCC_INC := $(shell gcc -print-file-name=include)
CFLAGS := -m64 -Os -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INC) \
 -I$(WEB)/libc $(BEARSSL_INC) $(BEARSSL_DEFS) -fno-pie -mno-red-zone \
 -mgeneral-regs-only -fno-stack-protector -ffunction-sections -fdata-sections
OBJS := $(patsubst $(BEARSSL)/src/%.c,$(BUILD)/%.o,$(BEARSSL_SRCS)) $(BUILD)/tls.o $(BUILD)/sysrng_stub.o $(BUILD)/modexp.o
.PHONY: all clean
all: $(BUILD)/libbearssl.a
$(BUILD)/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/modexp.o: native/modexp.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/tls.o: $(WEB)/tls.c $(WEB)/tls.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sysrng_stub.o: $(WEB)/sysrng_stub.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/libbearssl.a: $(OBJS)
	$(AR) rcs $@ $^
clean:
	rm -rf $(BUILD)
