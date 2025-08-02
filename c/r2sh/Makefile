build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../include \
		-c \
		syscall.c main.c
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-Xlinker "-Tlinker.ld" \
		-I ../include \
		-o r2sh.elf \
		*.o
	@llvm-objcopy -O binary r2sh.elf r2sh.bin

