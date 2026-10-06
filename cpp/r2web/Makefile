NAME := r2web
.DEFAULT_GOAL := build
MEMENTO := $(abspath ../../../Memento)
LIBCXXR2 := $(abspath ../libc++r2)
LIBRARY := $(LIBCXXR2)/libc++r2.a
COMPAT := $(LIBCXXR2)/libc++r2compat.a
BDIR := obj
WEB := ../memento-hello/web
MUJS := ../third_party/mujs
GCC_INC := $(shell gcc -print-file-name=include)
FLOPPY ?= $(abspath ../../fat.img)
CXXFLAGS := -std=c++17 -DMEMENTO_BACKEND_R2 -m64 -Os -ffreestanding -nostdlib -nostdinc -nostdinc++ \
 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-pie -fcf-protection=none -mno-red-zone -fno-math-errno -ffunction-sections -fdata-sections \
 -I$(LIBCXXR2)/include -I$(MEMENTO)/src -Iport -MMD -MP $(EXTRA)
include $(WEB)/build.mk
P := $(MEMENTO)/src/ui/platform
R2 := $(P)/impl/r2
SRCS := main.cpp runtime.cpp script.cpp $(WEB)/../netmux.cpp \
 $(MEMENTO)/src/common/common.cpp \
 $(P)/PlatformUIRoot.cpp $(P)/PlatformWindow.cpp $(P)/PlatformDrawingContext.cpp \
 $(P)/PlatformBitmap.cpp $(P)/PlatformColor.cpp $(P)/PlatformFont.cpp \
 $(P)/PlatformFontManager.cpp $(P)/PlatformKey.cpp $(P)/PlatformEnums.cpp $(P)/PlatformScaler.cpp $(P)/Converts.cpp \
 $(R2)/R2_Validated.cpp $(R2)/R2_ColorImpl.cpp $(R2)/R2_FontImpl.cpp $(R2)/R2_BitmapImpl.cpp \
 $(R2)/R2_DrawingContextImpl.cpp $(R2)/R2_Vga.cpp
OBJS := $(addprefix $(BDIR)/,$(notdir $(SRCS:.cpp=.o))) $(WEB_OBJS) $(BDIR)/mujs.o $(BDIR)/format.o $(BDIR)/math.o $(BDIR)/setjmp.o
JSFLAGS := -std=c99 -m64 -Os -ffreestanding -nostdlib -nostdinc \
 -isystem $(GCC_INC) -Iport -I$(WEB)/libc -I$(MUJS) -I$(STB) \
 -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pie -fcf-protection=none -mno-red-zone \
 -ffunction-sections -fdata-sections -fno-math-errno -MMD -MP
JSDEFS := -DWEB_MUJS_R2 -DWEB_MUJS_LIMITS -DJS_ASTLIMIT=100 -DJS_ENVLIMIT=128 \
 -DJS_STACKSIZE=2048 -DJS_STRLIMIT=1048576 -DJS_ARRAYLIMIT=65536 -DREG_MAXREC=64 -include port/hooks.h
.PHONY: all build check clean install test
all: build
$(BDIR):
	@mkdir -p $@
build: $(BDIR)/_crt0.o $(OBJS) $(BEARSSL_LIB) $(H264BSD_LIB) $(LIBRARY) $(COMPAT)
	g++ -nostdlib -nostartfiles -static -no-pie -Wl,--gc-sections -Wl,-T,$(LIBCXXR2)/linker.ld $^ -lgcc -o $(NAME).elf
	@python3 check_image.py $(NAME).elf
	llvm-objcopy -O binary $(NAME).elf $(NAME).bin
$(BDIR)/_crt0.o: $(LIBCXXR2)/src/_crt0.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 -DR2_STACK_BYTES=262144 $< -o $@
define CXX_RULE
$(BDIR)/$(notdir $(1:.cpp=.o)): $(1)
	@mkdir -p $$(dir $$@)
	g++ -c $$(CXXFLAGS) $$< -o $$@
endef
$(foreach src,$(SRCS),$(eval $(call CXX_RULE,$(src))))
$(BDIR)/mujs.o: $(MUJS)/one.c $(wildcard $(MUJS)/*.[ch]) $(wildcard port/*.h)
	@mkdir -p $(dir $@)
	gcc $(JSFLAGS) $(JSDEFS) -c $< -o $@
$(BDIR)/format.o: port/format.c $(STB)/stb_sprintf.h
	@mkdir -p $(dir $@)
	gcc $(JSFLAGS) -c $< -o $@
$(BDIR)/math.o: port/math.c
	@mkdir -p $(dir $@)
	gcc $(JSFLAGS) -c $< -o $@
$(BDIR)/setjmp.o: port/setjmp.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@
$(LIBRARY) $(COMPAT):
	$(MAKE) -C $(LIBCXXR2) all
check:
	@for src in $(SRCS) $(WEB_CXX_SRCS); do g++ $(filter-out -MMD -MP,$(CXXFLAGS)) -fsyntax-only $$src || exit 1; done
install: build
	mcopy -o -i $(FLOPPY) $(NAME).elf ::R2WEB.ELF
test:
	$(MAKE) -C tests check
clean:
	rm -rf $(BDIR) $(NAME).elf $(NAME).bin
-include $(OBJS:.o=.d) $(BDIR)/mujs.d $(BDIR)/format.d $(BDIR)/math.d
