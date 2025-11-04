NAME := them

build:
	@gcc \
		-m64 \
		-static \
		-n \
		-o2 \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-mno-red-zone \
		-I ../libcr2 \
		-I . \
		-Xlinker "-T../linker.ld" \
		-o ${NAME}.elf \
		../libcr2/*.c *.c
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin
	@nasm -o prg0.bin prg0.asm

dump:
	@objdump -b binary -Mintel,i8086 -m i386 -D prg0.bin

