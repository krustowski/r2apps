NAME := streamd

include ../Makefile.tmpl

.PHONY: test
test: ensure_build_dir
	gcc -O2 -ffreestanding -ffunction-sections -fdata-sections -I../libcr2 \
		tests/tcp_send_test.c -Wl,--gc-sections -o build/tcp_send_test
	./build/tcp_send_test


.PHONY: fpu-test
fpu-test:
	python3 tests/fpu/run_host.py


.PHONY: capture-test
capture-test: ensure_build_dir
	rustc --test --edition 2021 ../../../r2_main/src/video/capture.rs -o build/capture_test
	./build/capture_test
