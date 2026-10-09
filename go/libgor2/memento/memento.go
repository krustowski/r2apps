// Package memento connects a Go process to a window hosted by Memento.
// The window sends commands and paints app-defined, pointer-free snapshots;
// the Go process owns its model and workers. No framebuffer or input syscalls
// are needed by the child. host.hpp implements the C++ side of this protocol.
package memento

import (
	"errors"
	"strconv"
	"sync/atomic"
	"unsafe"
)

const (
	QueueSize     = 32
	NoBuffer      = 2
	HostTimeoutMS = 10000
)

type ExitReason uint32

const (
	Running ExitReason = iota
	ExitHostClosed
	ExitHostTimeout
	ExitBadQueue
	ExitRuntime
)

var (
	ErrArguments = errors.New("memento: expected --host <shared block address>")
	ErrAddress   = errors.New("memento: invalid shared block address")
	ErrProtocol  = errors.New("memento: unsupported host protocol")
	ErrLayout    = errors.New("memento: snapshot size must be a positive multiple of four, with alignment at most four")
)

// Command is one app-defined operation. Signed arguments can be carried by
// converting Value to int32. The queue has one host writer and one client reader.
type Command struct{ Op, Value uint32 }

// Control is the common header. Only the side named in each comment writes
// those words, using aligned 32-bit atomics after initialization.
type Control struct {
	Magic, Version                         uint32
	ClientBeat, Frame, Front, Exited, Tail uint32 // client writes
	HostBeat, Reading, Quit, Head          uint32 // host writes
}

// Block is allocated and owned by the C++ host on the shared kernel heap.
// S must match the host's snapshot type byte for byte: fixed-size byte arrays
// and 32-bit scalars, without Go pointers, strings, slices, maps or interfaces.
// Give each app its own magic and version and assert its block size/offsets.
// Snapshots are protected by Front/Reading ownership, not individual atomics.
type Block[S any] struct {
	Control
	Commands    [QueueSize]Command
	Snapshots   [2]S
	ExitReason  uint32
	RuntimeText [128]byte
}

// Client owns the child side of a block. Poll, DrainCommands and Publish must
// run on one goroutine. Workers communicate with that goroutine through Go
// channels and never retain pointers into the shared block.
type Client[S any] struct {
	block    *Block[S]
	hostBeat uint32
	hostSeen uint64
	reason   ExitReason
}

// The shared heap Memento hosts allocate blocks from: the 4 MiB from
// 0xC00000, and the extension the kernel adds once they are full, which lies
// below the first GiB (from 0xA000000, or past the tar archive).
const (
	heapStart = 0xc00000
	heapLimit = 0x40000000
)

// HostAddress validates the launch prefix and the complete block's range
// before any shared memory is read. Extra application arguments may follow
// args[2]. The block has to lie where the shared heap can be, and on r2 the
// kernel is asked whether it lies in one of the heap's regions (heapContains).
func HostAddress(args []string, size uintptr) (uintptr, error) {
	if len(args) < 3 || args[1] != "--host" {
		return 0, ErrArguments
	}
	address, err := strconv.ParseUint(args[2], 0, 64)
	if err != nil || address%4 != 0 || address < heapStart || address >= heapLimit || size == 0 ||
		uint64(size) > heapLimit-address || !heapContains(uintptr(address), size) {
		return 0, ErrAddress
	}
	return uintptr(address), nil
}

// Attach connects to the address passed by the host. now is milliseconds
// since boot (libgor2.Ticks). The host must initialize Reading to NoBuffer
// and finish initializing its entire block before launching the child.
func Attach[S any](args []string, magic, version uint32, now uint64) (*Client[S], error) {
	address, err := HostAddress(args, unsafe.Sizeof(Block[S]{}))
	if err != nil {
		return nil, err
	}
	return Connect((*Block[S])(unsafe.Pointer(address)), magic, version, now)
}

// Connect attaches to an already mapped block. On r2, use Attach to validate
// the host address first. Connect also supports in-memory protocol tests.
func Connect[S any](block *Block[S], magic, version uint32, now uint64) (*Client[S], error) {
	var snapshot S
	if unsafe.Sizeof(snapshot) == 0 || unsafe.Sizeof(snapshot)%4 != 0 || unsafe.Alignof(snapshot) > 4 {
		return nil, ErrLayout
	}
	if block == nil || block.Magic != magic || block.Version != version || atomic.LoadUint32(&block.Exited) != 0 {
		return nil, ErrProtocol
	}
	return &Client[S]{block: block, hostBeat: atomic.LoadUint32(&block.HostBeat), hostSeen: now}, nil
}

