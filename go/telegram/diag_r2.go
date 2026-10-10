//go:build r2

package main

import (
	"runtime"
	"strconv"
	"unsafe"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

// The diagnostic log, /mnt/tmp/TELEGRAM.LOG: what the last requests took and
// how they ended, the memory there was, and a crash's last words --- for a
// machine where nothing else can be seen.  Memento's file viewer reads it.
// Paths and tokens never go in: requests are logged by their method alone.
// Everything here is fixed in size, so that a panic with the Go heap full
// can still be written down.
var (
	logPath    = [...]byte{'/', 'm', 'n', 't', '/', 't', 'm', 'p', '/', 'T', 'E', 'L', 'E', 'G', 'R', 'A', 'M', '.', 'L', 'O', 'G', 0}
	logData    [4096]byte
	logEnd     int
	logRequest struct{ buffer, offset, length uint64 }
	logHost    *hosted.Client
	logStats   runtime.MemStats
	logHeap    r2.MemInfo
)

func startLog(host *hosted.Client) {
	logHost = host
	r2.SetConsoleSink(runtimeOutput)
	line := "telegram.elf started, " + strconv.Itoa(host.Colours()) + " colours"
	if r2.ReadMemInfo(&logHeap) == nil {
		line += ", shared heap free " + strconv.FormatUint(logHeap.HeapFree/1024, 10) + " KiB (largest " +
			strconv.FormatUint(logHeap.HeapLargestFree/1024, 10) + ")"
	}
	logLine(line)
}

// logLine puts a line in the log with the time since boot and the Go heap.
func logLine(s string) {
	runtime.ReadMemStats(&logStats)
	t := r2.Ticks()
	appendLog([]byte(strconv.FormatUint(t/1000, 10) + "." + strconv.FormatUint(t%1000/100, 10) + "s heap " +
		strconv.FormatUint(logStats.HeapAlloc/1024, 10) + "/" + strconv.FormatUint(logStats.HeapSys/1024, 10) +
		" KiB  " + s + "\n"))
}

// runtimeOutput is what the runtime prints, a panic's message mostly: into
// the log, and into the window's message for when the program has ended.
func runtimeOutput(out []byte) {
	if logHost != nil {
		logHost.RuntimeOutput(out)
	}
	appendLog(out)
}

// appendLog keeps the newest lines: when b does not fit, the older half goes.
func appendLog(b []byte) {
	if len(b) > len(logData)/2 {
		b = b[:len(logData)/2]
	}
	if logEnd+len(b) > len(logData) {
		cut := logEnd / 2
		for cut < logEnd && logData[cut-1] != '\n' {
			cut++
		}
		copy(logData[:], logData[cut:logEnd])
		logEnd -= cut
	}
	logEnd += copy(logData[logEnd:], b)
	for i := logEnd; i < len(logData); i++ {
		logData[i] = ' '
	}
	logRequest.buffer = uint64(uintptr(unsafe.Pointer(&logData[0])))
	logRequest.length = uint64(len(logData))
	r2.Syscall(r2.ScWriteFileAt, uintptr(unsafe.Pointer(&logPath[0])), uintptr(unsafe.Pointer(&logRequest)))
}
