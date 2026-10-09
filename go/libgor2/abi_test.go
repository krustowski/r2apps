//go:build r2abimock && !r2

package libgor2

import (
	"bytes"
	"strings"
	"testing"
	"unsafe"
)

func mockSyscall(t *testing.T, f func(number, arg1, arg2, arg3 uintptr) uintptr) {
	previous := abiSyscall
	abiSyscall = f
	t.Cleanup(func() { abiSyscall = previous })
}

func TestCaptureMetadata(t *testing.T) {
	rgb := bytes.Repeat([]byte{0x5a}, 640*480*3)
	info := FBCaptureInfo{FrameID: 42, TimestampMS: 99, Flags: 7, Reserved: 1}
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		if number != ScCaptureFBRGB24Scaled || arg1 != uintptr(unsafe.Pointer(&rgb[0])) ||
			arg2 != (uintptr(1)<<63|640<<16|480) || arg3 != uintptr(unsafe.Pointer(&info)) {
			t.Fatal("wrong capture arguments")
		}
		if info.FrameID != 42 || info.TimestampMS != 0 || info.Flags != 0 || info.Reserved != 0 {
			t.Fatalf("stale output metadata: %+v", info)
		}
		info.TimestampMS, info.Flags = 123, FBCaptureInfoSnapshot
		return uintptr(EUnchanged)
	})
	if e := CaptureFramebufferRGB24ScaledIfNew(rgb, 640, 480, &info); e != EUnchanged {
		t.Fatalf("unchanged result: %v", e)
	}
	if info.FrameID != 42 || info.TimestampMS != 123 || info.Flags != FBCaptureInfoSnapshot || bytes.Count(rgb, []byte{0x5a}) != len(rgb) {
		t.Fatal("unchanged snapshot lost metadata or modified pixels")
	}
}

func TestCaptureWithoutMetadata(t *testing.T) {
	rgb := make([]byte, 6)
	info := FBCaptureInfo{FrameID: 7, TimestampMS: 9, Flags: 1}
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		if number != ScCaptureFBRGB24Scaled || arg2 != 2<<16|1 || arg3 != 0 {
			t.Fatal("ordinary capture opted into metadata")
		}
		return 0
	})
	if e := CaptureFramebufferRGB24Scaled(rgb, 2, 1); e != nil {
		t.Fatal(e)
	}
	// An old kernel ignores the metadata pointer and copies pixels normally.
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr { return 0 })
	if e := CaptureFramebufferRGB24ScaledIfNew(rgb, 2, 1, &info); e != nil || info.TimestampMS != 0 || info.Flags != 0 {
		t.Fatalf("old-kernel fallback: %+v, %v", info, e)
	}
}

func TestCaptureRejectsInvalidBuffers(t *testing.T) {
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		t.Fatal("invalid capture reached kernel")
		return 0
	})
	for _, size := range [][3]uint32{{0, 1, 3}, {1, 0, 3}, {65536, 1, 3}, {1, 65536, 3}, {2, 2, 11}} {
		info := FBCaptureInfo{FrameID: 1, Flags: 2}
		if e := CaptureFramebufferRGB24ScaledIfNew(make([]byte, size[2]), size[0], size[1], &info); e != EInvalidInput || info.Flags != 2 {
			t.Fatalf("invalid capture %v: %+v, %v", size, info, e)
		}
	}
}

func TestCommandLineCountsAndErrors(t *testing.T) {
	for _, n := range []uintptr{0, 128, uintptr(EBusy), uintptr(EFileNotFound), uintptr(EInvalidSyscall)} {
		t.Run(Errno(n).Error(), func(t *testing.T) {
			mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
				if number != ScCmdline || arg1 != 23 || arg3 != 0 {
					t.Fatal("wrong command-line arguments")
				}
				if n <= CommandLineMax {
					copy(unsafe.Slice((*byte)(unsafe.Pointer(arg2)), CommandLineMax), strings.Repeat("x", int(n)))
				}
				return n
			})
			line, e := CommandLine(23)
			if n <= CommandLineMax {
				if e != nil || line != strings.Repeat("x", int(n)) {
					t.Fatalf("count %d: %q, %v", n, line, e)
				}
			} else if line != "" || e != Errno(n) {
				t.Fatalf("kernel error %d: %q, %v", n, line, e)
			}
		})
	}
}

