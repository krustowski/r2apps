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
		-o r2sh.elf \
		../libcr2/{printf,syscall}.c *.c
	@llvm-objcopy \
		-O binary \
		r2sh.elf \
		r2sh.bin