// Poll beats once, checks window closure, host liveness and queue bounds, and
// returns the reason to stop, or Running. Call it regularly even when idle.
// A stop is sticky; finish workers before Close acknowledges safe deallocation.
func (c *Client[S]) Poll(now uint64) ExitReason {
	if c.block == nil {
		return ExitHostClosed
	}
	if c.reason != Running {
		return c.reason
	}
	b := c.block
	if atomic.LoadUint32(&b.Quit) != 0 {
		return c.stop(ExitHostClosed)
	}
	beat := atomic.LoadUint32(&b.HostBeat)
	if beat != c.hostBeat {
		c.hostBeat, c.hostSeen = beat, now
	} else if now-c.hostSeen > HostTimeoutMS {
		return c.stop(ExitHostTimeout)
	}
	atomic.AddUint32(&b.ClientBeat, 1)
	if atomic.LoadUint32(&b.Head)-atomic.LoadUint32(&b.Tail) > QueueSize {
		return c.stop(ExitBadQueue)
	}
	return Running
}

func (c *Client[S]) stop(reason ExitReason) ExitReason {
	c.reason = reason
	atomic.StoreUint32(&c.block.ExitReason, uint32(reason))
	return reason
}

// DrainCommands handles the commands pending at entry, in order. Commands
// added during a callback wait for the next drain. It returns ExitBadQueue
// without reading any slots if the ring counters are corrupt. Callbacks must
// not Close the client or recursively drain it.
func (c *Client[S]) DrainCommands(handle func(Command)) ExitReason {
	if c.block == nil {
		return ExitHostClosed
	}
	if c.reason != Running {
		return c.reason
	}
	b := c.block
	head, tail := atomic.LoadUint32(&b.Head), atomic.LoadUint32(&b.Tail)
	if head-tail > QueueSize {
		return c.stop(ExitBadQueue)
	}
	for tail != head {
		command := b.Commands[tail%QueueSize]
		tail++
		atomic.StoreUint32(&b.Tail, tail)
		handle(command)
	}
	return Running
}

// Publish copies a snapshot into the available back buffer and publishes it.
// False means the host still leases that buffer, or the client has stopped;
// retry the latest state later. The caller never receives a shared pointer.
func (c *Client[S]) Publish(snapshot *S) bool {
	if c.block == nil || c.reason != Running || snapshot == nil {
		return false
	}
	b := c.block
	front := atomic.LoadUint32(&b.Front)
	if front > 1 {
		return false
	}
	back := 1 - front
	if atomic.LoadUint32(&b.Reading) == back {
		return false
	}
	b.Snapshots[back] = *snapshot
	atomic.StoreUint32(&b.Front, back)
	atomic.AddUint32(&b.Frame, 1)
	return true
}

// RuntimeOutput preserves the first 127 bytes of runtime output and marks
// ExitRuntime. It does not allocate, print or yield, so it can be called from
// libgor2.SetConsoleSink, including when the Go heap is exhausted. A sink sees
// ordinary console output too; hosted apps should reserve it for diagnostics.
func (c *Client[S]) RuntimeOutput(output []byte) {
	if c.block == nil {
		return
	}
	b := c.block
	n := 0
	for n < len(b.RuntimeText)-1 && b.RuntimeText[n] != 0 {
		n++
	}
	for _, ch := range output {
		if n == len(b.RuntimeText)-1 {
			break
		}
		if ch < ' ' {
			ch = ' '
		}
		b.RuntimeText[n] = ch
		n++
	}
	b.RuntimeText[n] = 0
	atomic.StoreUint32(&b.ExitReason, uint32(ExitRuntime))
}

// Close is the final shared-memory operation. It acknowledges that the host
// may free the block. Stop all users of the client first; do not defer Close
// across a panic, because the host still needs the runtime's crash output.
// Repeated Close calls and subsequent methods do not touch the block.
func (c *Client[S]) Close() {
	if c.block == nil {
		return
	}
	b := c.block
	c.block = nil
	atomic.StoreUint32(&b.Exited, 1)
}

// Text writes a zero-terminated string for Memento's single-byte font,
// replacing unsupported Unicode and control characters with '?'.
func Text(dst []byte, s string) {
	clear(dst)
	if len(dst) == 0 {
		return
	}
	n := 0
	for _, r := range s {
		if n == len(dst)-1 {
			break
		}
		if r < 32 || r > 126 {
			r = '?'
		}
		dst[n] = byte(r)
		n++
	}
}

const (
	_ = uint(unsafe.Sizeof(Control{}) - 44)
	_ = uint(44 - unsafe.Sizeof(Control{}))
	_ = uint(unsafe.Sizeof(Command{}) - 8)
	_ = uint(8 - unsafe.Sizeof(Command{}))
)
