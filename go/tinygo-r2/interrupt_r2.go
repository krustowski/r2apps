//go:build r2

package interrupt

// r2 programs run in ring 3 with no interrupt handlers of their own: cli and
// sti would fault, and there is nothing they could protect against anyway.
// The kernel does preempt us on the PIT, but that is a full context switch
// that restores every general-purpose register, not a callback into Go code,
// so a critical section here has nothing to exclude.  The cooperative
// scheduler only ever switches goroutines at explicit yield points.

// State represents the previous global interrupt state.
type State uintptr

// Disable would disable interrupts on a target that had any to disable.
func Disable() (state State) {
	return 0
}

// Restore undoes a Disable.
func Restore(state State) {}

// In reports whether the system is currently in an interrupt handler.
func In() bool {
	return false
}
