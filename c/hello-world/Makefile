NAME := hello

build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../libcr2 \
		-Xlinker "-T../linker.ld" \
		-Wall \
		-o ${NAME}.elf \
		../libcr2/{print,syscall}.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

