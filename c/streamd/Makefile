NAME := streamd
OPT_LEVEL := -O3
JPEG_QUALITY ?= 2
JPEG_SUBSAMPLING ?= 444
EXTRA_CFLAGS += -DJPEG_QUALITY=$(JPEG_QUALITY) -DTJE_SUBSAMPLING=$(JPEG_SUBSAMPLING)

include ../Makefile.tmpl

.PHONY: build_libcr2
build_libcr2:
	$(MAKE) -C .. libcr2

link: build_libcr2

.PHONY: test
test: ensure_build_dir
	gcc -O2 -ffreestanding -ffunction-sections -fdata-sections -I../libcr2 \
		tests/tcp_send_test.c -Wl,--gc-sections -o build/tcp_send_test
	./build/tcp_send_test
	gcc -O2 -ffreestanding -ffunction-sections -fdata-sections \
		-Dmemcpy=r2_test_memcpy -Dmemcmp=r2_test_memcmp -I../libcr2 \
		tests/net_stack_test.c ../libcr2/mem.c -Wl,--gc-sections -o build/net_stack_test
	./build/net_stack_test
	gcc -O3 -DTJE_SUBSAMPLING=420 -ffreestanding -ffunction-sections -fdata-sections -I../libcr2 \
		tests/tcp_send_test.c -Wl,--gc-sections -o build/tcp_send_test_420
	./build/tcp_send_test_420


.PHONY: fpu-test
fpu-test:
	python3 tests/fpu/run_host.py


.PHONY: capture-test
capture-test: ensure_build_dir
	rustc --test --edition 2021 ../../../r2_main/src/video/capture.rs -o build/capture_test
	./build/capture_test

.PHONY: jpeg-test
jpeg-test:
	python3 tests/jpeg_decode_test.py

.PHONY: bench
bench: ensure_build_dir
	@mkdir -p build/bench
	@gcc -O2 -ffreestanding -Dmemcpy=r2_test_memcpy -Dmemcmp=r2_test_memcmp \
		-I../libcr2 -c ../libcr2/mem.c -o build/bench/mem.o
	@for sampling in 444 420; do \
		gcc -O3 -ffreestanding -DTJE_SUBSAMPLING=$$sampling \
			-Dmemcpy=r2_test_memcpy -Dmemcmp=r2_test_memcmp -I. -I../libcr2 \
			tests/jpeg_bench.c build/bench/mem.o -o build/jpeg_bench_$$sampling && \
		printf 'JPEG sampling %s\n' $$sampling && ./build/jpeg_bench_$$sampling || exit 1; \
	done

.PHONY: r2-capture-test
r2-capture-test: ensure_build_dir build_libcr2
	mkdir -p build/capture
	nasm -f elf64 -DR2_STACK_KB=128 ../libcr2/_crt0.asm -o build/capture/_crt0.o
	gcc -O2 -ffreestanding -mno-red-zone -nostdlib -nostdinc -I../libcr2 \
		tests/capture/r2_capture_test.c build/capture/_crt0.o ../libcr2.a \
		-static -Wl,-T,../linker.ld -o build/r2_capture_test.elf
