package libgor2

// Goroutine stack accounting.  These read counters kept by the r2 build of
// TinyGo's task package (tinygo-r2/task_stack_r2.go), which paints every
// goroutine stack before it runs and caches stacks of finished goroutines for
// the next `go` statement.

// StackStats describes goroutine stacks since the program started.
type StackStats struct {
	Size      uintptr // bytes per goroutine stack (default-stack-size or -stack-size)
	Peak      uintptr // deepest any finished goroutine went, in bytes
	Allocated uintptr // stacks taken from the heap
	Reused    uintptr // stacks taken from the cache instead
	Exited    uintptr // goroutines that have returned
	Cached    int     // stacks waiting in the cache now
	CacheMax  int     // how many the cache keeps
}

// ReadStackStats returns the goroutine stack counters.  Peak only counts
// goroutines that have finished; a long-lived one can measure itself with
// StackUsed.
func ReadStackStats() StackStats {
	var v [7]uintptr
	taskStackStats(&v)

	return StackStats{
		Size:      v[0],
		Peak:      v[1],
		Allocated: v[2],
		Reused:    v[3],
		Exited:    v[4],
		Cached:    int(v[5]),
		CacheMax:  int(v[6]),
	}
}

// StackUsed is the calling goroutine's high-water mark so far and the size of
// its stack.  Both are zero on main's stack, which is not a goroutine stack.
func StackUsed() (used, size uintptr) {
	taskStackCurrent(&used, &size)

	return used, size
}

// SetStackCache sets how many finished goroutine stacks are kept for reuse
// (default 8, at most 32) and returns the previous limit.  Every cached stack
// is heap the collector cannot have, so a program that starts a burst of
// goroutines once and never again can set this to 0 afterwards.
func SetStackCache(n int) int {
	return taskSetStackCache(n)
}
