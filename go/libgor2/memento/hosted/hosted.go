// Package hosted is the child side of Memento's HostedWindow, the window
// r2web.elf and jug.elf are shown in (cpp/memento-hello/windows/hosted_window.cpp,
// with the block in cpp/r2web/host.h).  Memento allocates the block on the
// shared heap, starts the program with `--host 0x<address>`, queues the
// window's keys, clicks and size into it, and shows the frames the program
// draws there, one palette index a pixel.  Unlike package memento, nothing
// app-specific is compiled into Memento: the program draws its own window.
//
// Each word has one writer.  Memento writes HostBeat, Quit, Head and the
// commands; the program writes ClientBeat, Tail, the frames, Front, Serial,
// the mailboxes it raises (copy, open, attention), Error and finally Exited.
package hosted

import (
	"errors"
	"sync/atomic"
	"unsafe"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento"
)

const (
	Version       = 1
	NoFrame       = 2 // Front before the first frame
	QueueSize     = 64
	MaxDimension  = 2048
	TextCapacity  = 1200
	TitleCapacity = 96
	HostTimeoutMS = 10000
)

// Command operations.
const (
	OpKey = iota + 1
	OpMouseMove
	OpMouseButton
	OpWheel
	OpResize
)

// PasteImage in a key command's X: Ctrl+V with a picture on Memento's
// clipboard, in a window that asked for pictures.  Text names a PNG of it.
const PasteImage = 1

// Frame slot states.
const (
	slotReady = iota
	slotWriting
	slotReading
)

// Command is one input event.  Text is the clipboard (code page 437, as
// Memento's font) at the time of a Ctrl+V.
type Command struct {
	Op, Flags uint32
	X, Y      int32
	Value     uint32 // a key's character; the right button; the wheel going up
	Extra     uint32 // a key's F number; a button pressed (not released)
	Text      [TextCapacity]byte
}

// Frame describes one of the two pixel buffers that follow the block.
type Frame struct {
	State, Serial, Width, Height uint32
	Title                        [TitleCapacity]byte
}

// Block is r2web::HostBlock, byte for byte.
type Block struct {
	Magic, Version, Capacity, MaxWidth, MaxHeight, Colours, PortBase uint32
	HostBeat, ClientBeat, Quit, Exited, Head, Tail, Front, Serial    uint32
	CopyPending, OpenPending                                         uint32
	CopyText, OpenURL, InitialURL                                    [TextCapacity]byte
	Error                                                            [124]byte
	AttentionPending                                                 uint32
	Frames                                                           [2]Frame
	Commands                                                         [QueueSize]Command
}

// BlockSize is the block without its two pixel buffers.
const BlockSize = unsafe.Sizeof(Block{})

type ExitReason int

const (
	Running ExitReason = iota
	ExitHostClosed
	ExitHostTimeout
	ExitBadQueue
)

var ErrProtocol = errors.New("hosted: not a Memento window block for this program")

// Client is the program's side.  All of it runs on one goroutine.
type Client struct {
	b        *Block
	pixels   [2][]byte
	hostBeat uint32
	hostSeen uint64
	reason   ExitReason

	runtimeSaid bool // RuntimeOutput has begun the message
}

// Attach checks the block at the address Memento passed (args[2]) and
// connects to it.  now is milliseconds since boot (libgor2.Ticks).
func Attach(args []string, magic uint32, now uint64) (*Client, error) {
	address, err := memento.HostAddress(args, BlockSize)
	if err != nil {
		return nil, err
	}
	b := (*Block)(unsafe.Pointer(address))
	capacity := uintptr(atomic.LoadUint32(&b.Capacity))
	if capacity == 0 || capacity > MaxDimension*MaxDimension {
		return nil, ErrProtocol
	}
	// The pixel buffers too must lie in the shared heap.
	if _, err := memento.HostAddress(args, BlockSize+2*capacity); err != nil {
		return nil, err
	}
	pixels := unsafe.Slice((*byte)(unsafe.Add(unsafe.Pointer(b), BlockSize)), 2*capacity)
	return Connect(b, pixels, magic, now)
}

