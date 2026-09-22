package libgor2

import "unsafe"

// Exit ends the process with the given status code (syscall 0x00).  It does
// not return: the kernel reaps the process and reschedules.
//
// Returning from main does the same thing, so this is for exiting early.
func Exit(code int) {
	Flush()
	Syscall(ScExit, 0, uintptr(code))

	for {
	}
}

// ReadSysInfo fills info with the kernel's system information block.
func ReadSysInfo(info *SysInfo) error {
	return err(Syscall(ScSysInfo, 0x01, ptr(unsafe.Pointer(info))))
}

// WriteSysInfo writes info back to the kernel.  The Ethernet driver uses this
// to publish the IP address it was given.
func WriteSysInfo(info *SysInfo) error {
	return err(Syscall(ScSysInfo, 0x02, ptr(unsafe.Pointer(info))))
}

// ReadRTC fills t from the real-time clock (syscall 0x02).
func ReadRTC(t *RTC) error {
	return err(Syscall(ScRTC, 0x01, ptr(unsafe.Pointer(t))))
}

// Ticks is the number of milliseconds since boot, at the 10 ms resolution of
// the PIT (syscall 0x04).
func Ticks() uint64 {
	return uint64(Syscall(ScGetTicks, 0, 0))
}

// SleepMS blocks the whole process for at least ms milliseconds, rounded up to
// the next PIT tick (syscall 0x05).
//
// This parks the process in the kernel, so every goroutine stops with it.  To
// sleep one goroutine and let the others run, use time.Sleep.
func SleepMS(ms uint64) {
	if ms == 0 {
		return
	}

	Syscall(ScSleep, uintptr(ms), 0)
}

// ListTasks returns the scheduler's task table (syscall 0x2f).  The kernel has
// ten process slots, so it never returns more than ten entries.
func ListTasks() []TaskInfo {
	var buf [10]TaskInfo

	n := int(Syscall(ScListTasks, ptr(unsafe.Pointer(&buf[0])), uintptr(len(buf))))
	if n <= 0 || n > len(buf) {
		return nil
	}

	out := make([]TaskInfo, n)
	copy(out, buf[:n])

	return out
}

// Kill ends the process with the given PID --- the number ListTasks reports,
// not a slot (syscall 0x3b).  It reports whether there was a process to kill.
func Kill(pid uint64) bool {
	return Syscall(ScKillTask, uintptr(pid), 0) == 0
}

// Run loads name as a background task and returns its PID, or 0 on failure
// (syscall 0x2a).
//
// name is a filename of at most 12 characters, with .ELF appended when it has
// no extension.  args is a space-delimited command line whose first token
// becomes argv[0]; pass "" to use name alone.
func Run(name, args string) uint8 {
	nameBuf := cstring(name)

	var argsPtr uintptr
	if args != "" {
		argsBuf := cstring(args)
		argsPtr = ptr(unsafe.Pointer(&argsBuf[0]))
	}

	return uint8(Syscall(ScRunELF, ptr(unsafe.Pointer(&nameBuf[0])), argsPtr))
}

// Implemented in targets/r2.S, which stashes the argv frame the kernel builds
// on the initial stack before _start switches away from it.
//
//go:export r2_get_argc
func rawArgc() uintptr

//go:export r2_get_argv
func rawArgv() uintptr

// Args returns the command line, with Args()[0] the program name --- the same
// thing os.Args would be if this target had an os package worth the name.
func Args() []string {
	n := int(rawArgc())
	if n <= 0 {
		return nil
	}

	addr := rawArgv()
	if addr == 0 {
		return nil
	}

	// One conversion out of the address the kernel left us, then pointer
	// arithmetic from there: the vector is an array of char* built by
	// push_user_args on the initial stack, which nothing overwrites.
	base := unsafe.Pointer(addr)
	size := int(unsafe.Sizeof(uintptr(0)))

	out := make([]string, 0, n)
	for i := 0; i < n; i++ {
		p := *(**byte)(unsafe.Add(base, i*size))
		if p == nil {
			break
		}

		out = append(out, goString(p))
	}

	return out
}

// cstring copies s into a NUL-terminated buffer for a kernel that reads C
// strings.  The buffer is a Go allocation, so it lives in the process's own
// frame and satisfies the ABI's pointer range check.
func cstring(s string) []byte {
	b := make([]byte, len(s)+1)
	copy(b, s)

	return b
}

// goString reads a NUL-terminated string the kernel left in our memory.
func goString(p *byte) string {
	if p == nil {
		return ""
	}

	n := 0
	for *(*byte)(unsafe.Add(unsafe.Pointer(p), n)) != 0 {
		n++
	}

	return string(unsafe.Slice(p, n))
}
