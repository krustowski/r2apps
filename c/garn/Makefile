NAME := garn

build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-mno-red-zone \
		-I ../libcr2 \
		-Xlinker "-T../linker.ld" \
		-o ${NAME}.elf \
		../libcr2/*.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

