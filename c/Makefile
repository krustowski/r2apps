all:
	@for proj in $$(ls -d */); do \
		cd $${proj} && make build && cd ..; \
	done

.PHONY: libcr2
LIBCR2_BUILD_DIR := build/libcr2
LIBCR2_OBJECTS := $(patsubst libcr2/%.c,$(LIBCR2_BUILD_DIR)/%.o,$(wildcard libcr2/*.c))

libcr2: libcr2.a

$(LIBCR2_BUILD_DIR)/%.o: libcr2/%.c $(wildcard libcr2/*.h) Makefile
	@mkdir -p $(LIBCR2_BUILD_DIR)
	@gcc \
		-c \
		-O2 \
		-m64 \
		-static \
		-nostdlib \
		-nostdinc \
		-mno-red-zone \
		-ffreestanding \
		-I ./libcr2/ \
		$< -o $@

libcr2.a: $(LIBCR2_OBJECTS)
	@rm -f $@
	@ar \
		rcs \
		$@ \
		$(LIBCR2_OBJECTS)
