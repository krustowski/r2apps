//go:build r2 && r2netcheck

package main

import (
	"github.com/krustowski/rou2exOS-apps/go/spotify/third_party/shannon"
	"runtime"
)

func nativeHeapCheck() string {
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	if before.HeapSys < 4*1024*1024 {
		return "REGRESSION FAILED: large Go heap unavailable"
	}
	// This live object alone exceeds Spotify's entire former private GC arena.
	live := make([]byte, 2*1024*1024)
	for i := range live {
		live[i] = byte(i*17 + 3)
	}
	packet := make([]byte, 65535)
	cipher := shannon.New([]byte("native heap test key"))
	var mac [16]byte
	runtime.ReadMemStats(&before)
	for cycle := 0; cycle < 100; cycle++ {
		for i := range packet {
			packet[i] = byte(i*17 + 3)
		}
		cipher.NonceU32(uint32(cycle))
		cipher.Encrypt(packet)
		cipher.Finish(mac[:])
		cipher.NonceU32(uint32(cycle))
		cipher.Decrypt(packet)
		cipher.Finish(mac[:])
		for i, v := range packet {
			if v != byte(i*17+3) {
				return "REGRESSION FAILED: native Shannon round trip"
			}
		}
	}
	runtime.ReadMemStats(&after)
	if after.TotalAlloc != before.TotalAlloc {
		return "REGRESSION FAILED: Shannon packet allocations"
	}
	done := make(chan bool, 1)
	go func() {
		// The holder must survive collection while this goroutine is suspended.
		runtime.Gosched()
		runtime.GC()
		ok := true
		for i, v := range live {
			if v != byte(i*17+3) {
				ok = false
				break
			}
		}
		done <- ok
	}()
	runtime.GC()
	if !<-done {
		return "REGRESSION FAILED: external heap roots"
	}
	diagnosticStage("large Go heap + 100 Shannon packets passed")
	return ""
}
