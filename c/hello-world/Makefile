NAME := hello

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
		-Wall \
		-o ${NAME}.elf \
		../include/{print,syscall}.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

