build:
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../include \
		-Xlinker "-Tlinker.ld" \
		-o r2sh.elf \
		*.c
	@llvm-objcopy \
		-O binary \
		r2sh.elf \
		r2sh.bin

