NAME    := memento-hello
MEMENTO := $(abspath ../../../Memento)
LIBCR2  := $(abspath ../../c/libcr2)
LINKER  := $(LIBCR2)/../linker.ld
BDIR   := ./obj

CXXFLAGS := -std=c++17 -DMEMENTO_BACKEND_R2 -m64 -mno-red-zone -O2 \
            -fno-exceptions \
            -ffunction-sections -fdata-sections \
            -I$(MEMENTO)/src

P  := $(MEMENTO)/src/ui/platform
R2 := $(MEMENTO)/src/ui/platform/impl/r2

SRCS := main.cpp \
        r2_stubs.cpp \
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
        $(R2)/R2_WindowImpl.cpp \
        $(R2)/R2_UIRootImpl.cpp

OBJS     := $(addprefix $(BDIR)/,$(notdir $(SRCS:.cpp=.o)))
CRT0_OBJ := $(BDIR)/_crt0.o

.PHONY: check build clean

# Compile-only check — no linking, no libcr2 needed
check:
	g++ -c $(CXXFLAGS) $(SRCS)
	@echo "All sources compile OK"
	@rm -f *.o

# Full build for r2 target
build: $(BDIR) $(CRT0_OBJ) $(OBJS)
	g++ -nostdlib -nostartfiles -static \
	    $(CRT0_OBJ) $(OBJS) \
	    $(LIBCR2)/../libcr2.a \
	    -lstdc++ -lgcc \
	    -Wl,--gc-sections,--allow-multiple-definition \
	    -Xlinker "-T$(LINKER)" \
	    -o $(NAME).elf
	llvm-objcopy -O binary $(NAME).elf $(NAME).bin
	@echo "Built $(NAME).elf  — copy to fat.img with:"
	@echo "  mcopy -i fat.img $(NAME).elf ::$(shell echo $(NAME) | tr a-z A-Z).ELF"

$(BDIR):
	@mkdir -p $(BDIR)

$(CRT0_OBJ): $(LIBCR2)/_crt0.asm | $(BDIR)
	nasm -f elf64 $< -o $@

# Per-file compile rules (avoids basename collisions)
$(BDIR)/main.o:                 	main.cpp                          		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/r2_stubs.o:             	r2_stubs.cpp                      		| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
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
$(BDIR)/R2_WindowImpl.o:        	$(R2)/R2_WindowImpl.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@
$(BDIR)/R2_UIRootImpl.o:        	$(R2)/R2_UIRootImpl.cpp       			| $(BDIR) ; g++ -c $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BDIR) $(NAME).elf $(NAME).bin *.o
