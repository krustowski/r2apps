NAME 		:= them

BUILD_DIR	?= ./build

CRT0_OBJ	?= ${BUILD_DIR}/_crt0.o
PRG_OBJ		?= ${BUILD_DIR}/${NAME}.o
LIBCR2		?= ../libcr2

CRT0_ASM	?= ${LIBCR2}/_crt0.asm

# The emulator carries a 640x480x32bpp frame in its image -- 1.2 MB -- because
# r2 only accepts syscall pointers into the loaded image, and image plus stack
# share one 2 MiB mapping. Nothing here recurses and every stack frame in the
# program adds up to some 5 KB, so the room comes out of the default stack.
CRT0_FLAGS	?= -DR2_STACK_KB=256

SOURCE_FILES	?= ../*.c

export

ensure_build_dir:
	@mkdir -p ${BUILD_DIR}

build_crt0:
	@nasm -f elf64 ${CRT0_FLAGS} ${CRT0_ASM} -o ${CRT0_OBJ}

compile:
	@cd ${BUILD_DIR} && gcc \
		-c \
		-O2 \
		-flto \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-mno-red-zone \
		-ffreestanding \
		-I .. \
		-I ../../libcr2/ \
		${SOURCE_FILES}

link:
	@gcc \
		-O2 \
		-flto \
		-nostdinc \
		-nostdlib \
		-nostartfiles \
		${BUILD_DIR}/*.o \
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

prg0:
	@nasm -t -f bin -o prg0.bin prg0.asm
