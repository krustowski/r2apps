#
#  mpegplay --- an MPEG-1 video player for r2, shown in memento's Video window.
#
#  pl_mpeg_r2.h is made from third_party/pl_mpeg.h by tools/trim_pl_mpeg.py
#  (video only, no floating point); `make header` makes it again after the
#  upstream file has been replaced.
#
#  -mgeneral-regs-only: r2 does not keep a task's SSE or x87 registers across
#  a task switch, so nothing in this program may use them.  `make check-fp`
#  looks for any that came in anyway, from the library.
#
NAME     := mpegplay
LIBCXXR2 := $(abspath ../libc++r2)
SOURCES  := main.cpp
#  The parts of libc++r2 the player calls, built again here with the flag
#  below: the archive's copies are ordinary -O2 and GCC copies and zeroes
#  structures through SSE registers in them.  Defined here, they are never
#  taken from the archive.
LIBSRC   := fs.cpp heap.cpp io.cpp start.cpp libc.cpp
SOURCES  += $(addprefix $(LIBCXXR2)/src/,$(LIBSRC))

include $(LIBCXXR2)/Makefile.tmpl

CXXFLAGS += -mgeneral-regs-only -I. -Ishim

$(BUILD)/main.o: pl_mpeg_r2.h vidhost.hpp

$(BUILD)/%.o: $(LIBCXXR2)/src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: header check-fp

header:
	python3 tools/trim_pl_mpeg.py third_party/pl_mpeg.h pl_mpeg_r2.h

#  Every function in the ELF that touches an SSE or x87 register.
check-fp: $(NAME).elf
#  (x87 by mnemonic: a bare "f" would also catch text disassembled with an
#  fs: prefix.)  Nothing printed is the answer wanted.
#  (From _start on: the GNU build-id note before it is a hash, not code, and
#  its bytes can look like anything to a disassembler.)
	@objdump -d --no-show-raw-insn --start-address=0x$$(nm $(NAME).elf | awk '$$3 == "_start" {print $$1}') $(NAME).elf | awk '/^[0-9a-f]+ <.*>:$$/ {fn=$$2} /%[xyz]mm|%st|\tf(i?ld|i?stp?|add|sub|mul|div|x?ch|u?comi?|abs|sqrt|n?init|n?stcw|ldcw|xsave|xrstor)[a-z]* / {print fn}' | sort | uniq -c