func TestSetUser(t *testing.T) {
	calls := 0
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		calls++
		info := (*SysInfo)(unsafe.Pointer(arg2))
		if number != ScSysInfo || arg1 != 3 || arg3 != 0 || string(info.User[:5]) != "alice" || info.User[5] != 0 {
			t.Fatal("wrong set-user request")
		}
		return uintptr(EBusy)
	})
	for _, name := range []string{"", "two words", "nul\x00tail", "\x7f", "é", strings.Repeat("x", 32)} {
		if e := SetUser(name); e != EInvalidInput {
			t.Fatalf("accepted user %q: %v", name, e)
		}
	}
	if e := SetUser("alice"); e != EBusy || calls != 1 {
		t.Fatalf("set user: %v, %d calls", e, calls)
	}
}

func TestDesktopControl(t *testing.T) {
	for _, result := range []uintptr{0, 1, uintptr(EBusy), uintptr(ENotImplemented), uintptr(EInvalidSyscall)} {
		mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
			if number != ScDesktopRelaunch || arg1 != 1 || arg2 != 0 || arg3 != 0 {
				t.Fatal("wrong desktop poll")
			}
			return result
		})
		pending, e := DesktopRelaunchPending()
		if pending != (result == 1) || (result <= 1 && e != nil) || (result > 1 && e != Errno(result)) {
			t.Fatalf("poll %d: %v, %v", result, pending, e)
		}
	}
	for _, c := range []struct {
		op, pid uintptr
		call    func() error
	}{
		{0, 23, func() error { return RequestDesktopRelaunch(23) }},
		{2, 0, RegisterDesktopRelaunch},
	} {
		mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
			if number != ScDesktopRelaunch || arg1 != c.op || arg2 != c.pid || arg3 != 0 {
				t.Fatal("wrong desktop request")
			}
			return uintptr(ENotImplemented)
		})
		if e := c.call(); e != ENotImplemented {
			t.Fatal(e)
		}
	}
}

func TestNetworkConfigAndPower(t *testing.T) {
	var cfg NetConfig
	for _, op := range []uintptr{1, 2} {
		mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
			if number != ScNetConfig || arg1 != op || arg2 != uintptr(unsafe.Pointer(&cfg)) || arg3 != 0 {
				t.Fatal("wrong net-config request")
			}
			return uintptr(EBusy)
		})
		var e error
		if op == 1 {
			e = ReadNetConfig(&cfg)
		} else {
			e = WriteNetConfig(&cfg)
		}
		if e != EBusy {
			t.Fatal(e)
		}
	}
	for _, c := range []struct {
		op   uintptr
		call func() error
	}{{1, Reboot}, {2, PowerOff}} {
		mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
			if number != ScPower || arg1 != c.op || arg2 != 0 || arg3 != 0 {
				t.Fatal("wrong power request")
			}
			return uintptr(EInvalidSyscall)
		})
		if e := c.call(); e != EInvalidSyscall {
			t.Fatal(e)
		}
	}
}

func TestCaptureInfoLayout(t *testing.T) {
	var info FBCaptureInfo
	if unsafe.Sizeof(info) != 24 || unsafe.Offsetof(info.FrameID) != 0 || unsafe.Offsetof(info.TimestampMS) != 8 || unsafe.Offsetof(info.Flags) != 16 || unsafe.Offsetof(info.Reserved) != 20 {
		t.Fatal("capture metadata no longer matches the packed kernel/C ABI")
	}
}

func TestSharedHeapContains(t *testing.T) {
	answer := uintptr(1)
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr {
		if number != ScHeapContains || arg1 != 0xa104ce0 || arg2 != 8336 || arg3 != 0 {
			t.Fatal("wrong heap check arguments")
		}
		return answer
	})
	if !SharedHeapContains(0xa104ce0, 8336) {
		t.Fatal("rejected a block in the heap extension")
	}
	answer = 0
	if SharedHeapContains(0xa104ce0, 8336) {
		t.Fatal("accepted what the kernel refused")
	}

	// A kernel without 0x43 can only vouch for the first 4 MiB.
	mockSyscall(t, func(number, arg1, arg2, arg3 uintptr) uintptr { return uintptr(EInvalidSyscall) })
	for _, c := range []struct {
		addr uintptr
		size uint64
		want bool
	}{
		{0xc00000, 8336, true}, {0xffdf70, 8336, true}, {0xffdf71, 8336, false},
		{0xbffff0, 16, false}, {0xc00000, 0, false}, {0xa104ce0, 8336, false},
	} {
		if SharedHeapContains(c.addr, c.size) != c.want {
			t.Fatalf("old kernel, %#x+%d: want %v", c.addr, c.size, c.want)
		}
	}
}
