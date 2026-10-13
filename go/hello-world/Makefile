NAME := gohello
MEMENTO_APP := 1

include ../Makefile.tmpl

FLOPPY ?= $(abspath ../../fat.img)

.PHONY: test install
test:
	go test -race . ../libgor2/memento/hosted
	python3 -B ../libgor2/memento/hosted/tools/test_manifest.py

install: build
	mcopy -o -i $(FLOPPY) $(NAME).elf ::GOHELLO.ELF
