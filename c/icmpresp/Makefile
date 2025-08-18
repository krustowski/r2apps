NAME := icmpresp

build:
	@gcc \
		-Wall \
		-m64 \
		-static \
		-n \
		-O \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../libcr2 \
		-Xlinker "-T../linker.ld" \
		-o ${NAME}.elf \
		../libcr2/{mem,net,printf,syscall}.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

