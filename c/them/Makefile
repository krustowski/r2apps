NAME 		:= them

BUILD_DIR	?= ./build

CRT0_OBJ	?= ${BUILD_DIR}/_crt0.o
PRG_OBJ		?= ${BUILD_DIR}/${NAME}.o
LIBCR2		?= ../libcr2

CRT0_ASM	?= ${LIBCR2}/_crt0.asm

SOURCE_FILES	?= *.c

export

ensure_build_dir:
	@mkdir -p ${BUILD_DIR}

build_crt0:
	@nasm -f elf64 ${CRT0_ASM} -o ${CRT0_OBJ}

compile:
	@gcc \
		-c \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-mno-red-zone \
		-ffreestanding \
		-I . \
		-I ../libcr2/ \
		${SOURCE_FILES}

link:
	@gcc \
		-nostdinc \
		-nostdlib \
		-nostartfiles \
		${CRT0_OBJ} ./*.o \
		-L .. \
		-lcr2 \
		-static \
		-Xlinker "-T../linker.ld" \
		-Wl,-v \
		-o ${NAME}.elf \

.PHONY: build
build: ensure_build_dir build_crt0 compile link 

clean:
	@rm -rf ${BUILD_DIR} ./*.o

dump:
	@objdump -b binary -Mintel,i8086 -m i386 -D prg0.bin

