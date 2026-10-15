# Linux harness build for theM.  Compiles the emulator sources unchanged and
# links them against host.c, which stands in for the r2 syscalls.
#
#   make            build ./them-host
#   make run PROG=../vlak.com

NAME      ?= them-host
THEM      ?= ..
LIBCR2    ?= ../../libcr2

SOURCES   := $(wildcard $(THEM)/*.c) $(LIBCR2)/printf.c $(LIBCR2)/mem.c host.c
HEADERS   := $(wildcard $(THEM)/*.h) $(wildcard $(LIBCR2)/*.h)
CFLAGS    ?= -O1 -g -DTHEM_HOST -fno-builtin -Wno-builtin-declaration-mismatch -I$(THEM) -I$(LIBCR2)

$(NAME): $(SOURCES) $(HEADERS)
	@gcc $(CFLAGS) -o $@ $(SOURCES)

run: $(NAME)
	@./$(NAME) $(PROG)

clean:
	@rm -f $(NAME) palette-test window-test pacing-test retrace-test irq-host window-lifetime-test bios-floppy-test window-host *.ppm *.png

.PHONY: run clean

# Assemble as a 286 guest to keep conditional branches within the ISA.
palette-test: $(filter-out $(THEM)/main.c,$(SOURCES)) tests/palette.c $(HEADERS)
	@gcc $(CFLAGS) -o $@ $(filter %.c,$^)

window-test: $(filter-out $(THEM)/main.c,$(SOURCES)) tests/window.c $(HEADERS)
	@gcc $(CFLAGS) -o $@ $(filter %.c,$^)

bios-floppy-test: $(filter-out $(THEM)/main.c,$(SOURCES)) tests/bios-floppy.c $(HEADERS)
	@gcc $(CFLAGS) -o $@ $(filter %.c,$^)

pacing-test: $(filter-out $(THEM)/main.c,$(SOURCES)) tests/pacing.c $(HEADERS)
	@gcc $(CFLAGS) -Wl,--wrap=get_ticks,--wrap=hw_millis,--wrap=winhost_publish \
		-Wl,--wrap=blit_buffer_scaled,--wrap=hw_quit_requested -o $@ $(filter %.c,$^)

retrace-test: $(filter-out $(THEM)/main.c,$(SOURCES)) tests/retrace.c $(HEADERS)
	@gcc $(CFLAGS) -Wl,--wrap=hw_millis -o $@ $(filter %.c,$^)

irq-host: $(SOURCES) tests/irq-clock.c $(HEADERS)
	@gcc $(CFLAGS) -Wl,--wrap=get_ticks -o $@ $(filter %.c,$^)

window-host: $(SOURCES) tests/window-host.c $(HEADERS)
	@gcc $(CFLAGS) -Wl,--wrap=winhost_attach,--wrap=winhost_beat,--wrap=host_dump -o $@ $(filter %.c,$^)

# The kernel image is supplied by its own repository, not vendored here.
rourexs-test: $(NAME) window-host
	@test -n "$(ROUREXS)" || (echo 'Set ROUREXS to the ROUREXS.COM path'; exit 1)
	@python3 tests/rourexs.py ./$(NAME) ./window-host "$(ROUREXS)"

.PHONY: rourexs-test

window-lifetime-test: tests/window-lifetime.cpp $(THEM)/winhost_lifetime.h $(THEM)/winhost.h
	@g++ -std=c++17 -O1 -g -Wall -Wextra -I$(THEM) -o $@ $<

test: $(NAME) palette-test window-test pacing-test retrace-test irq-host window-lifetime-test bios-floppy-test
	@python3 tests/origin.py ./$(NAME)
	@./bios-floppy-test
	@set -eu; test_dir=$$(mktemp -d); trap 'rm -rf "$$test_dir"' EXIT; \
	nasm -f bin tests/bios-floppy.asm -o "$$test_dir/check.com"; \
	test_host=$$(realpath ./$(NAME)); \
	(cd "$$test_dir"; THEM_RUN_MS=1000 "$$test_host" --org 0 check.com >output 2>&1); \
	cat "$$test_dir/output"; \
	rg -q 'BIOS floppy interrupt regression PASS' "$$test_dir/output"
	@set -eu; test_dir=$$(mktemp -d); trap 'rm -rf "$$test_dir"' EXIT; \
	nasm -f bin tests/memory-trap.asm -o "$$test_dir/check.com"; \
	test_host=$$(realpath ./$(NAME)); \
	(cd "$$test_dir"; THEM_RUN_MS=1000 "$$test_host" check.com >output 2>&1); \
	cat "$$test_dir/output"; \
	rg -q 'memory and trap regression PASS' "$$test_dir/output"
	@set -eu; test_dir=$$(mktemp -d); trap 'rm -rf "$$test_dir"' EXIT; \
	nasm -f bin tests/file-parent.asm -o "$$test_dir/parent.com"; \
	nasm -f bin tests/file-child.asm -o "$$test_dir/child.com"; \
	nasm -f bin tests/file-grand.asm -o "$$test_dir/grand.com"; \
	test_host=$$(realpath ./$(NAME)); \
	(cd "$$test_dir"; THEM_RUN_MS=10000 "$$test_host" parent.com >output 2>&1); \
	cat "$$test_dir/output"; \
	rg -q 'file lifetime regression PASS' "$$test_dir/output"; \
	python3 -c 'from pathlib import Path; import sys; p = Path(sys.argv[1]); assert (p / "CHILD.DAT").read_bytes()[0:1] == b"C"; assert (p / "GRAND.DAT").read_bytes()[0:1] == b"G"' "$$test_dir"
	@set -eu; test_dir=$$(mktemp -d); trap 'rm -rf "$$test_dir"' EXIT; \
	nasm -f bin tests/irq-guard.asm -o "$$test_dir/guard.com"; \
	test_host=$$(realpath ./irq-host); \
	for pause in 251 60000 3600000; do \
	  (cd "$$test_dir"; THEM_IRQ_PAUSE_MS=$$pause THEM_RUN_MS=5000 "$$test_host" guard.com >output 2>&1); \
	  cat "$$test_dir/output"; \
	  rg -q 'IRQ guard regression PASS' "$$test_dir/output"; \
	done
	@./palette-test
	@./window-test
	@./pacing-test
	@./pacing-test vesa
	@./pacing-test cpu
	@./retrace-test
	@./window-lifetime-test

.PHONY: test
