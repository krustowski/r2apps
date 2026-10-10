# Shared freestanding web engine and codec build.
WEB ?= web
include $(WEB)/bearssl.mk
GCC_INC := $(shell gcc -print-file-name=include)
#  stb_image, for pictures (web/image.cpp, web/stb_image.c).
STB := $(abspath ../third_party/stb)
WEB_CFLAGS := -std=gnu99 -m64 -Os -ffreestanding -nostdlib -nostdinc \
              -isystem $(GCC_INC) -I$(WEB)/libc -I$(WEB) \
              -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pie \
              -fcf-protection=none -mno-red-zone -mgeneral-regs-only \
              -ffunction-sections -fdata-sections -w \
              $(BEARSSL_DEFS) $(BEARSSL_INC) -I$(STB)
WEB_CXX_SRCS := $(WEB)/wbase.cpp $(WEB)/url.cpp $(WEB)/http.cpp $(WEB)/css.cpp $(WEB)/doc.cpp $(WEB)/pixels.cpp \
                $(WEB)/loader.cpp $(WEB)/net_r2.cpp $(WEB)/web_r2.cpp $(WEB)/image.cpp $(WEB)/png.cpp \
                $(WEB)/mp4.cpp
WEB_C_SRCS   := $(WEB)/tls.c $(WEB)/sysrng_stub.c $(WEB)/stb_image.c $(WEB)/h264.c
WEB_OBJS     := $(patsubst $(WEB)/%,$(BDIR)/web/%.o,$(basename $(WEB_CXX_SRCS) $(WEB_C_SRCS)))
BEARSSL_LIB  := $(BDIR)/libbearssl.a
BEARSSL_OBJS := $(patsubst $(BEARSSL)/src/%.c,$(BDIR)/bearssl/%.o,$(BEARSSL_SRCS))
#  h264bsd, the H.264 (Baseline) decoder behind the MP4s Telegram makes of
#  GIFs (web/mp4.cpp, web/h264.c).  Integer C only, so -mgeneral-regs-only
#  costs it nothing; its malloc and free are the picture allocator's
#  (web/h264_r2.h).  -Os like the rest: at -O2 it is 22 KiB bigger and no
#  faster to notice, most of a GIF's time going on scaling and dithering.
H264BSD      := $(abspath ../third_party/h264bsd/src)
H264BSD_LIB  := $(BDIR)/libh264bsd.a
H264BSD_OBJS := $(patsubst $(H264BSD)/%.c,$(BDIR)/h264bsd/%.o,$(wildcard $(H264BSD)/*.c))
H264_CFLAGS  := $(WEB_CFLAGS) -I$(H264BSD) -include $(abspath $(WEB)/h264_r2.h)

# The browser engine and BearSSL.  -Os here too: the engine is parsing and
# bookkeeping, nothing that -O2 makes visibly faster, and every kilobyte of
# text comes out of the same 2 MiB as the frame buffer and the heap.
$(BDIR)/web/%.o: $(WEB)/%.cpp | $(BDIR)
	@mkdir -p $(dir $@)
	g++ -c $(CXXFLAGS) -Os -I$(WEB) $< -o $@

$(BDIR)/web/h264.o: $(WEB)/h264.c | $(BDIR)
	@mkdir -p $(dir $@)
	gcc -c $(H264_CFLAGS) -MMD -MP $< -o $@

$(BDIR)/web/%.o: $(WEB)/%.c | $(BDIR)
	@mkdir -p $(dir $@)
	gcc -c $(WEB_CFLAGS) -MMD -MP $< -o $@

$(BDIR)/h264bsd/%.o: $(H264BSD)/%.c $(WEB)/h264_r2.h | $(BDIR)
	@mkdir -p $(dir $@)
	@gcc -c $(H264_CFLAGS) $< -o $@

$(H264BSD_LIB): $(H264BSD_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BDIR)/bearssl/%.o: $(BEARSSL)/src/%.c | $(BDIR)
	@mkdir -p $(dir $@)
	@gcc -c $(WEB_CFLAGS) $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@rm -f $@
	ar rcs $@ $^

