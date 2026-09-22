package r2net

import "github.com/krustowski/rou2exOS-apps/go/libgor2"

// ticksNow is the kernel's millisecond clock, which is what every deadline in
// this package is measured against.  It is the PIT tick at 1 kHz, so it moves
// once per millisecond and no faster --- there is no finer clock to be had
// from userland here.
func ticksNow() uint64 {
	return libgor2.Ticks()
}
