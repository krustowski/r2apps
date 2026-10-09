//go:build r2

package memento

import "github.com/krustowski/rou2exOS-apps/go/libgor2"

// heapContains asks the kernel whether the block lies in one region of the
// shared heap (syscall 0x43), so that an address in neither the first 4 MiB
// nor the extension is refused before it is read.
var heapContains = func(address, size uintptr) bool {
	return libgor2.SharedHeapContains(address, uint64(size))
}
