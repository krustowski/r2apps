WEB := ../../memento-hello/web
CXXFLAGS := -std=c++17 -DWEB_HOST -Iinclude -Wall -Wextra -Wno-unused-parameter -g -O1 -fno-exceptions -fno-rtti -pthread
VM := ../obj/mujs.o ../obj/math.o ../obj/format.o ../obj/setjmp.o
.PHONY: check clean vm
check: unit
	./unit
vm:
	$(MAKE) -C .. obj/mujs.o obj/math.o obj/format.o obj/setjmp.o
unit: unit.cpp ../script.cpp ../script.h ../host.h include/setjmp.h ../port/setjmp.h vm $(WEB)/wbase.cpp $(WEB)/doc.cpp $(WEB)/css.cpp $(WEB)/doc.h
	g++ $(CXXFLAGS) -no-pie unit.cpp ../script.cpp $(WEB)/wbase.cpp $(WEB)/doc.cpp $(WEB)/css.cpp $(VM) -lm -o $@
clean:
	rm -f unit
