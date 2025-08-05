NAME := garn

build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../include \
		-Xlinker "-T../linker.ld" \
		-o ${NAME}.elf \
		../include/*.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

