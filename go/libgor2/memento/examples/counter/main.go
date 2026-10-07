//go:build r2

// wincount.elf is a minimal child for a Memento window. Its C++ wrapper uses
// host.hpp to send an increment action and paint Count/Label from snapshots.
package main

import (
	"fmt"
	"strconv"
	"time"
	"unsafe"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento"
)

const (
	magic     = 0x54433252 // "R2CT"
	version   = 1
	increment = 1
)

type Snapshot struct {
	Count uint32
	Label [64]byte
}

func main() {
	host, err := memento.Attach[Snapshot](r2.Args(), magic, version, r2.Ticks())
	if err != nil {
		fmt.Println("Counter host:", err)
		return
	}
	r2.SetConsoleSink(host.RuntimeOutput)
	var snapshot Snapshot
	updateLabel := func() { memento.Text(snapshot.Label[:], "Count: "+strconv.FormatUint(uint64(snapshot.Count), 10)) }
	updateLabel()
	for host.Poll(r2.Ticks()) == memento.Running {
		if host.DrainCommands(func(command memento.Command) {
			if command.Op == increment {
				snapshot.Count++
				updateLabel()
			}
		}) != memento.Running {
			break
		}
		host.Publish(&snapshot)
		time.Sleep(20 * time.Millisecond)
	}
	host.Close()
}

const (
	_ = uint(unsafe.Sizeof(Snapshot{}) - 68)
	_ = uint(68 - unsafe.Sizeof(Snapshot{}))
	_ = uint(unsafe.Sizeof(memento.Block[Snapshot]{}) - 568)
	_ = uint(568 - unsafe.Sizeof(memento.Block[Snapshot]{}))
)
