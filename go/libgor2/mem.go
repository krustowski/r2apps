package libgor2

import "unsafe"

// The kernel's userland heap is shared by every process and supplied by
// syscalls 0x0a/0x0b/0x0f. It starts at 0xC00000-0xFFFFFF and newer kernels
// can extend it. Ordinary KMalloc blocks are managed explicitly, outside the
// collector's arena. The runtime uses the private linker arena by default;
// r2largeheap instead reserves one kernel block as its entire Go GC arena.
//
// Individual KMalloc blocks are useful for large file reads and frame buffers
// that should not enter Go's heap. Syscalls accept these buffers, and KBytes
// turns a block into a slice usable with the rest of this package.
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
// heap added up block by block (syscall 0x3c, version 2).  EBusy means the
// heap or the scheduler was locked at that moment; ask again.  A kernel from
// before 32 slots answers with version 1, which has room for 16; it is put
// into this layout, and Version stays 1.
func ReadMemInfo(info *MemInfo) error {
	info.Version = 0
	if e := err(Syscall(ScMemInfo, ptr(unsafe.Pointer(info)), 2)); e != nil {
		return e
	}
	if info.Version == 1 {
		// The fields before HeapBySlot are where they belong already.
		v1 := *(*memInfoV1)(unsafe.Pointer(info))
		info.HeapBySlot = [MaxSlots + 1]uint64{}
		copy(info.HeapBySlot[:16], v1.HeapBySlot[:16])
		info.HeapBySlot[MaxSlots] = v1.HeapBySlot[16]
		info.FrameBase = v1.FrameBase
		info.FrameSize = v1.FrameSize
		info.FrameVirt = v1.FrameVirt
		info.Slots = v1.Slots
		for k := range info.SlotTask {
			info.SlotTask[k] = 0xFF
		}
		copy(info.SlotTask[:16], v1.SlotTask[:])
	}
	return nil
}
