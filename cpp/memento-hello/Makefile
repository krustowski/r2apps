#
#  memento-hello --- the Memento UI toolkit on r2.
#
#  Built freestanding against libc++r2: no host headers, no libstdc++ and no
#  libcr2 runtime.  The library supplies the C functions the compiler assumes,
#  the maths, operator new, the containers, the heap and the entry point;
#  libc++r2compat.a adds the C allocation names and snprintf on top of the
#  same arena.  libcr2.a is still on the link line for one thing: its TCP/IP
#  stack, which the chat and IRC windows use and which libc++r2 has no
#  equivalent of.  It goes last so that every symbol both libraries define
#  comes from libc++r2.
#
NAME     := memento-hello
.DEFAULT_GOAL := build
MEMENTO  := $(abspath ../../../Memento)
LIBCXXR2 := $(abspath ../libc++r2)
LIBCR2   := $(abspath ../../c/libcr2.a)
LINKER   := $(LIBCXXR2)/linker.ld
#  Our own crt0, for a smaller stack than the library's default: the frame
#  buffer the window manager composes into lives in .bss, and a process has
#  2 MiB for everything.
CRT0_OBJ     = $(BDIR)/_crt0.o
CRT0_SRC    := $(LIBCXXR2)/src/_crt0.asm
STACK_BYTES := 262144
LIBRARY  := $(LIBCXXR2)/libc++r2.a
COMPAT   := $(LIBCXXR2)/libc++r2compat.a
BDIR     := ./obj
FLOPPY   ?= $(abspath ../../fat.img)

#  C++17 rather than the library's default C++23: Memento is a C++17 codebase.
#  libc++r2's C++20/23 headers are empty below C++20 and R2_REQUIRES erases the
#  constraints, so the same archive serves either way.
CXXFLAGS := -std=c++17 -DMEMENTO_BACKEND_R2 -m64 -O2 \
            -ffreestanding -nostdlib -nostdinc -nostdinc++ \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics \
            -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-pie -fcf-protection=none -mno-red-zone -fno-math-errno \
            -ffunction-sections -fdata-sections \
            -I$(LIBCXXR2)/include \
            -I$(MEMENTO)/src

#  Header dependencies, generated as a side effect of compiling and pulled in
#  at the bottom of this file.
#
#  These are not a nicety.  Every translation unit that says `new UIRootImpl`
#  compiles the size of the object into the call, and the implementation's
#  members are all in a header.  Add a member without rebuilding the callers
#  and the allocation is the old size while the constructor writes the new
#  one: the object runs past the end of its heap block and over the next
#  block's boundary tags.  The allocator then merges a live object into the
#  free list and overwrites its first two words with list pointers, and the
#  program dies later, somewhere else, on a null vtable — with nothing at the
#  scene of the crash to say a stale .o was the cause.
CXXFLAGS += -MMD -MP

#  Extra flags for a one-off build, e.g. the serial diagnostics in the r2
#  backend:  make install EXTRA=-DMEMENTO_R2_SERIAL_DEBUG
CXXFLAGS += $(EXTRA)

#
#  The web browser's engine (web/) and BearSSL under it.
#
#  The C side --- BearSSL, the TLS glue and the trust anchors --- is built
#  freestanding with GCC's own <stddef.h>/<stdint.h>/<limits.h> and a small
#  <string.h> of ours, and with -mgeneral-regs-only: this kernel does not
#  promise to keep vector registers across a context switch, and crypto is
#  the last place to find out that it does not.  -Os, because the whole
#  process is 2 MiB and BearSSL is most of what the browser adds to it.
#  web/bearssl.mk says which parts of BearSSL are left out and why.
#
include web/build.mk

LDFLAGS  := -nostdlib -nostartfiles -static -no-pie \
            -Wl,--gc-sections,--allow-multiple-definition \
            -Xlinker "-T$(LINKER)"

P  := $(MEMENTO)/src/ui/platform
R2 := $(MEMENTO)/src/ui/platform/impl/r2

SRCS := main.cpp \
        r2_stubs.cpp \
        netmux.cpp \
        nsk.cpp \
        $(MEMENTO)/src/common/common.cpp \
        $(MEMENTO)/src/ui/platform/impl/UIImpl.cpp \
        $(P)/PlatformUIRoot.cpp \
        $(P)/PlatformWindow.cpp \
        $(P)/PlatformDrawingContext.cpp \
        $(P)/PlatformBitmap.cpp \
        $(P)/PlatformColor.cpp \
        $(P)/PlatformFont.cpp \
        $(P)/PlatformFontManager.cpp \
        $(P)/PlatformKey.cpp \
        $(P)/PlatformEnums.cpp \
        $(P)/PlatformScaler.cpp \
        $(P)/Converts.cpp \
        $(R2)/R2_Validated.cpp \
        $(R2)/R2_ColorImpl.cpp \
        $(R2)/R2_FontImpl.cpp \
        $(R2)/R2_BitmapImpl.cpp \
        $(R2)/R2_DrawingContextImpl.cpp \
        $(R2)/R2_Vga.cpp \
        $(R2)/R2_WindowImpl.cpp \
        $(R2)/R2_UIRootImpl.cpp

OBJS := $(addprefix $(BDIR)/,$(notdir $(SRCS:.cpp=.o)))

