all:
	@for proj in $$(ls -d */); do \
		cd $${proj} && make build && cd ..; \
	done

.PHONY: libcr2
libcr2:
	@rm -f libcr2.a libcr2.so*
	@nasm -g -DPIC -f elf64 -o ./libcr2/_crt0.o ./libcr2/_crt0.asm
	@gcc \
		-c \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-mno-red-zone \
		-ffreestanding \
		-I ./libcr2/ \
		./libcr2/*.c
	@ar \
		rcs \
		libcr2.a \
		./*.o