// Connect attaches to a mapped block whose two pixel buffers are pixels.
// Tests build both in ordinary Go memory.
func Connect(b *Block, pixels []byte, magic uint32, now uint64) (*Client, error) {
	if b == nil || b.Magic != magic || b.Version != Version || atomic.LoadUint32(&b.Exited) != 0 ||
		b.MaxWidth == 0 || b.MaxHeight == 0 || b.MaxWidth > MaxDimension || b.MaxHeight > MaxDimension ||
		b.Capacity != b.MaxWidth*b.MaxHeight || len(pixels) < 2*int(b.Capacity) ||
		(b.Colours != 16 && b.Colours != 256) {
		return nil, ErrProtocol
	}
	n := int(b.Capacity)
	return &Client{b: b, pixels: [2][]byte{pixels[:n:n], pixels[n : 2*n : 2*n]},
		hostBeat: atomic.LoadUint32(&b.HostBeat), hostSeen: now}, nil
}

// Colours is 16 (the VGA) or 256 (the graphics kernel's framebuffer).
func (c *Client) Colours() int { return int(c.b.Colours) }

// MaxSize is the largest frame the window can show.
func (c *Client) MaxSize() (int, int) { return int(c.b.MaxWidth), int(c.b.MaxHeight) }

// PortBase is the first of the 32 local TCP ports Memento set aside for this
// window's process.
func (c *Client) PortBase() uint16 { return uint16(c.b.PortBase) }

// Fits says whether a w x h frame can be published.
func (c *Client) Fits(w, h int) bool {
	return w > 0 && h > 0 && w <= int(c.b.MaxWidth) && h <= int(c.b.MaxHeight) && w*h <= int(c.b.Capacity)
}

// Poll beats once and says whether to go on: the window still open, Memento
// still beating.  A stop is sticky.
func (c *Client) Poll(now uint64) ExitReason {
	if c.reason != Running {
		return c.reason
	}
	b := c.b
	atomic.AddUint32(&b.ClientBeat, 1)
	if atomic.LoadUint32(&b.Quit) != 0 {
		c.reason = ExitHostClosed
	} else if beat := atomic.LoadUint32(&b.HostBeat); beat != c.hostBeat {
		c.hostBeat, c.hostSeen = beat, now
	} else if now-c.hostSeen > HostTimeoutMS {
		c.reason = ExitHostTimeout
		c.Fail("Memento heartbeat timed out.")
	}
	return c.reason
}

// Next takes the oldest queued command into cmd; false when there is none.
// An impossible queue stops the client (ExitBadQueue).
func (c *Client) Next(cmd *Command) bool {
	if c.reason != Running {
		return false
	}
	tail, head := atomic.LoadUint32(&c.b.Tail), atomic.LoadUint32(&c.b.Head)
	if head-tail > QueueSize {
		c.reason = ExitBadQueue
		c.Fail("Invalid input queue.")
		return false
	}
	if head == tail {
		return false
	}
	*cmd = c.b.Commands[tail%QueueSize]
	cmd.Text[TextCapacity-1] = 0
	atomic.StoreUint32(&c.b.Tail, tail+1)
	return true
}

// Publish copies a w x h frame of palette indices (rows of w) into the buffer
// Memento is not showing and makes it the one to show.  False when it does
// not fit or Memento is still reading that buffer; try again later.
func (c *Client) Publish(px []byte, w, h int, title string) bool {
	if c.reason != Running || !c.Fits(w, h) || len(px) < w*h {
		return false
	}
	back := uint32(0)
	if atomic.LoadUint32(&c.b.Front) == 0 {
		back = 1
	}
	f := &c.b.Frames[back]
	if !atomic.CompareAndSwapUint32(&f.State, slotReady, slotWriting) {
		return false
	}
	copy(c.pixels[back], px[:w*h])
	f.Width, f.Height = uint32(w), uint32(h)
	putText(f.Title[:], title)
	f.Serial = atomic.LoadUint32(&c.b.Serial) + 1
	atomic.StoreUint32(&f.State, slotReady)
	atomic.StoreUint32(&c.b.Front, back)
	atomic.StoreUint32(&c.b.Serial, f.Serial)
	return true
}

