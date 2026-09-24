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

WEB_SRCS := $(WEB)/wbase.cpp $(WEB)/url.cpp $(WEB)/http.cpp $(WEB)/css.cpp $(WEB)/doc.cpp $(WEB)/loader.cpp
WEB_C    := $(WEB)/tls.c $(WEB)/sysrng_stub.c

BEARSSL_OBJS := $(patsubst $(BEARSSL)/src/%.c,$(BDIR)/bearssl/%.o,$(BEARSSL_SRCS))

.PHONY: all check clean

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

host_fetch: host_fetch.cpp $(WEB_SRCS) $(wildcard $(WEB)/*.h) $(WEB)/charmap.inc $(WEB_C_OBJS) $(BDIR)/libbearssl.a
	g++ $(CXXFLAGS) host_fetch.cpp $(WEB_SRCS) $(WEB_C_OBJS) $(BDIR)/libbearssl.a -o $@

#  The offline checks need neither the loader nor TLS.
unit: unit.cpp $(filter-out %/loader.cpp,$(WEB_SRCS)) $(wildcard $(WEB)/*.h) $(WEB)/charmap.inc
	g++ $(CXXFLAGS) unit.cpp $(filter-out %/loader.cpp,$(WEB_SRCS)) -o $@

#  The trust anchor file writer (tools/mkcacerts.sh runs it).
$(BDIR)/mkcacerts: $(WEB)/tools/mkcacerts.c $(BDIR)/libbearssl.a
	gcc $(CFLAGS) $< $(BDIR)/libbearssl.a -o $@

check: unit
	./unit

clean:
	rm -rf $(BDIR) host_fetch unit
