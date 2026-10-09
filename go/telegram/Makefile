#
#  telegram --- the Telegram client Memento's Telegram window hosts.
#
#    make            telegram.elf, built in the tinygo-r2 image
#    make test       the client, its drawing and the decoders, on the host
#    make gen        font.go and charmap.go again, from Memento's tables
#
#  The C half (media/native, stb_image and h264bsd, as Memento's web engine
#  builds them) and BearSSL (../r2tls) are built on the host first, then
#  linked in by TinyGo.
#
NAME := telegram
GO_ROOT := $(abspath ..)
APP_ROOT := $(abspath ../..)
IMAGE ?= tinygo-r2:0.38.0
DOCKER ?= docker
FLOPPY ?= $(abspath ../../fat.img)
export CCACHE_DISABLE := 1

WEB     := $(APP_ROOT)/cpp/memento-hello/web
STB     := $(APP_ROOT)/cpp/third_party/stb
H264BSD := $(APP_ROOT)/cpp/third_party/h264bsd/src
GCC_INC := $(shell gcc -print-file-name=include)
BUILD   := media/build

MEDIA_CFLAGS := -std=gnu99 -m64 -Os -ffreestanding -nostdlib -nostdinc -isystem $(GCC_INC) \
 -I$(WEB)/libc -I$(WEB) -I$(STB) -Imedia -fno-stack-protector -fno-asynchronous-unwind-tables \
 -fno-pie -fcf-protection=none -mno-red-zone -mgeneral-regs-only -ffunction-sections -fdata-sections -w
H264_CFLAGS := $(MEDIA_CFLAGS) -I$(H264BSD) -include $(WEB)/h264_r2.h
H264_OBJS := $(patsubst $(H264BSD)/%.c,$(BUILD)/h264bsd/%.o,$(wildcard $(H264BSD)/*.c))
MEDIA_OBJS := $(BUILD)/media.o $(BUILD)/h264.o $(H264_OBJS)

.PHONY: build media test gen install clean
build: media
	$(MAKE) -C ../r2tls
	$(DOCKER) run --rm -v $(APP_ROOT):/src:ro -v $(GO_ROOT)/telegram:/out \
	 -w /src/go/telegram $(IMAGE) tinygo build -target=r2 -tags=r2largeheap -stack-size=64KB -opt=z -no-debug -o /out/$(NAME).elf .
	@stat -c '$(NAME).elf: %s bytes' $(NAME).elf

media: $(BUILD)/libtgmedia.a

$(BUILD)/media.o: media/native/media.c media/media.h
	@mkdir -p $(dir $@)
	gcc $(MEDIA_CFLAGS) -c $< -o $@
$(BUILD)/h264.o: $(WEB)/h264.c $(WEB)/h264.h
	@mkdir -p $(dir $@)
	gcc $(H264_CFLAGS) -c $< -o $@
$(BUILD)/h264bsd/%.o: $(H264BSD)/%.c $(WEB)/h264_r2.h
	@mkdir -p $(dir $@)
	@gcc $(H264_CFLAGS) -c $< -o $@
$(BUILD)/libtgmedia.a: $(MEDIA_OBJS)
	@rm -f $@
	ar rcs $@ $^

test: media
	go vet . ./media
	go test . ./media ../libgor2/memento/hosted ../r2tls/wire

gen:
	go run tools/gen.go

# TELEGRAM.ELF onto the floppy, for `cd /mnt/fat` before Memento starts.
install: build
	mcopy -o -i $(FLOPPY) $(NAME).elf ::TELEGRAM.ELF

clean:
	rm -rf $(BUILD) $(NAME).elf
