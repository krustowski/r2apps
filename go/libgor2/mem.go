package libgor2

// The kernel's userland heap (0xC00000-0xFFFFFF) is a separate 4 MiB region
// shared by every process, handed out by syscalls 0x0a/0x0b/0x0f.  It is not
// where Go allocations come from: those live in the process's own frame, which
// the linker script carves up, and are managed by the garbage collector.
//
// Ordinary Go memory is already readable and writable by the kernel, so these
// are rarely what you want.  They are here because the ABI has them, and
// because a program that wants memory outside the collector's reach -- a large
// frame buffer it would rather not have scanned on every collection -- has no
// other way to ask for it.
//
// These return a bare address rather than an unsafe.Pointer on purpose.  The
// collector must never be handed one: the block lives outside the heap it
// knows about, and a pointer into it would be a pointer it cannot account for.
// Convert it where you use it, and free it yourself.

// KMalloc allocates size zeroed bytes from the kernel heap, or 0 on failure.
func KMalloc(size uint64) uintptr {
	return Syscall(ScMalloc, uintptr(size), 0)
}

// KRealloc resizes a block from KMalloc.  An address of 0 allocates fresh, and
// a size of 0 frees the block and returns 0.
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
