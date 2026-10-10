NAME := r2web
.DEFAULT_GOAL := build
MEMENTO := $(abspath ../../../Memento)
LIBCXXR2 := $(abspath ../libc++r2)
LIBRARY := $(LIBCXXR2)/libc++r2.a
COMPAT := $(LIBCXXR2)/libc++r2compat.a
BDIR := obj
WEB := ../memento-hello/web
#  JavaScript: libjsr2 (QuickJS, the event loop, the Web APIs) and its
#  compiler, which turns js/dom.js into bytecode at build time.
LIBJSR2 := $(abspath ../libjsr2)
QJS := $(abspath ../third_party/quickjs)
JSR2C := $(LIBJSR2)/obj/host/jsr2c
GCC_INC := $(shell gcc -print-file-name=include)
FLOPPY ?= $(abspath ../../fat.img)
CXXFLAGS := -std=c++17 -DMEMENTO_BACKEND_R2 -m64 -Os -ffreestanding -nostdlib -nostdinc -nostdinc++ \
 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-pie -fcf-protection=none -mno-red-zone -fno-math-errno -ffunction-sections -fdata-sections \
 -I$(LIBCXXR2)/include -I$(MEMENTO)/src -I$(LIBJSR2)/include -I$(LIBJSR2)/port -isystem $(QJS) \
 -isystem $(GCC_INC) -MMD -MP $(EXTRA)
include $(WEB)/build.mk
P := $(MEMENTO)/src/ui/platform
R2 := $(P)/impl/r2
SRCS := main.cpp runtime.cpp script.cpp htmlparse.cpp $(WEB)/../netmux.cpp \
 $(MEMENTO)/src/common/common.cpp \
 $(P)/PlatformUIRoot.cpp $(P)/PlatformWindow.cpp $(P)/PlatformDrawingContext.cpp \
 $(P)/PlatformBitmap.cpp $(P)/PlatformColor.cpp $(P)/PlatformFont.cpp \
 $(P)/PlatformFontManager.cpp $(P)/PlatformKey.cpp $(P)/PlatformEnums.cpp $(P)/PlatformScaler.cpp $(P)/Converts.cpp \
 $(R2)/R2_Validated.cpp $(R2)/R2_ColorImpl.cpp $(R2)/R2_FontImpl.cpp $(R2)/R2_BitmapImpl.cpp \
 $(R2)/R2_DrawingContextImpl.cpp $(R2)/R2_Vga.cpp
OBJS := $(addprefix $(BDIR)/,$(notdir $(SRCS:.cpp=.o))) $(WEB_OBJS) $(BDIR)/dom.o
.PHONY: all build check clean install test jsr2
all: build
$(BDIR):
	@mkdir -p $@
build: jsr2 $(BDIR)/_crt0.o $(OBJS) $(BEARSSL_LIB) $(H264BSD_LIB) $(LIBRARY) $(COMPAT)
	g++ -nostdlib -nostartfiles -static -no-pie -Wl,--gc-sections -Wl,-T,$(LIBCXXR2)/linker.ld \
	 $(BDIR)/_crt0.o $(OBJS) $(BEARSSL_LIB) $(H264BSD_LIB) $(LIBJSR2)/libjsr2.a $(LIBRARY) $(COMPAT) -lgcc -o $(NAME).elf
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
#  libjsr2 is a make of its own; it decides what is out of date there.
jsr2:
	$(MAKE) -C $(LIBJSR2) all
$(JSR2C) $(LIBJSR2)/libjsr2.a: jsr2
$(BDIR)/gen/dom.c: js/dom.js | jsr2
	@mkdir -p $(dir $@)
	$(JSR2C) $< $@ r2web_dom
$(BDIR)/dom.o: $(BDIR)/gen/dom.c
	gcc -c -m64 -Os -ffreestanding -nostdinc -isystem $(GCC_INC) -fno-pie -fdata-sections $< -o $@
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
-include $(OBJS:.o=.d)
