//go:build r2

package media

import (
	"unsafe"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// Pixels go on the kernel's user heap, outside the collector's arena: a few
// animations are megabytes, and they are given back exactly when let go.
func alloc(n int) []byte {
	if n <= 0 {
		return nil
	}
	a := r2.KMalloc(uint64(n))
	if a == 0 {
		return nil
	}
	return r2.KBytes(a, n)
}

func release(b []byte) {
	if cap(b) > 0 {
		r2.KFree(uintptr(unsafe.Pointer(unsafe.SliceData(b))))
	}
}
