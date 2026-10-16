NAME := mdnsd
.DEFAULT_GOAL := build
# Byte loops stay loops: libcr2 has no memset for gcc to call instead.
EXTRA_CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns

include ../Makefile.tmpl

.PHONY: build_libcr2
build_libcr2:
	$(MAKE) -C .. libcr2

link: build_libcr2

.PHONY: test sanitizer-test
test: ensure_build_dir
	gcc -O2 -g -Wall -Wextra -I../libcr2 tests/mdns_test.c mdns.c -o build/mdns_test
	./build/mdns_test
	gcc -O2 -g -Wall -Wextra -I../libcr2 tests/browse_test.c browse.c mdns.c -o build/browse_test
	./build/browse_test

sanitizer-test: ensure_build_dir
	clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-I../libcr2 tests/mdns_test.c mdns.c -o build/mdns_test_sanitize
	./build/mdns_test_sanitize
	clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-I../libcr2 tests/browse_test.c browse.c mdns.c -o build/browse_test_sanitize
	./build/browse_test_sanitize
