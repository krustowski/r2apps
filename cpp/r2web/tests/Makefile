WEB := ../../memento-hello/web
LIBJSR2 := ../../libjsr2
QJS := ../../third_party/quickjs
CXXFLAGS := -std=c++17 -DWEB_HOST -I$(LIBJSR2)/include -isystem $(QJS) -Wall -Wextra -Wno-unused-parameter -g -O1 \
 -fno-exceptions -fno-rtti -pthread
SRCS := unit.cpp ../script.cpp ../htmlparse.cpp $(WEB)/wbase.cpp $(WEB)/doc.cpp $(WEB)/pixels.cpp $(WEB)/css.cpp
.PHONY: check clean jsr2
check: unit browser_menu
	./unit
	./browser_menu
#  The host build of libjsr2, and dom.js as bytecode (the r2 build makes it).
jsr2:
	$(MAKE) -C $(LIBJSR2) host obj/host/jsr2c
	$(MAKE) -C .. obj/gen/dom.c
unit: jsr2 $(SRCS) ../script.h ../htmlparse.h ../host.h ../js/dom.js $(WEB)/doc.h
	g++ $(CXXFLAGS) -no-pie $(SRCS) ../obj/gen/dom.c $(LIBJSR2)/obj/host/libjsr2.a -lm -lpthread -o $@
# The real browser window with a host window/bitmap and no network or drawing.
MEMENTO := $(abspath ../../../../Memento)
browser_menu: jsr2 browser_menu.cpp ../browser.cpp ../script.h include/ui/platform/impl/r2/R2_BitmapImpl.h $(SRCS)
	g++ $(CXXFLAGS) -Wno-unused-function -Iinclude -I$(MEMENTO)/src -ffunction-sections -fdata-sections -no-pie \
	 browser_menu.cpp $(filter-out unit.cpp,$(SRCS)) $(WEB)/url.cpp ../obj/gen/dom.c \
	 $(LIBJSR2)/obj/host/libjsr2.a -Wl,--gc-sections -lm -lpthread -o $@
clean:
	rm -f unit browser_menu
