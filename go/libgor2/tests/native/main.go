//go:build r2

// ABI smoke test for an isolated r2 VM. Build as ABI.ELF, then run
// `fg ABI --smoke` from a FAT floppy's INIT.RC. It spawns a temporary
// Ethernet driver process and writes a temporary file on the RAM disk.
package main

import (
	"fmt"
	"strings"
	"time"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

func check(ok bool, message string) {
	if !ok {
		panic(message)
	}
}

func main() {
	// QEMU's debug console records buffered output and panic diagnostics.
	r2.SetConsoleSink(func(output []byte) {
		for _, b := range output {
			r2.WritePort(0xe9, uint32(b))
		}
	})
	args := r2.Args()
	if len(args) > 1 && args[1] == "--driver" {
		check(r2.NetRegister() == nil, "driver registration")
		cfg := r2.NetConfig{IP: [4]byte{10, 3, 4, 2}, Netmask: [4]byte{255, 255, 255, 0}, Source: r2.NetSourceStatic}
		check(r2.WriteNetConfig(&cfg) == nil, "publish network config")
		for {
			r2.SleepMS(1000)
		}
	}
	check(len(args) == 2 && args[1] == "--smoke", "startup argv")
	var system r2.SysInfo
	check(r2.ReadSysInfo(&system) == nil, "read sysinfo")
	previousUser := strings.TrimRight(string(system.User[:]), "\x00")
	check(r2.SetUser("goabi") == nil, "set user")
	check(r2.ReadSysInfo(&system) == nil && string(system.User[:5]) == "goabi", "read changed user")
	check(r2.SetUser(previousUser) == nil, "restore user")
	check(r2.RegisterDesktopRelaunch() == r2.ENotImplemented, "ordinary app cannot register desktop")
	_, e := r2.DesktopRelaunchPending()
	check(e == r2.ENotImplemented, "ordinary app cannot poll desktop")
	check(r2.Syscall(r2.ScPower, 0, 0) == uintptr(r2.EInvalidInput), "power syscall present")

	entries, e := r2.ListDirPath("/")
	check(e == nil && len(entries) > 0, "root directory")
	entries, e = r2.ListDirPath("/mnt")
	check(e == nil && len(entries) > 0, "mount directory")
	path, data := "/mnt/tmp/GOABI.TST", []byte("Go ABI range check")
	n, e := r2.WriteFileAt(path, data, 0)
	check(e == nil && n == len(data), "RAM disk write")
	read, e := r2.ReadFile(path)
	check(e == nil && string(read) == string(data), "RAM disk read")
	check(r2.Delete(path) == nil, "RAM disk delete")

	driver := r2.Run("ABI", "ABI --driver")
	check(driver != 0, "spawn driver")
	line, e := r2.CommandLine(uint64(driver))
	check(e == nil && line == "ABI --driver", "saved command line")
	var status r2.NetStatus
	for deadline := r2.Ticks() + 5000; ; {
		check(r2.ReadNetStatus(&status) == nil, "network status")
		if status.DrvActive != 0 {
			break
		}
		check(r2.Ticks() < deadline, "driver startup timed out")
		r2.SleepMS(1)
	}
	stack, e := r2net.Open(r2net.Options{Link: "eth"})
	check(e == nil && !stack.CanICMP(), "another process owns driver")
	for _, ip := range []r2net.IP{{127, 0, 0, 1}, {127, 7, 8, 9}, stack.LocalIP()} {
		_, e := stack.Ping(ip, time.Second)
		check(e == nil, "local ping without driver ownership")
	}
	stack.Close()
	check(r2.Kill(uint64(driver)), "kill driver")
	check(r2.ReadNetStatus(&status) == nil && status.DrvActive == 0, "release driver on kill")
	check(r2.NetRegister() == nil, "re-register driver")
	check(r2.NetBindPort(23456) == nil && r2.NetUnbindPort(23456) == nil, "bind/unbind TCP")

	var fb r2.FBInfo
	if r2.GetFBInfo(&fb) == nil && fb.BPP == 32 {
		check(r2.IndexedPresentAvailable(), "indexed presentation")
		pixels, palette := []byte{1}, make([]byte, 768)
		palette[3] = 0x5a
		check(r2.BeginIndexedPresent() == nil, "begin indexed frame")
		check(r2.BlitIndexed(pixels, palette, 1, 1, 0, 1, true) == nil, "draw indexed frame")
		check(r2.EndIndexedPresent(pixels, palette, 1, 1) == nil, "publish indexed frame")
		rgb := make([]byte, 640*480*3)
		var metadata r2.FBCaptureInfo
		check(r2.CaptureFramebufferRGB24ScaledIfNew(rgb, 640, 480, &metadata) == nil, "capture metadata")
		check(metadata.FrameID != 0 && metadata.Flags == r2.FBCaptureInfoSnapshot, "snapshot metadata layout")
		rgb[0] = 0xab
		check(r2.CaptureFramebufferRGB24ScaledIfNew(rgb, 640, 480, &metadata) == r2.EUnchanged && rgb[0] == 0xab, "skip unchanged capture")
		fmt.Println("GO CAPTURE CHECK PASS")
	} else {
		fmt.Println("GO CAPTURE CHECK SKIPPED (requires 32bpp)")
	}
	fmt.Println("GO ABI CHECK PASS")
	// Optional QEMU isa-debug-exit device: successful test exits with code 33.
	r2.WritePort(0xf4, 0x10)
}
