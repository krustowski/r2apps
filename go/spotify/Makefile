NAME := spotify
GO_ROOT := $(abspath ..)
APP_ROOT := $(abspath ../..)
IMAGE ?= tinygo-r2:0.38.0
DOCKER ?= docker
OPT ?= 1
export CCACHE_DISABLE := 1
.PHONY: build test clean
build:
	$(MAKE) -C ../r2tls
	$(MAKE) -C codec
	$(DOCKER) run --rm -v $(APP_ROOT):/src:ro -v $(GO_ROOT)/spotify:/out \
	 -w /src/go/spotify $(IMAGE) tinygo build -target=r2 -tags=r2largeheap -stack-size=64KB -opt=$(OPT) -no-debug -o /out/spotify.elf .
	@stat -c 'spotify.elf: %s bytes' spotify.elf
test:
	$(MAKE) -C ../r2tls
	$(MAKE) -C codec test
	$(MAKE) -C ../libgor2/memento test
	go test . ./model ./protocol ./stream ./codec ./third_party/shannon ../r2tls/wire
	go test -tags=r2cryptotest ./stream
	go test ../r2net/tcp_receive.go ../r2net/tcp_receive_test.go
	go test ../r2net/dns_stream.go ../r2net/dns_stream_test.go
clean:
	rm -f spotify.elf
