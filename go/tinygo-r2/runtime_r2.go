//go:build r2

// Runtime support for rou2exOS (r2).
//
// r2 is a hosted target wearing baremetal clothes: there is a kernel, and it
// offers a syscall ABI over `int 0x7f`, but it offers none of what TinyGo's
// unix runtime expects --- no libc, no mmap, no signals, no threads.  So the
// program is built as baremetal (heap and stack come from the linker script)
// and the four things the runtime genuinely cannot do without --- write a
// character, read the clock, sleep and exit --- are syscalls.

package runtime

import (
	"unsafe"

	_ "runtime/interrupt" // for the no-op interrupt shims the scheduler needs
)

// The handful of syscall numbers the runtime itself needs.  The rest of the
// ABI belongs to libgor2; these are here because printing, sleeping, reading
// the clock and exiting all have to work before any user code runs.
const (
	sysExit     = 0x00
	sysGetTicks = 0x04
	sysSleep    = 0x05
	sysPrint    = 0x10
)

// Implemented in targets/r2.S.
//
//go:export r2_syscall
func r2syscall(num, a1, a2 uintptr) uintptr

// Entry point for Go: _start calls this once it has switched to our stack.
//
//export main
func main() {
	preinit()
	run()
	exit(0)
}

func preinit() {
	// Nothing to do.  Unlike a microcontroller, we are started by a loader:
	// input/elf.rs zeroes everything between p_filesz and p_memsz, so .bss
	// arrives zeroed, and there is no separate flash image to copy .data
	// from.  Zeroing .bss here would in fact be harmful --- _start has
	// already stashed argc/argv in it.
}

// Output is buffered a line at a time.  Not for speed alone: the kernel traces
// every syscall to the serial port, so a syscall per character makes printing
// cost far more than the printing.
var (
	stdoutBuf [256]byte
	stdoutLen int
)

func putchar(c byte) {
	// The kernel's print stops at the first NUL, so a NUL can never be sent
	// and would silently truncate everything buffered behind it.
	if c == 0 {
		return
	}

	stdoutBuf[stdoutLen] = c
	stdoutLen++

	if c == '\n' || stdoutLen == len(stdoutBuf) {
		flushStdout()
	}
}

func flushStdout() {
	if stdoutLen == 0 {
		return
	}

	r2syscall(sysPrint, uintptr(unsafe.Pointer(&stdoutBuf[0])), uintptr(stdoutLen))
	stdoutLen = 0
}

// r2 delivers keystrokes to whichever process subscribed to the keyboard pipe
// rather than over a console device, so there is no stdin for the runtime to
// read.  Programs that want keys ask libgor2 for them.
func getchar() byte {
	return 0
}

func buffered() int {
	return 0
}

// The kernel's clock (syscall 0x04) counts milliseconds since boot, to the
// resolution of one PIT tick (1 ms at 1000 Hz), so a timeUnit here is one
// millisecond.
func ticks() timeUnit {
	return timeUnit(r2syscall(sysGetTicks, 0, 0))
}

func ticksToNanoseconds(t timeUnit) int64 {
	return int64(t) * 1e6
}

func nanosecondsToTicks(ns int64) timeUnit {
	return timeUnit(ns / 1e6)
}

// sleepTicks blocks the whole process, not just the calling goroutine: the
// scheduler only calls it once it has nothing else to run.
func sleepTicks(d timeUnit) {
	if d <= 0 {
		return
	}

	r2syscall(sysSleep, uintptr(d), 0)
}

func exit(code int) {
	flushStdout()
	r2syscall(sysExit, 0, uintptr(code))

	// 0x00 does not return; the kernel parks the process and reschedules.
	for {
	}
}

func abort() {
	exit(1)
}
