//go:build scheduler.tasks && r2

// task_stack_r2.go replaces TinyGo's task_stack.go on r2 (the Dockerfile
// excludes the stock file with a `!r2` build constraint).  It keeps the same
// state layout and canary, and adds two things a 1.5 MiB heap needs:
//
//   - Painting.  Every goroutine stack is filled with the canary word before
//     the goroutine runs, and how much of the pattern has been overwritten when
//     it exits is its high-water mark.  libgor2.ReadStackStats reports the
//     deepest one seen, which is what default-stack-size should be chosen from.
//
//   - Reuse.  A finished goroutine's stack goes into a small cache and the
//     next `go` statement takes it from there, instead of leaving it on the
//     heap until the collector happens to run.  Without this, a loop of
//     short-lived goroutines grows the heap by one stack per goroutine and
//     fragments it.

package task

import (
	"unsafe"
)

//go:linkname runtimePanic runtime.runtimePanic
func runtimePanic(str string)

// Stack canary, to detect a stack overflow.  The same value paints the whole
// stack, so the canary is simply the last word of the pattern to go.
const stackCanary = uintptr(uint64(0x670c1333b83bf575) & uint64(^uintptr(0)))

const wordSize = unsafe.Sizeof(uintptr(0))

// state is a structure which holds a reference to the state of the task.
// When the task is suspended, the registers are stored onto the stack and the
// stack pointer is stored into sp.
type state struct {
	// sp is the stack pointer of the saved state.
	sp uintptr

	// canaryPtr points to the lowest word of the stack.  The collector finds
	// the stack through it.
	canaryPtr *uintptr

	// size is the stack's length in bytes, kept so the stack can be measured
	// and cached when the goroutine exits.
	size uintptr
}

// The stack cache.  A slot holds a stack of stackCacheSize bytes that no
// goroutine is running on; being in this array is what keeps the collector
// from taking it back.
const stackCacheSlots = 32

var (
	stackCache      [stackCacheSlots]unsafe.Pointer
	stackCacheLen   int
	stackCacheLimit = 8
	stackCacheSize  uintptr

	stackAllocated uintptr // stacks taken from the heap
	stackReused    uintptr // stacks taken from the cache
	stackExited    uintptr // goroutines that have returned
	stackPeak      uintptr // deepest high-water mark among them, in bytes
	stackLastSize  uintptr // size of the most recently created stack
)

// wipeMargin is how far below taskExit's own frame the dead part of a stack is
// repainted.  It covers the frame itself and anything a leaf call keeps below
// the stack pointer.
const wipeMargin = 256

//export tinygo_task_exit
func taskExit() {
	s := &currentTask.state
	stack := unsafe.Pointer(s.canaryPtr)

	used := stackUsed(stack, s.size)
	if used > stackPeak {
		stackPeak = used
	}
	stackExited++

	// Repaint what the goroutine left behind below this frame.  The stack is
	// scanned conservatively for as long as it sits in the cache, and stale
	// frames would otherwise keep whatever they pointed at alive.
	var here uintptr
	limit := uintptr(unsafe.Pointer(&here)) - wipeMargin
	if limit > uintptr(stack) && limit < uintptr(stack)+s.size {
		for p := uintptr(stack) + wordSize; p < limit; p += wordSize {
			*(*uintptr)(unsafe.Pointer(p)) = stackCanary
		}
	}

	// Hand the stack to the next `go` statement.  This goroutine is still
	// running on it, but nothing can take it before Pause switches away: the
	// scheduler is cooperative and Pause does not allocate.
	if stackCacheLen < stackCacheLimit && (stackCacheLen == 0 || stackCacheSize == s.size) {
		stackCache[stackCacheLen] = stack
		stackCacheLen++
		stackCacheSize = s.size
	}

	Pause()
}

// initialize the state and prepare to call the specified function with the
// specified argument bundle.
func (s *state) initialize(fn uintptr, args unsafe.Pointer, stackSize uintptr) {
	var stack unsafe.Pointer

	if stackCacheLen > 0 && stackCacheSize == stackSize {
		stackCacheLen--
		stack = stackCache[stackCacheLen]
		stackCache[stackCacheLen] = nil
		stackReused++
	} else {
		stack = runtime_alloc(stackSize, nil)
		stackAllocated++
	}
	stackLastSize = stackSize

	// Paint the whole stack.  The lowest word doubles as the canary that Pause
	// checks on every switch.
	for p := uintptr(stack); p < uintptr(stack)+stackSize; p += wordSize {
		*(*uintptr)(unsafe.Pointer(p)) = stackCanary
	}

	s.canaryPtr = (*uintptr)(stack)
	s.size = stackSize

	// The initial register values sit at the top of the stack and are popped
	// on the first switch to the goroutine (see archInit).
	r := (*calleeSavedRegs)(unsafe.Add(stack, stackSize-unsafe.Sizeof(calleeSavedRegs{})))

	s.archInit(r, fn, args)
}

// stackUsed is how many bytes of a painted stack have been written, counted
// from the top down to the lowest word that no longer holds the pattern.
func stackUsed(stack unsafe.Pointer, size uintptr) uintptr {
	for off := uintptr(0); off < size; off += wordSize {
		if *(*uintptr)(unsafe.Add(stack, off)) != stackCanary {
			return size - off
		}
	}

	return 0
}

//export tinygo_swapTask
func swapTask(oldStack uintptr, newStack *uintptr)

// startTask is a small wrapper function that sets up the first (and only)
// argument to the new goroutine and makes sure it is exited when the goroutine
// finishes.
//
//go:extern tinygo_startTask
var startTask [0]uint8

// start creates and starts a new goroutine with the given function and arguments.
// The new goroutine is scheduled to run later.
func start(fn uintptr, args unsafe.Pointer, stackSize uintptr) {
	t := &Task{}
	t.state.initialize(fn, args, stackSize)
	scheduleTask(t)
}

// OnSystemStack returns whether the caller is running on the system stack.
func OnSystemStack() bool {
	return Current() == nil
}

// r2StackStats is read by libgor2.ReadStackStats through go:linkname.
func r2StackStats(out *[7]uintptr) {
	out[0] = stackLastSize
	out[1] = stackPeak
	out[2] = stackAllocated
	out[3] = stackReused
	out[4] = stackExited
	out[5] = uintptr(stackCacheLen)
	out[6] = uintptr(stackCacheLimit)
}

// r2StackCurrent is the calling goroutine's high-water mark so far and its
// stack size, or zeros on the system stack.
func r2StackCurrent(used, size *uintptr) {
	t := Current()
	if t == nil {
		*used, *size = 0, 0

		return
	}

	*used = stackUsed(unsafe.Pointer(t.state.canaryPtr), t.state.size)
	*size = t.state.size
}

// r2SetStackCache sets how many finished stacks are kept for reuse, and
// returns the previous limit.  Lowering it releases the surplus to the
// collector.
func r2SetStackCache(n int) int {
	if n < 0 {
		n = 0
	}
	if n > stackCacheSlots {
		n = stackCacheSlots
	}

	old := stackCacheLimit
	stackCacheLimit = n

	for stackCacheLen > n {
		stackCacheLen--
		stackCache[stackCacheLen] = nil
	}

	return old
}
