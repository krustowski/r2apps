#
#  jug --- programs for r2, fresh from a server.
#
#    make            jug.elf (and jug.bin)
#    make check      the list, registry, SHA-256 and ELF code, on the host
#    make install    JUG.ELF onto the floppy image
#    make sums DIR=  sums.txt for a directory of programs (see README.md)
#
#  Built like r2web, from the same pieces: libc++r2, the web engine's network,
#  HTTP and TLS (../memento-hello/web, BearSSL under it) for the downloads,
#  and Memento's software renderer for the window jug draws when Memento's
#  Jug window hosts it.  C++17, as Memento is.
#
NAME := jug
.DEFAULT_GOAL := build
MEMENTO  := $(abspath ../../../Memento)
LIBCXXR2 := $(abspath ../libc++r2)
LIBRARY  := $(LIBCXXR2)/libc++r2.a
COMPAT   := $(LIBCXXR2)/libc++r2compat.a
BDIR     := obj
WEB      := ../memento-hello/web
FLOPPY   ?= $(abspath ../../fat.img)

CXXFLAGS := -std=c++17 -DMEMENTO_BACKEND_R2 -m64 -Os -ffreestanding -nostdlib -nostdinc -nostdinc++ \
 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-pie -fcf-protection=none -mno-red-zone -fno-math-errno -ffunction-sections -fdata-sections \
 -I$(LIBCXXR2)/include -I$(MEMENTO)/src -I$(WEB) -MMD -MP $(EXTRA)
#  Warnings for jug's own sources; the engine and Memento build as they do
#  for r2web.
JUG_WARN := -Wall -Wextra
#  hosted.cpp takes in Memento's headers, which warn about themselves.
$(BDIR)/hosted.o: JUG_WARN += -Wno-deprecated-copy -Wno-unused-function

#  BearSSL, the TLS glue, and their compile rules (WEB_CFLAGS, the pattern
#  rules for $(BDIR)/web/ and $(BDIR)/bearssl/).
include $(WEB)/build.mk

JUG_SRCS := main.cpp cli.cpp hosted.cpp fetch.cpp store.cpp lists.cpp sha256.cpp
P  := $(MEMENTO)/src/ui/platform
R2 := $(P)/impl/r2
UI_SRCS := $(MEMENTO)/src/common/common.cpp ../memento-hello/netmux.cpp \
 $(P)/PlatformUIRoot.cpp $(P)/PlatformWindow.cpp $(P)/PlatformDrawingContext.cpp \
 $(P)/PlatformBitmap.cpp $(P)/PlatformColor.cpp $(P)/PlatformFont.cpp \
 $(P)/PlatformFontManager.cpp $(P)/PlatformKey.cpp $(P)/PlatformEnums.cpp $(P)/PlatformScaler.cpp $(P)/Converts.cpp \
 $(R2)/R2_Validated.cpp $(R2)/R2_ColorImpl.cpp $(R2)/R2_FontImpl.cpp $(R2)/R2_BitmapImpl.cpp \
 $(R2)/R2_DrawingContextImpl.cpp $(R2)/R2_Vga.cpp
#  The engine's network half only: no HTML, CSS, pictures or video.
NET_OBJS := $(addprefix $(BDIR)/web/,wbase.o url.o http.o loader.o net_r2.o web_r2.o tls.o sysrng_stub.o)

OBJS := $(addprefix $(BDIR)/,$(JUG_SRCS:.cpp=.o)) \
        $(addprefix $(BDIR)/ui/,$(notdir $(UI_SRCS:.cpp=.o))) $(NET_OBJS)

.PHONY: all build check clean install sums
all: build

$(BDIR):
	@mkdir -p $@

build: $(NAME).elf

$(NAME).elf: $(BDIR)/_crt0.o $(OBJS) $(BEARSSL_LIB) $(LIBRARY) $(COMPAT)
	g++ -nostdlib -nostartfiles -static -no-pie -Wl,--gc-sections -Wl,-T,$(LIBCXXR2)/linker.ld $^ -lgcc -o $@
	@python3 ../r2web/check_image.py $@
	llvm-objcopy -O binary $@ $(NAME).bin

#  A 256 KiB stack, as r2web has: the 2 MiB image is code, stack and nothing
#  else; the heap is on the kernel's user heap (main.cpp).
$(BDIR)/_crt0.o: $(LIBCXXR2)/src/_crt0.asm | $(BDIR)
	nasm -f elf64 -DR2_STACK_BYTES=262144 $< -o $@

$(BDIR)/%.o: %.cpp | $(BDIR)
	g++ -c $(CXXFLAGS) $(JUG_WARN) $< -o $@

define UI_RULE
$(BDIR)/ui/$(notdir $(1:.cpp=.o)): $(1)
	@mkdir -p $$(dir $$@)
	g++ -c $$(CXXFLAGS) $$< -o $$@
endef
$(foreach src,$(UI_SRCS),$(eval $(call UI_RULE,$(src))))

$(LIBRARY) $(COMPAT):
	$(MAKE) -C $(LIBCXXR2) all

check:
	@$(MAKE) -C tests run

install: build
	mcopy -o -i $(FLOPPY) $(NAME).elf ::JUG.ELF

#  sums.txt for the programs in DIR, which is what a server publishes beside
#  them:  make sums DIR=../../../r2_main/iso/bin
sums:
	@test -n "$(DIR)" || (echo "make sums DIR=<directory of .elf programs>"; exit 1)
	python3 mksums.py "$(DIR)" --output "$(DIR)/sums.txt"
	@echo "wrote $(DIR)/sums.txt"

clean:
	rm -rf $(BDIR) $(NAME).elf $(NAME).bin
	@$(MAKE) -C tests clean

-include $(OBJS:.o=.d)
