//go:build !r2

package memento

// heapContains has no kernel to ask off r2: ordinary Go tests take the range
// HostAddress checks itself, and may put a kernel's answer in its place.
var heapContains = func(address, size uintptr) bool { return true }
