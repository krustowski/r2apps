//go:build r2 && r2largeheap

package runtime

// Select one contiguous, process-owned GC arena before initHeap or any Go
// allocation. The kernel maps and reclaims this block, including on a crash.
// Never move or realloc it: live Go pointers and collector metadata stay put.
func initR2Heap() {
	const alignment = uintptr(32) // conservative GC block size on amd64
	for size := uintptr(8 * 1024 * 1024); size >= 2*1024*1024; size /= 2 {
		block := r2syscall(0x0a, size+alignment-1, 0)
		if block == 0 {
			continue
		}
		heapStart = (block + alignment - 1) &^ (alignment - 1)
		heapEnd = heapStart + size
		return
	}
	// Older kernels or machines with insufficient RAM retain the private arena.
}
