package libgor2

import (
	"unsafe"
)

// Console output is buffered by the runtime and flushed a line at a time,
// because the kernel traces every syscall to the serial port and a syscall per
// character costs far more than the printing does.
//
// Print and friends go through that same buffer as the print builtin and the
// fmt package, so output from all three arrives in the order it was written.
// Only WriteDirect bypasses it.
//
//go:linkname flush runtime.flushStdout
func flush()

// Flush writes out anything the runtime has buffered.  Output is flushed
// automatically at each newline and when the program exits, so this is only
// needed to make partial lines appear --- a prompt, or a progress indicator.
func Flush() {
	flush()
}

// Print writes s to the console.
//
// A NUL byte ends the write: the kernel's print syscall stops at the first one.
func Print(s string) {
	print(s)
}

// Println writes s followed by a newline, and flushes.
func Println(s string) {
	print(s)
	print("\n")
}

// WriteDirect writes b to the console in a single syscall, bypassing the
// runtime's buffer.
//
// It exists for callers that already have a large block of text and do not
// want it copied through a 256-byte buffer a line at a time.  Mixing it with
// Print or fmt will interleave badly unless you Flush first.
func WriteDirect(b []byte) {
	if len(b) == 0 {
		return
	}

	Syscall(ScPrintString, ptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
}

// Clear clears the screen (syscall 0x11).
func Clear() error {
	return err(Syscall(ScClearScreen, 0, 0))
}
