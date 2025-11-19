NAME := hellofs

build:
	@gcc \
		-c \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-I ../libcr2 \
		../libcr2/{printf,syscall}.c *.c
	@nasm -f elf64 -o ../libcr2/_crt0.o ../libcr2/_crt0.asm 
	@ld.lld \
		--nostdlib \
		--no-pie \
		--static \
		--script ./linker.ld \
		-o ${NAME}.elf \
		../libcr2/_crt0.o ./*.o 
	@llvm-objcopy \
		-O binary \
		${NAME}.elf \
		${NAME}.bin

