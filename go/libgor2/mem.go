package libgor2

import "unsafe"

// The kernel's userland heap (0xC00000-0xFFFFFF) is a separate 4 MiB region
// shared by every process, handed out by syscalls 0x0a/0x0b/0x0f.  It is not
// where Go allocations come from: those live in the process's own frame, which
// the linker script carves up, and are managed by the garbage collector.
//
// It is the one way to get memory beyond the ~1.5 MiB the collector has, and
// memory it never scans: a large file read with ReadFileAt, or a frame buffer
// for Blit, that would otherwise take most of the Go heap and be walked on
// every collection.  Syscalls accept a heap block for any pointer argument as
// long as the buffer the call uses fits inside the heap, so KBytes turns a
// block into a slice the rest of this package takes like any other.
//
// Each block is tagged with the process that allocated it and freed by the
// kernel when that process exits, is killed or crashes.  A resized block keeps
// its owner.
//
// These return a bare address rather than an unsafe.Pointer on purpose: memory
// from here is outside the collector's arena, is never freed by it, and is not
// scanned by it.  Convert it where you use it, and free it yourself.

// KMalloc allocates size zeroed bytes from the kernel heap, or 0 on failure.
func KMalloc(size uint64) uintptr {
	return Syscall(ScMalloc, uintptr(size), 0)
}

// KRealloc resizes a block from KMalloc.  An address of 0 allocates fresh, and
// a size of 0 frees the block and returns 0.  The block may move, so a slice
// made from the old address by KBytes has to be made again.
func KRealloc(addr uintptr, size uint64) uintptr {
	return Syscall(ScRealloc, addr, uintptr(size))
}

// KFree returns a block to the kernel heap.
//
// The kernel frees whatever a process still holds when it dies, so leaking one
// of these costs nothing beyond the life of the program.
func KFree(addr uintptr) {
	if addr == 0 {
		return
	}

	Syscall(ScFree, addr, 0)
}

// KBytes is the size bytes at addr, a block from KMalloc, as a slice that can
// be passed to ReadFileAt, WriteVGA, Send and the rest.  It returns nil for a
// zero address.
//
// The conservative collector ignores a pointer outside its own arena, so the
// slice is safe to hold --- but it is only valid until KFree or KRealloc, and
// the collector does not look inside it, so a Go pointer stored there does not
// keep what it points at alive.
func KBytes(addr uintptr, size int) []byte {
	if addr == 0 || size <= 0 {
		return nil
	}

	return unsafe.Slice((*byte)(unsafe.Pointer(addr)), size)
}

// ReadMemInfo fills info with the RAM, the per-process frames and the user
// heap added up block by block (syscall 0x3c).  EBusy means the heap or the
// scheduler was locked at that moment; ask again.
func ReadMemInfo(info *MemInfo) error {
	return err(Syscall(ScMemInfo, ptr(unsafe.Pointer(info)), 0))
}
