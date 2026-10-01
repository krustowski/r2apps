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

# Goroutine stacks: task_stack_r2.go paints and reuses them.  It replaces the
# stock task_stack.go, which is excluded for r2 by its build constraint --- the
# grep fails the build if a TinyGo bump has changed the line being edited.
COPY task_stack_r2.go /usr/local/tinygo/src/internal/task/task_stack_r2.go
USER root
RUN f=/usr/local/tinygo/src/internal/task/task_stack.go && \
    sed -i '1s#^//go:build scheduler.tasks || scheduler.cores$#//go:build (scheduler.tasks || scheduler.cores) \&\& !r2#' $f && \
    head -1 $f | grep -qF '&& !r2'
USER tinygo
