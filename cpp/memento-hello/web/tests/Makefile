#
#  Host tests for the web engine.
#
#      make            # builds host_fetch
#      make check      # the offline checks: URL resolution and HTML layout
#      ./host_fetch https://example.com/
#
WEB := ..
include $(WEB)/bearssl.mk

BDIR := build
CFLAGS   := -O2 -g -Wall $(BEARSSL_DEFS) $(BEARSSL_INC)
CXXFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c++17 -DWEB_HOST \
            -fno-exceptions -fno-rtti $(BEARSSL_INC)

WEB_SRCS := $(WEB)/wbase.cpp $(WEB)/url.cpp $(WEB)/http.cpp $(WEB)/css.cpp $(WEB)/doc.cpp $(WEB)/pixels.cpp $(WEB)/loader.cpp \
            $(WEB)/image.cpp $(WEB)/png.cpp $(WEB)/mp4.cpp
STB      := $(abspath $(WEB)/../../third_party/stb)
#  h264bsd, for MP4s (web/mp4.cpp, web/h264.c), on the host's C library.
H264BSD      := $(abspath $(WEB)/../../third_party/h264bsd/src)
H264BSD_OBJS := $(patsubst $(H264BSD)/%.c,$(BDIR)/h264bsd/%.o,$(wildcard $(H264BSD)/*.c)) $(BDIR)/h264.o
WEB_C    := $(WEB)/tls.c $(WEB)/sysrng_stub.c

BEARSSL_OBJS := $(patsubst $(BEARSSL)/src/%.c,$(BDIR)/bearssl/%.o,$(BEARSSL_SRCS))

.PHONY: all check clean network_check

all: host_fetch unit

$(BDIR)/libbearssl.a: $(BEARSSL_OBJS)
	ar rcs $@ $^

$(BDIR)/bearssl/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -w -c $< -o $@

WEB_C_OBJS := $(patsubst $(WEB)/%.c,$(BDIR)/%.o,$(WEB_C))

$(BDIR)/%.o: $(WEB)/%.c $(WEB)/tls.h
	@mkdir -p $(BDIR)
	gcc $(CFLAGS) -c $< -o $@

host_fetch: host_fetch.cpp $(WEB_SRCS) $(wildcard $(WEB)/*.h) $(WEB)/charmap.inc $(WEB_C_OBJS) $(BDIR)/libbearssl.a $(BDIR)/stb_image.o $(H264BSD_OBJS)
	g++ $(CXXFLAGS) host_fetch.cpp $(WEB_SRCS) $(WEB_C_OBJS) $(BDIR)/stb_image.o $(H264BSD_OBJS) $(BDIR)/libbearssl.a -o $@

#  The offline checks need neither the loader nor TLS.
unit: unit.cpp $(filter-out %/loader.cpp,$(WEB_SRCS)) $(wildcard $(WEB)/*.h) $(WEB)/charmap.inc $(BDIR)/stb_image.o $(H264BSD_OBJS)
	g++ $(CXXFLAGS) unit.cpp $(filter-out %/loader.cpp,$(WEB_SRCS)) $(BDIR)/stb_image.o $(H264BSD_OBJS) -o $@

$(BDIR)/h264bsd/%.o: $(H264BSD)/%.c
	@mkdir -p $(dir $@)
	@gcc -O2 -g -w -I$(H264BSD) -c $< -o $@

$(BDIR)/h264.o: $(WEB)/h264.c $(WEB)/h264.h
	@mkdir -p $(BDIR)
	gcc -O2 -g -Wall -I$(H264BSD) -c $< -o $@

#  stb_image with the same settings as on r2 (web/stb_image.c), on the host's C library.
$(BDIR)/stb_image.o: $(WEB)/stb_image.c $(STB)/stb_image.h
	@mkdir -p $(BDIR)
	gcc -O2 -g -w -I$(STB) -c $< -o $@

#  The trust anchor file writer (tools/mkcacerts.sh runs it).
$(BDIR)/mkcacerts: $(WEB)/tools/mkcacerts.c $(BDIR)/libbearssl.a
	gcc $(CFLAGS) $< $(BDIR)/libbearssl.a -o $@

image_viewer_unit: image_viewer.cpp ../../windows/image_viewer_window.cpp $(WEB)/wbase.cpp $(WEB)/image.cpp $(WEB)/png.cpp $(WEB)/image.h $(WEB)/wbase.h $(WEB)/png.h $(BDIR)/stb_image.o
	g++ $(CXXFLAGS) image_viewer.cpp $(WEB)/wbase.cpp $(WEB)/image.cpp $(WEB)/png.cpp $(BDIR)/stb_image.o -o $@

check: unit net_unit network_unit image_viewer_unit
	./unit
	./net_unit
	./network_unit
	./image_viewer_unit

# The production r2 stack with a host implementation of its kernel calls.
net_unit: net.cpp $(WEB)/net_r2.cpp $(WEB)/net_r2.h $(WEB)/loader.h $(WEB)/http.h $(WEB)/url.h $(WEB)/wbase.h $(WEB)/wbase.cpp $(WEB)/url.cpp $(WEB)/../netmux.h $(wildcard include/r2/*.hpp)
	g++ $(CXXFLAGS) -Iinclude net.cpp $(WEB)/wbase.cpp $(WEB)/url.cpp -o $@

network_unit: ../../tests/network.cpp ../../nsk.cpp ../../nsk.h ../../netmux.cpp ../../netmux.h $(wildcard include/r2/*.hpp)
	g++ $(CXXFLAGS) -Iinclude ../../tests/network.cpp ../../nsk.cpp ../../netmux.cpp -o $@

network_check: net_unit network_unit
	./net_unit
	./network_unit

clean:
	rm -rf $(BDIR) host_fetch unit net_unit network_unit image_viewer_unit
