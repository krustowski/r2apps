package hosted

import (
	"strings"
	"testing"
	"unsafe"
)

// The sizes cpp/r2web/host.h asserts.
func TestLayout(t *testing.T) {
	if s := unsafe.Sizeof(Command{}); s != 1224 {
		t.Fatalf("Command is %d bytes", s)
	}
	if s := unsafe.Sizeof(Frame{}); s != 112 {
		t.Fatalf("Frame is %d bytes", s)
	}
	if BlockSize != 82356 {
		t.Fatalf("Block is %d bytes", BlockSize)
	}
	var b Block
	if off := unsafe.Offsetof(b.AttentionPending); off != 3792 {
		t.Fatalf("attentionPending at %d", off)
	}
	if off := unsafe.Offsetof(b.Commands); off != 4020 {
		t.Fatalf("commands at %d", off)
	}
}

func newBlock(w, h uint32) (*Block, []byte) {
	b := &Block{Magic: 0x31474554, Version: Version, Capacity: w * h, MaxWidth: w, MaxHeight: h,
		Colours: 16, PortBase: 48032, Front: NoFrame, HostBeat: 1}
	return b, make([]byte, 2*w*h)
}

func TestFramesAndQueue(t *testing.T) {
	b, px := newBlock(8, 4)
	c, err := Connect(b, px, 0x31474554, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := Connect(b, px, 1, 0); err == nil {
		t.Fatal("wrong magic accepted")
	}
	frame := make([]byte, 6*3)
	for i := range frame {
		frame[i] = byte(i)
	}
	if !c.Publish(frame, 6, 3, "Telegram") || b.Front != 0 || b.Serial != 1 || b.Frames[0].Width != 6 {
		t.Fatalf("first frame: front %d serial %d", b.Front, b.Serial)
	}
	if px[17] != 17 || px[18] != 0 {
		t.Fatal("pixels not in slot 0")
	}
	b.Frames[1].State = slotReading
	if c.Publish(frame, 6, 3, "x") {
		t.Fatal("published into a buffer being read")
	}
	b.Frames[1].State = slotReady
	if !c.Publish(frame, 6, 3, "x") || b.Front != 1 || px[32+5] != 5 {
		t.Fatal("second frame not in slot 1")
	}
	if c.Publish(frame, 9, 2, "x") {
		t.Fatal("published a frame wider than the window")
	}

	var cmd Command
	if c.Next(&cmd) {
		t.Fatal("command from an empty queue")
	}
	b.Commands[0] = Command{Op: OpKey, Flags: 1<<0 | 1<<2 | 1<<14, Value: 'v'}
	copy(b.Commands[0].Text[:], "hello")
	b.Head = 1
	if !c.Next(&cmd) || b.Tail != 1 {
		t.Fatal("command not taken")
	}
	k := cmd.Key()
	if !k.Down || !k.IsChar || !k.Ctrl() || k.Char != 'v' || cmd.TextString() != "hello" {
		t.Fatalf("key %+v text %q", k, cmd.TextString())
	}
	b.Head = 100
	if c.Next(&cmd) || c.Poll(0) != ExitBadQueue {
		t.Fatal("impossible queue accepted")
	}
}

func TestLiveness(t *testing.T) {
	b, px := newBlock(4, 4)
	c, _ := Connect(b, px, 0x31474554, 0)
	if c.Poll(5000) != Running || b.ClientBeat != 1 {
		t.Fatal("not running")
	}
	b.HostBeat++
	if c.Poll(14000) != Running {
		t.Fatal("a beating host timed out")
	}
	if c.Poll(24001) != ExitHostTimeout {
		t.Fatal("a silent host did not time out")
	}
	b2, px2 := newBlock(4, 4)
	c2, _ := Connect(b2, px2, 0x31474554, 0)
	if !c2.Copy("one") || c2.Copy("two") || string(b2.CopyText[:4]) != "one\x00" {
		t.Fatal("copy mailbox")
	}
	b2.Quit = 1
	if c2.Poll(1) != ExitHostClosed {
		t.Fatal("quit ignored")
	}
	c2.Close()
	if b2.Exited != 1 {
		t.Fatal("exit not acknowledged")
	}
}

// A panic's words, printed a piece at a time, become the window's message.
func TestRuntimeOutput(t *testing.T) {
	b, px := newBlock(4, 4)
	c, _ := Connect(b, px, 0x31474554, 0)
	c.Fail("Memento heartbeat timed out.")
	c.RuntimeOutput([]byte("panic: runtime error: "))
	c.RuntimeOutput([]byte("out of memory\n\n"))
	got := string(b.Error[:])
	if i := strings.IndexByte(got, 0); i >= 0 {
		got = got[:i]
	}
	if got != "panic: runtime error: out of memory " {
		t.Fatalf("message %q", got)
	}
	long := make([]byte, 300)
	for i := range long {
		long[i] = 'x'
	}
	c.RuntimeOutput(long)
	if b.Error[len(b.Error)-1] != 0 {
		t.Fatal("message not terminated")
	}
}
