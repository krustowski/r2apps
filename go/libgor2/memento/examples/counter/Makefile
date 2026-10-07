GO_ROOT := $(abspath ../../../..)
IMAGE ?= tinygo-r2:0.38.0
DOCKER ?= docker
.PHONY: build clean
build:
	$(DOCKER) run --rm -v $(GO_ROOT):/src:ro -v $(CURDIR):/out \
	 -w /src/libgor2/memento/examples/counter $(IMAGE) \
	 tinygo build -target=r2 -opt=z -no-debug -o /out/wincount.elf .
clean:
	rm -f wincount.elf
