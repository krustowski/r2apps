//go:build r2 || !r2abimock

package libgor2

import _ "unsafe" // required for go:linkname

// Syscall issues int 0x7f with two arguments and zero in RCX. The r2 target
// links its implementation from tinygo-r2/r2.S.
//
//go:export r2_syscall
func Syscall(number, arg1, arg2 uintptr) uintptr

// Syscall3 issues int 0x7f with arg3 in RCX. Older kernels ignore arg3.
//
//go:export r2_syscall3
func Syscall3(number, arg1, arg2, arg3 uintptr) uintptr

// _start saves the kernel's argv frame before switching to the Go stack.
//
//go:export r2_get_argc
func rawArgc() uintptr

//go:export r2_get_argv
func rawArgv() uintptr

//go:linkname flush runtime.flushStdout
func flush()

// SetConsoleSink mirrors buffered runtime output, including panic messages.
// The sink must not allocate, print, or yield. Nil restores console-only output.
//
//go:linkname SetConsoleSink runtime.setR2StdoutSink
func SetConsoleSink(sink func([]byte))

//go:linkname taskStackStats internal/task.r2StackStats
func taskStackStats(out *[7]uintptr)

//go:linkname taskStackCurrent internal/task.r2StackCurrent
func taskStackCurrent(used, size *uintptr)

//go:linkname taskSetStackCache internal/task.r2SetStackCache
func taskSetStackCache(n int) int
