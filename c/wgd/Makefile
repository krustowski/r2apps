CC ?= gcc
HOST_CC ?= cc
VENDOR := vendor/wireguard
CRYPTO := $(VENDOR)/crypto/refc
JITTER := vendor/jitterentropy
JITTER_SRC := $(filter-out $(JITTER)/src/jitterentropy-status.c,$(wildcard $(JITTER)/src/*.c)) $(wildcard $(JITTER)/arch/*.c)
JITTER_CC_INCLUDE := $(shell $(CC) -print-file-name=include)
CORE := config.c engine.c $(VENDOR)/wireguard.c $(VENDOR)/crypto.c \
        $(CRYPTO)/blake2s.c $(CRYPTO)/chacha20.c $(CRYPTO)/chacha20poly1305.c \
        $(CRYPTO)/poly1305-donna.c $(CRYPTO)/x25519.c
SOURCES := main.c platform_r2.c entropy.c string.c $(CORE) $(JITTER_SRC)
OBJECTS := $(patsubst %.c,build/%.o,$(SOURCES))
CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wno-sign-compare -Wno-unused-function \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -mno-red-zone \
          -ffunction-sections -fdata-sections -nostdinc -Icompat -I$(VENDOR) -I$(JITTER) -I$(JITTER)/src -I../libcr2
HEADERS := $(wildcard *.h compat/*.h compat/sys/*.h $(VENDOR)/*.h $(CRYPTO)/*.h $(JITTER)/*.h $(JITTER)/src/*.h $(JITTER)/arch/*.h)
ENTROPY_TEST_FLAGS := -O0 -g -fwrapv -ffreestanding -fno-builtin -fno-lto -std=c11 \
                      -DWGD_ENTROPY_TEST -DJENT_CONF_ENABLE_MOCK_TIMER -I. -I$(VENDOR) -I$(JITTER) -I$(JITTER)/src
ENTROPY_TEST_SRC := tests/entropy.c entropy.c $(JITTER_SRC)

.PHONY: build clean test entropy-test entropy-sanitizer-test sanitizer-test kernel-test check hosted-test-driver
build: wgd.elf
../libcr2.a: FORCE
	$(MAKE) -C .. libcr2
.PHONY: FORCE
FORCE:
build/%.o: %.c $(HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
# The timed entropy core must retain its exact unoptimized execution loops.
# Keep it out of LTO as well, including when the rest of the daemon is optimized.
build/$(JITTER)/%.o: $(JITTER)/%.c $(HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(filter-out -O% -flto%,$(CFLAGS)) -isystem $(JITTER_CC_INCLUDE) -O0 -fno-lto -fwrapv -c $< -o $@
build/_crt0.o: ../libcr2/_crt0.asm
	@mkdir -p build
	nasm -f elf64 -DR2_STACK_KB=128 $< -o $@
wgd.elf: $(OBJECTS) build/_crt0.o ../libcr2.a ../linker.ld
	$(CC) -nostdlib -nostartfiles -static -no-pie -Wl,--gc-sections,--build-id=none,-T,../linker.ld \
		$(OBJECTS) build/_crt0.o ../libcr2.a -o $@
	@size $@
test: build/unit
	./build/unit
build/unit: tests/unit.c $(CORE) $(HEADERS)
	@mkdir -p build
	$(HOST_CC) -O1 -g -std=c11 -Wall -Wextra -Wno-sign-compare -Wno-unused-function \
		-I. -I$(VENDOR) tests/unit.c $(CORE) -o $@
sanitizer-test: entropy-sanitizer-test
	@mkdir -p build
	clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-I. -I$(VENDOR) tests/unit.c $(CORE) -o build/unit-sanitize
	./build/unit-sanitize
kernel-test:
	@mkdir -p build
	rustc --edition 2021 --test kernel/tunnel.rs -o build/tunnel_test
	./build/tunnel_test
check: test kernel-test entropy-test build
build/entropy-test: $(ENTROPY_TEST_SRC) $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(ENTROPY_TEST_FLAGS) $(ENTROPY_TEST_SRC) -o $@
entropy-test: build/entropy-test
	./build/entropy-test
	./build/entropy-test --stalled
	./build/entropy-test --failed-after-start
entropy-sanitizer-test:
	@mkdir -p build
	clang $(ENTROPY_TEST_FLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		$(ENTROPY_TEST_SRC) -o build/entropy-sanitize
	./build/entropy-sanitize
	./build/entropy-sanitize --stalled
	./build/entropy-sanitize --failed-after-start
hosted-test-driver: build/hosted.elf
build/hosted.elf: tests/hosted_shell.c ../r2sh/host.h ../libcr2.a ../linker.ld
	@mkdir -p build
	$(CC) -O2 -ffreestanding -fno-pie -mno-red-zone -nostdinc -I../libcr2 -c tests/hosted_shell.c -o build/hosted.o
	nasm -f elf64 -DR2_STACK_KB=128 ../libcr2/_crt0.asm -o build/hosted-crt0.o
	$(CC) -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none,-T,../linker.ld build/hosted.o build/hosted-crt0.o ../libcr2.a -o $@
clean:
	rm -rf build wgd.elf
