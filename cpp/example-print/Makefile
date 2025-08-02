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
		syscall.cpp main.cpp
	@gcc \
		-m64 \
		-static \
		-n \
		-nostdinc \
		-nostdlib \
		-ffreestanding \
		-Xlinker "-Tlinker.ld" \
		-I ../include \
		-o print.elf \
		*.o
	@llvm-objcopy -O binary print.elf print.bin 

dump:
	@llvm-objdump -d a.out
