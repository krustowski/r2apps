//go:build r2

package main

import (
	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/spotify/codec"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
	"runtime"
	"strconv"
	"sync/atomic"
	"unsafe"
)

// Fixed buffers let a runtime panic be recorded even when Go's heap is full.
var diagnosticPath = [...]byte{'/', 'm', 'n', 't', '/', 't', 'm', 'p', '/', 'S', 'P', 'O', 'T', 'I', 'F', 'Y', '.', 'L', 'O', 'G', 0}
var diagnosticData [1024]byte
var diagnosticRequest struct{ buffer, offset, length uint64 }
var diagnosticBlock *protocol.Block
var diagnosticEnd int
var diagnosticMemory runtime.MemStats
var diagnosticHeap r2.MemInfo
var diagnosticInitialized bool

func startDiagnostics(block *protocol.Block) {
	diagnosticBlock = block
	r2.SetConsoleSink(captureRuntimeOutput)
	diagnosticStage("started")
}
func writeDiagnostics() {
	diagnosticRequest.buffer = uint64(uintptr(unsafe.Pointer(&diagnosticData[0])))
	diagnosticRequest.length = uint64(len(diagnosticData))
	r2.Syscall(r2.ScWriteFileAt, uintptr(unsafe.Pointer(&diagnosticPath[0])), uintptr(unsafe.Pointer(&diagnosticRequest)))
}
func diagnosticStage(stage string) {
	if !diagnosticInitialized {
		for i := range diagnosticData {
			diagnosticData[i] = ' '
		}
		diagnosticInitialized = true
	}
	// Keep only the current stage and memory figures; never account credentials.
	for i := range diagnosticData {
		diagnosticData[i] = ' '
	}
	b := diagnosticData[:0]
	b = append(b, "stage: "...)
	b = append(b, stage...)
	b = append(b, '\n')
	runtime.ReadMemStats(&diagnosticMemory)
	used, peak := codec.MemoryStats()
	stackUsed, stackSize := r2.StackUsed()
	b = append(b, "go heap: "...)
	b = strconv.AppendUint(b, diagnosticMemory.HeapAlloc, 10)
	b = append(b, " native decoder: "...)
	b = strconv.AppendUint(b, used, 10)
	b = append(b, " peak: "...)
	b = strconv.AppendUint(b, peak, 10)
	b = append(b, " stack: "...)
	b = strconv.AppendUint(b, uint64(stackUsed), 10)
	b = append(b, '/')
	b = strconv.AppendUint(b, uint64(stackSize), 10)
	b = append(b, '\n')
	if r2.ReadMemInfo(&diagnosticHeap) == nil {
		b = append(b, "shared heap free: "...)
		b = strconv.AppendUint(b, diagnosticHeap.HeapFree, 10)
		b = append(b, " largest: "...)
		b = strconv.AppendUint(b, diagnosticHeap.HeapLargestFree, 10)
		b = append(b, '\n')
	}
	diagnosticEnd = len(b)
	writeDiagnostics()
}
func captureRuntimeOutput(output []byte) {
	if diagnosticBlock == nil {
		return
	}
	// Panic output can arrive in several fragments. Preserve its first 127 bytes.
	n := 0
	for n < len(diagnosticBlock.RuntimeText)-1 && diagnosticBlock.RuntimeText[n] != 0 {
		n++
	}
	for _, c := range output {
		if n < len(diagnosticBlock.RuntimeText)-1 {
			if c < ' ' {
				c = ' '
			}
			diagnosticBlock.RuntimeText[n] = c
			n++
		}
	}
	atomic.StoreUint32(&diagnosticBlock.ExitReason, protocol.ExitRuntime)
	for _, c := range output {
		if diagnosticEnd < len(diagnosticData) {
			diagnosticData[diagnosticEnd] = c
			diagnosticEnd++
		}
	}
	writeDiagnostics()
}