#  The windows are #included into main.cpp rather than compiled on their own,
#  so main.o has to be rebuilt when any of them changes; without this, editing
#  a window leaves the previous build in place and looks like a change that
#  did nothing.
WINDOWS := $(wildcard windows/*.cpp)

OBJS += $(WEB_OBJS)

.PHONY: check build install clean webtest nettest notifytest browser jug

# Compile-only check — no linking, so neither archive has to exist yet.
# Without -MMD: with no -o to follow, it writes main.d and the rest into the
# source directory.
check:
	@for src in $(SRCS); do \
		g++ $(filter-out -MMD -MP,$(CXXFLAGS)) -fsyntax-only $$src || exit 1; \
	done
	@echo "All sources compile OK"
	@$(MAKE) -C ../r2web check

build: $(BDIR) $(CRT0_OBJ) $(OBJS) $(BEARSSL_LIB) $(H264BSD_LIB) $(LIBRARY) $(COMPAT) browser jug
	g++ $(LDFLAGS) $(CRT0_OBJ) $(OBJS) $(BEARSSL_LIB) $(H264BSD_LIB) $(LIBRARY) $(COMPAT) $(LIBCR2) -lgcc -o $(NAME).elf
	llvm-objcopy -O binary $(NAME).elf $(NAME).bin
	@python3 ../r2web/check_image.py $(NAME).elf
	@echo "Built $(NAME).elf — install it with: make install"

# The kernel loads 8.3 names from the FAT12 image, so the file goes on in
# upper case and truncated to eight characters: MEMENTO.ELF, run with `fg MEMENTO`.
install: build
	@mcopy -o -i $(FLOPPY) $(NAME).elf ::MEMENTO.ELF
	@mcopy -o -i $(FLOPPY) ../r2web/r2web.elf ::R2WEB.ELF
	@mcopy -o -i $(FLOPPY) ../jug/jug.elf ::JUG.ELF
	@echo "copied $(NAME).elf to $(FLOPPY) as MEMENTO.ELF"

browser:
	@$(MAKE) -C ../r2web build

jug:
	@$(MAKE) -C ../jug build

$(CRT0_OBJ): $(CRT0_SRC) | $(BDIR)
	nasm -f elf64 -DR2_STACK_BYTES=$(STACK_BYTES) $< -o $@

$(LIBRARY) $(COMPAT):
	@$(MAKE) -C $(LIBCXXR2) all

$(BDIR):
	@mkdir -p $(BDIR)

# Per-file compile rules (avoids basename collisions)
$(BDIR)/main.o:                 	main.cpp $(WINDOWS)               		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/r2_stubs.o:             	r2_stubs.cpp                      		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/netmux.o:               	netmux.cpp netmux.h               		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/nsk.o:                  	nsk.cpp nsk.h netmux.h            		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/common.o:               	$(MEMENTO)/src/common/common.cpp  		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/UIImpl.o:               	$(MEMENTO)/src/ui/platform/impl/UIImpl.cpp 	| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformUIRoot.o:       	$(P)/PlatformUIRoot.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformWindow.o:       	$(P)/PlatformWindow.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformDrawingContext.o: 	$(P)/PlatformDrawingContext.cpp 		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformBitmap.o:       	$(P)/PlatformBitmap.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformColor.o:        	$(P)/PlatformColor.cpp        			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformFont.o:         	$(P)/PlatformFont.cpp         			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformFontManager.o:  	$(P)/PlatformFontManager.cpp  			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformKey.o:          	$(P)/PlatformKey.cpp          			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformEnums.o:        	$(P)/PlatformEnums.cpp        			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/PlatformScaler.o:       	$(P)/PlatformScaler.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/Converts.o:             	$(P)/Converts.cpp             			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_Validated.o:         	$(R2)/R2_Validated.cpp        			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_ColorImpl.o:         	$(R2)/R2_ColorImpl.cpp        			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_FontImpl.o:          	$(R2)/R2_FontImpl.cpp         			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_BitmapImpl.o:        	$(R2)/R2_BitmapImpl.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_DrawingContextImpl.o: 	$(R2)/R2_DrawingContextImpl.cpp 		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_Vga.o:               	$(R2)/R2_Vga.cpp              			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_WindowImpl.o:        	$(R2)/R2_WindowImpl.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_UIRootImpl.o:        	$(R2)/R2_UIRootImpl.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@

# Deterministic tests of the network tools, mux and port lifetime.
nettest:
	@$(MAKE) -C web/tests network_check

# Clock bubble expiry, burst ordering and the hosted notification ABI.
notifytest: $(BDIR)/notifications_test
	$(BDIR)/notifications_test

$(BDIR)/notifications_test: tests/notifications.cpp ../r2web/host.h $(R2)/R2_NotificationStack.h | $(BDIR)
	g++ -std=c++17 -Wall -Wextra -Werror -I$(MEMENTO)/src $< -o $@

# Host-side tests of the engine: make -C web/tests check
webtest:
	@$(MAKE) -C $(WEB)/tests check
	@$(MAKE) -C ../r2web test

clean:
	rm -rf $(BDIR) $(NAME).elf $(NAME).bin *.o

#  Written by -MMD above; absent on the first build, which is what the leading
#  dash is for.
-include $(OBJS:.o=.d)
