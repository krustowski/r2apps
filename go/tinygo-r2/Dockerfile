# A TinyGo image that knows about rou2exOS.
#
# TinyGo reads its own standard library and target definitions from TINYGOROOT
# at compile time rather than baking them into the binary, so teaching it a new
# target is a matter of dropping four files in --- no LLVM rebuild, and the
# image builds in seconds.

FROM tinygo/tinygo:0.38.0

COPY runtime_r2.go    /usr/local/tinygo/src/runtime/runtime_r2.go
COPY interrupt_r2.go  /usr/local/tinygo/src/runtime/interrupt/interrupt_r2.go
COPY r2.S             /usr/local/tinygo/targets/r2.S
COPY r2.ld            /usr/local/tinygo/targets/r2.ld
COPY r2.json          /usr/local/tinygo/targets/r2.json
