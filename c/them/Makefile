NAME 		:= them

BUILD_DIR	?= ./build

CRT0_OBJ	?= ${BUILD_DIR}/crt0.o
PRG_OBJ		?= ${BUILD_DIR}/${NAME}.o
LIBCR2		?= ../libcr2

CRT0_ASM	?= ${LIBCR2}/_crt0.asm

SOURCE_FILES	?= ${LIBCR2}/*.c *.c

export

ensure_build_dir:
	@mkdir -p ${BUILD_DIR}

build_crt0:
	@nasm -f elf64 ${CRT0_ASM} -o ${CRT0_OBJ}

#-pie 
#-Xlinker "-T../linker.ld" 

compile:
	@gcc \
		-c \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-mno-red-zone \
		-ffreestanding \
		-I ${LIBCR2} \
		-I . \
		${SOURCE_FILES}

link:
	@ld.lld \
		--nostdlib \
		--no-pie \
		--static \
		--script ./linker.ld \
		-o ${NAME}.elf \
		${CRT0_OBJ} ./*.o 

.PHONY: build
build: ensure_build_dir build_crt0 compile link 

clean:
	@rm -rf ${BUILD_DIR} ./*.{o}

dump:
	@objdump -b binary -Mintel,i8086 -m i386 -D prg0.bin

build_old:
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


