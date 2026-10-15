NAME := mines
MEMENTO_APP := 1

include ../Makefile.tmpl

FLOPPY ?= $(abspath ../../fat.img)

.PHONY: test install
test:
	go test -race .

install: build
	mcopy -o -i $(FLOPPY) $(NAME).elf ::MINES.ELF
