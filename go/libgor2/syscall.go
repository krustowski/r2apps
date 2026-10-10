// Package libgor2 is the Go binding for the rou2exOS (r2) kernel ABI.
//
// It is to Go what c/libcr2 is to C: every syscall the kernel offers, plus the
// few conveniences (formatted output, C-string handling) that are awkward
// enough to be worth writing once.
//
// # Calling convention
//
// A syscall is an `int 0x7f` with the number in RAX, the first argument in RDI
// and the second in RSI; an optional third argument goes in RCX. The result
// comes back in RAX. Syscall passes zero in RCX; Syscall3 supplies its value.
//
// # Pointers
//
// Every syscall that takes a pointer checks that the whole buffer it is about
// to use lies inside one user region --- the process's own frame
// and user stacks (0x400000..0xA00000) or the kernel's shared user heap
// (0xC00000..0x1000000 and any heap extension)
// --- and returns InvalidInput otherwise. The default Go arena, globals and
// system stack live in the private frame at 0x600000..0x7C0000. The optional
// r2largeheap arena and KMalloc blocks live in the admitted kernel heap, so
// their buffers can also be handed to syscalls directly; see KBytes.
//
// The check is against the region, not against the Go slice: the kernel still
// is not told how long a buffer is, so a slice shorter than what a syscall
// writes is overrun into whatever follows it in the frame.
package libgor2

import "unsafe"

// ptr is the address of x, in the form the ABI wants it.
//
// The collector is conservative and never moves an object, and no syscall
// allocates, so an address handed to the kernel cannot go stale underneath it.
func ptr(x unsafe.Pointer) uintptr {
	return uintptr(x)
}

// Syscall numbers from c/libcr2/syscall.h and the current kernel dispatcher.
const (
	ScExit = 0x00

	// System and memory
	ScSysInfo       = 0x01
	ScRTC           = 0x02
	ScPipeSubscribe = 0x03
	ScGetTicks      = 0x04
	ScSleep         = 0x05
	ScMalloc        = 0x0a
	ScRealloc       = 0x0b
	ScFree          = 0x0f

	// Video and audio
	ScPrintString          = 0x10
	ScClearScreen          = 0x11
	ScWritePixel           = 0x12
	ScWriteVGA             = 0x13
	ScMapVram              = 0x14
	ScSetVideoMode         = 0x15
	ScGetFBInfo            = 0x16
	ScBlitBuffer           = 0x17
	ScGetKernelFont        = 0x18
	ScBlitIndexed          = 0x19
	ScPlayFreq             = 0x1a
	ScPlayFile             = 0x1b
	ScCaptureFB            = 0x1c
	ScCaptureFBRGB24Scaled = 0x1d
	ScPlayStop             = 0x1f

	// Filesystem
	ScReadFile    = 0x20
	ScWriteFile   = 0x21
	ScRenameFile  = 0x22
	ScDeleteFile  = 0x23
	ScWriteSubdir = 0x27
	ScListDir     = 0x28
	ScRunELF      = 0x2a
	ScRunFsCheck  = 0x2b
	ScListMounts  = 0x2c
	ScListDirPath = 0x2d
	ScChdir       = 0x2e
	ScListTasks   = 0x2f

	// Port I/O and networking
	ScWritePort       = 0x30
	ScReadPort        = 0x31
	ScSerialPort      = 0x32
	ScNewPacket       = 0x33
	ScSendPacket      = 0x34
	ScReceivePort     = 0x35
	ScSendPort        = 0x36
	ScNetRegister     = 0x37
	ScNetStatus       = 0x38
	ScReadFileAt      = 0x39
	ScWriteFileAt     = 0x3a
	ScKillTask        = 0x3b
	ScMemInfo         = 0x3c
	ScNetConfig       = 0x3d
	ScPower           = 0x3e
	ScAudio           = 0x3f
	ScFsStat          = 0x40
	ScCmdline         = 0x41
	ScDesktopRelaunch = 0x42
	ScHeapContains    = 0x43
	ScNetStats        = 0x45
	ScNetPorts        = 0x46
)

// Errno is a kernel return code.  Zero means success; every other value is one
// of the constants below.
type Errno uintptr

const (
	OK               = Errno(0x00)
	EUnchanged       = Errno(0xf9)
	EBusy            = Errno(0xfa)
	ENotImplemented  = Errno(0xfb)
	EInvalidInput    = Errno(0xfc)
	EFilesystemError = Errno(0xfd)
	EFileNotFound    = Errno(0xfe)
	EInvalidSyscall  = Errno(0xff)
)

// Error implements the error interface.
func (e Errno) Error() string {
	switch e {
	case OK:
		return "ok"
	case EUnchanged:
		return "frame unchanged"
	case EBusy:
		return "kernel busy"
	case ENotImplemented:
		return "not implemented"
	case EInvalidInput:
		return "invalid input"
	case EFilesystemError:
		return "filesystem error"
	case EFileNotFound:
		return "file not found"
	case EInvalidSyscall:
		return "invalid syscall"
	default:
		return "unknown error"
	}
}

// err turns a raw syscall result into an error, so that a caller can write
// `if err := libgor2.Chdir(p); err != nil`.
func err(ret uintptr) error {
	if ret == 0 {
		return nil
	}

	return Errno(ret)
}
