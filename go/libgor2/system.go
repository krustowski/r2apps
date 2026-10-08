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

// ReadSysInfo fills info with the kernel's system information block.  EBusy
// means the kernel's configuration was locked for longer than it would wait;
// info is left untouched then, so ask again rather than read it.
func ReadSysInfo(info *SysInfo) error {
	return err(Syscall(ScSysInfo, 0x01, ptr(unsafe.Pointer(info))))
}

// WriteSysInfo sets the IP address from info.IP. Other fields are ignored.
func WriteSysInfo(info *SysInfo) error {
	return err(Syscall(ScSysInfo, 0x02, ptr(unsafe.Pointer(info))))
}

// SetUser changes the system user name (syscall 0x01, op 0x03). Like libcr2,
// it accepts 1..31 printable ASCII bytes without spaces.
func SetUser(name string) error {
	if len(name) == 0 || len(name) >= len(SysInfo{}.User) {
		return EInvalidInput
	}
	for i := 0; i < len(name); i++ {
		if name[i] < 0x21 || name[i] > 0x7e {
			return EInvalidInput
		}
	}
	var info SysInfo
	copy(info.User[:], name)
	return err(Syscall(ScSysInfo, 0x03, ptr(unsafe.Pointer(&info))))
}

// ReadRTC fills t from the real-time clock (syscall 0x02).
func ReadRTC(t *RTC) error {
	return err(Syscall(ScRTC, 0x01, ptr(unsafe.Pointer(t))))
}

// Ticks is the number of milliseconds since boot, to the resolution of one PIT
// tick, which is 1 ms at the kernel's 1000 Hz (syscall 0x04).
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

// ListTasks returns the scheduler's task table (syscall 0x2f): one entry per
// task, so never more than MaxSlots.
func ListTasks() []TaskInfo {
	var buf [MaxSlots]TaskInfo

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

// CommandLineMax is the kernel's saved command-line capacity, in bytes.
const CommandLineMax = 128

// CommandLine returns the original command line for a PID from ListTasks
// (syscall 0x41). Kernel tasks return an empty string. EBusy means retry;
// EFileNotFound means the PID no longer exists.
func CommandLine(pid uint64) (string, error) {
	var buf [CommandLineMax]byte
	n := Syscall(ScCmdline, uintptr(pid), ptr(unsafe.Pointer(&buf[0])))
	if n > uintptr(len(buf)) {
		return "", Errno(n)
	}
	return string(buf[:n]), nil
}

// RequestDesktopRelaunch asks a supervised Memento process to exit
// cooperatively and restart with its original command line (syscall 0x42).
// ENotImplemented means that process has not registered support or was
// launched outside the graphics-session supervisor.
func RequestDesktopRelaunch(pid uint64) error {
	return err(Syscall(ScDesktopRelaunch, 0, uintptr(pid)))
}

// RegisterDesktopRelaunch opts the calling Memento process into cooperative
// desktop relaunch. Other processes receive ENotImplemented.
func RegisterDesktopRelaunch() error {
	return err(Syscall(ScDesktopRelaunch, 2, 0))
}

// DesktopRelaunchPending polls the calling process for a relaunch request.
// A pending request is committed only when it exits cooperatively.
func DesktopRelaunchPending() (bool, error) {
	n := Syscall(ScDesktopRelaunch, 1, 0)
	if n > 1 {
		return false, Errno(n)
	}
	return n == 1, nil
}

// Reboot restarts the machine (syscall 0x3e). It does not return on success.
func Reboot() error { return err(Syscall(ScPower, 1, 0)) }

// PowerOff switches off the machine (syscall 0x3e). It does not return on success.
func PowerOff() error { return err(Syscall(ScPower, 2, 0)) }

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
