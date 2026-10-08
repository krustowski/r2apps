//go:build r2abimock && !r2

package libgor2

// Host-only hooks for ABI regression tests. The r2 build always uses abi.go.
// Unexpected calls panic so tests cannot silently depend on a fake kernel.
var abiSyscall = func(number, arg1, arg2, arg3 uintptr) uintptr {
	if number == ScGetTicks {
		return 0
	}
	panic("unexpected mock syscall")
}

func Syscall(number, arg1, arg2 uintptr) uintptr {
	return abiSyscall(number, arg1, arg2, 0)
}

func Syscall3(number, arg1, arg2, arg3 uintptr) uintptr {
	return abiSyscall(number, arg1, arg2, arg3)
}

func rawArgc() uintptr                     { panic("mock argv unavailable") }
func rawArgv() uintptr                     { panic("mock argv unavailable") }
func flush()                               {}
func SetConsoleSink(sink func([]byte))     { panic("mock console sink unavailable") }
func taskStackStats(out *[7]uintptr)       { panic("mock stack stats unavailable") }
func taskStackCurrent(used, size *uintptr) { panic("mock stack stats unavailable") }
func taskSetStackCache(n int) int          { panic("mock stack cache unavailable") }