// Copy puts text (code page 437) on Memento's clipboard.  False while the
// last one has not been taken yet.
func (c *Client) Copy(text string) bool {
	if atomic.LoadUint32(&c.b.CopyPending) != 0 {
		return false
	}
	putText(c.b.CopyText[:], text)
	atomic.StoreUint32(&c.b.CopyPending, 1)
	return true
}

// Open asks Memento for a Web window on url.
func (c *Client) Open(url string) bool {
	if atomic.LoadUint32(&c.b.OpenPending) != 0 {
		return false
	}
	putText(c.b.OpenURL[:], url)
	atomic.StoreUint32(&c.b.OpenPending, 1)
	return true
}

// Attention marks the window (a red title and taskbar button) unless it has
// the focus.
func (c *Client) Attention() { atomic.StoreUint32(&c.b.AttentionPending, 1) }

// Fail leaves a message the window shows once the program has ended.
func (c *Client) Fail(msg string) { putText(c.b.Error[:], msg) }

// RuntimeOutput keeps the runtime's own words --- a panic's --- as the
// message the window shows once the program has ended.  It does not
// allocate, so it works with the Go heap exhausted: give it to
// libgor2.SetConsoleSink.
func (c *Client) RuntimeOutput(output []byte) {
	if c.b == nil {
		return
	}
	e := &c.b.Error
	n := 0
	if c.runtimeSaid {
		for n < len(e) && e[n] != 0 {
			n++
		}
	}
	c.runtimeSaid = true
	for _, ch := range output {
		if n >= len(e)-1 {
			break
		}
		if ch < ' ' || ch > '~' {
			ch = ' '
		}
		if ch == ' ' && (n == 0 || e[n-1] == ' ') {
			continue
		}
		e[n] = ch
		n++
	}
	e[n] = 0
}

// Close is the last access to the block: Memento may free it after this.
func (c *Client) Close() {
	if c.b == nil {
		return
	}
	atomic.StoreUint32(&c.b.Exited, 1)
	c.b = nil
	c.pixels = [2][]byte{}
	if c.reason == Running {
		c.reason = ExitHostClosed
	}
}

// Text is a command's text up to its terminator.
func (cmd *Command) TextString() string {
	for i, b := range cmd.Text {
		if b == 0 {
			return string(cmd.Text[:i])
		}
	}
	return string(cmd.Text[:])
}

func putText(dst []byte, s string) {
	n := copy(dst[:len(dst)-1], s)
	dst[n] = 0
}

// Key is a key command decoded: R2WEB_KEY_FLAGS in host.h, bit for bit.
type Key struct {
	Down, Up, IsChar, IsF, Home, End, Insert, PageUp, PageDown, Delete bool
	ArrowUp, ArrowLeft, ArrowRight, ArrowDown                          bool
	LeftControl, RightControl, LeftAlt, RightAlt, Space                bool
	LeftShift, RightShift, Tab, Backspace, Enter, Escape               bool
	Char                                                               byte
	F                                                                  int8
}

func (k *Key) Ctrl() bool  { return k.LeftControl || k.RightControl }
func (k *Key) Alt() bool   { return k.LeftAlt || k.RightAlt }
func (k *Key) Shift() bool { return k.LeftShift || k.RightShift }

// Key decodes an OpKey command.
func (cmd *Command) Key() Key {
	bit := func(n uint) bool { return cmd.Flags&(1<<n) != 0 }
	return Key{
		Down: bit(0), Up: bit(1), IsChar: bit(2), IsF: bit(3), Home: bit(4), End: bit(5), Insert: bit(6),
		PageUp: bit(7), PageDown: bit(8), Delete: bit(9), ArrowUp: bit(10), ArrowLeft: bit(11),
		ArrowRight: bit(12), ArrowDown: bit(13), LeftControl: bit(14), RightControl: bit(15),
		LeftAlt: bit(16), RightAlt: bit(17), Space: bit(18), LeftShift: bit(19), RightShift: bit(20),
		Tab: bit(21), Backspace: bit(22), Enter: bit(23), Escape: bit(24),
		Char: byte(cmd.Value), F: int8(cmd.Extra),
	}
}
