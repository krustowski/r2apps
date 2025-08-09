NAME := hellofs

build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../libcr2 \
		-Xlinker "-Tlinker.ld" \
		-o ${NAME}.elf \
		../libcr2/{printf,syscall}.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

