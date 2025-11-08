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

compile:
	@gcc \
		-c \
		-n \
		-mno-red-zone \
		-pie \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-ffreestanding \
		-Xlinker "-T../linker.ld" \
		-I ${LIBCR2} \
		-I . \
		${SOURCE_FILES}

link:
	@gcc \
		-nostartfiles \
		-m64 \
		-mno-red-zone \
		-pie \
		-static \
		-nostdlib \
		-nostdinc \
		-ffreestanding \
		${CRT0_OBJ} ./*o \
		-o ${NAME}.elf

build_untested: ensure_build_dir build_crt0 compile link 

clean:
	@rm -rf ${BUILD_DIR} ./*.{elf,o}

.PHONY: build
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

