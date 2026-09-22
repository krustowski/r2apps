IMAGE  ?= tinygo-r2:0.38.0
DOCKER ?= docker

.PHONY: image clean

# Cheap: this only copies five files into the stock TinyGo image, so there is
# no LLVM rebuild and the whole thing takes seconds.
image:
	@$(DOCKER) build -t $(IMAGE) .

clean:
	@$(DOCKER) rmi -f $(IMAGE)
