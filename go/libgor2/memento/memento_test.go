package memento

import (
	"math"
	"runtime"
	"sync/atomic"
	"testing"
	"unsafe"
)

type testSnapshot struct {
	Count uint32
	Text  [12]byte
}

func connectTest(t *testing.T, now uint64) (*Client[testSnapshot], *Block[testSnapshot]) {
	t.Helper()
	b := &Block[testSnapshot]{Control: Control{Magic: 123, Version: 1, Reading: NoBuffer, HostBeat: 1}}
	c, err := Connect(b, 123, 1, now)
	if err != nil {
		t.Fatal(err)
	}
	return c, b
}

func TestLayout(t *testing.T) {
	var b Block[testSnapshot]
	if unsafe.Offsetof(b.Commands) != 44 || unsafe.Offsetof(b.Snapshots) != 300 ||
		unsafe.Offsetof(b.ExitReason) != 332 || unsafe.Offsetof(b.RuntimeText) != 336 || unsafe.Sizeof(b) != 464 {
		t.Fatal("shared ABI offsets changed")
	}
}

func TestHostAddress(t *testing.T) {
	for _, args := range [][]string{
		nil, {"app.elf"}, {"app.elf", "--host"}, {"app.elf", "--other", "0xc00000"},
		{"app.elf", "--host", "garbage"}, {"app.elf", "--host", "-1"},
		{"app.elf", "--host", "0xbffffc"}, {"app.elf", "--host", "0xc00001"},
		{"app.elf", "--host", "0x1000000"}, {"app.elf", "--host", "0xfffffc"},
		{"app.elf", "--host", "0xffffffffffffffff"},
	} {
		if address, err := HostAddress(args, 4040); err == nil || address != 0 {
			t.Fatalf("accepted %v", args)
		}
	}
	for _, address := range []string{"0xc00000", "0xfff038"} {
		if _, err := HostAddress([]string{"app.elf", "--host", address, "extra"}, 4040); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := HostAddress([]string{"app.elf", "--host", "0xc00000"}, 0); err != ErrAddress {
		t.Fatal(err)
	}
}

func TestConnect(t *testing.T) {
	_, b := connectTest(t, 0)
	for _, values := range [][2]uint32{{0, 1}, {123, 0}} {
		if _, err := Connect(b, values[0], values[1], 0); err != ErrProtocol {
			t.Fatal(err)
		}
	}
	if _, err := Connect[testSnapshot](nil, 123, 1, 0); err != ErrProtocol {
		t.Fatal(err)
	}
	b.Exited = 1
	if _, err := Connect(b, 123, 1, 0); err != ErrProtocol {
		t.Fatal(err)
	}
	if _, err := Connect(&Block[uint64]{}, 0, 0, 0); err != ErrLayout {
		t.Fatal(err)
	}
	if _, err := Connect(&Block[[3]byte]{}, 0, 0, 0); err != ErrLayout {
		t.Fatal(err)
	}
	if _, err := Connect(&Block[struct{}]{}, 0, 0, 0); err != ErrLayout {
		t.Fatal(err)
	}
}

func TestHeartbeatAndCloseRequest(t *testing.T) {
	c, b := connectTest(t, 500)
	if c.Poll(10500) != Running || b.ClientBeat != 1 {
		t.Fatal("timeout before deadline")
	}
	b.HostBeat++
	if c.Poll(10501) != Running {
		t.Fatal("new beat did not reset deadline")
	}
	if c.Poll(20502) != ExitHostTimeout || b.ExitReason != uint32(ExitHostTimeout) || b.Exited != 0 {
		t.Fatal("missing timeout")
	}
	b.HostBeat++
	if c.Poll(20503) != ExitHostTimeout {
		t.Fatal("stop was not sticky")
	}
	c, b = connectTest(t, 0)
	b.Quit = 1
	if c.Poll(20000) != ExitHostClosed || b.ExitReason != uint32(ExitHostClosed) || b.Exited != 0 {
		t.Fatal("close must take precedence over timeout")
	}
}

func TestCommandWrapAndDrainBoundary(t *testing.T) {
	c, b := connectTest(t, 0)
	b.Tail = math.MaxUint32 - 2
	b.Head = b.Tail
	for i := uint32(0); i < QueueSize; i++ {
		b.Commands[b.Head%QueueSize] = Command{Op: i + 1, Value: ^i}
		b.Head++
	}
	var got []Command
	result := c.DrainCommands(func(cmd Command) {
		got = append(got, cmd)
		if len(got) == 1 {
			// The first slot was acknowledged before invoking the callback.
			b.Commands[b.Head%QueueSize] = Command{Op: 999}
			atomic.AddUint32(&b.Head, 1)
		}
	})
	if result != Running || len(got) != QueueSize || b.Head-b.Tail != 1 {
		t.Fatal("drain crossed its entry boundary")
	}
	for i, cmd := range got {
		if cmd.Op != uint32(i+1) || cmd.Value != ^uint32(i) {
			t.Fatalf("command %d: %+v", i, cmd)
		}
	}
	c.DrainCommands(func(cmd Command) {
		if cmd.Op != 999 {
			t.Fatal(cmd)
		}
	})
	if b.Tail != b.Head {
		t.Fatal("did not drain remaining command")
	}
}

func TestBadQueue(t *testing.T) {
	for _, distance := range []uint32{QueueSize + 1, math.MaxUint32} {
		for _, poll := range []bool{false, true} {
			c, b := connectTest(t, 0)
			b.Head = distance
			var reason ExitReason
			if poll {
				reason = c.Poll(1)
			} else {
				reason = c.DrainCommands(func(Command) { t.Fatal("read corrupt ring") })
			}
			if reason != ExitBadQueue || b.ExitReason != uint32(ExitBadQueue) || b.Tail != 0 {
				t.Fatal("bad queue not rejected")
			}
		}
	}
}

func TestSnapshotLease(t *testing.T) {
	c, b := connectTest(t, 0)
	snapshot := testSnapshot{Count: 1}
	if !c.Publish(&snapshot) || b.Front != 1 || b.Frame != 1 || b.Snapshots[1].Count != 1 {
		t.Fatal("first publication")
	}
	// The host still holds the previous front buffer after a newer publication.
	b.Reading = 0
	snapshot.Count = 2
	if c.Publish(&snapshot) || b.Frame != 1 || b.Snapshots[0].Count != 0 {
		t.Fatal("overwrote leased buffer")
	}
	b.Reading = NoBuffer
	if !c.Publish(&snapshot) || b.Front != 0 || b.Frame != 2 || b.Snapshots[0].Count != 2 {
		t.Fatal("publication after lease release")
	}
	b.Front = 2
	if c.Publish(&snapshot) || b.Frame != 2 {
		t.Fatal("accepted invalid front")
	}
	if c.Publish(nil) {
		t.Fatal("accepted nil snapshot")
	}
}

func TestRuntimeOutputAndFinalClose(t *testing.T) {
	c, b := connectTest(t, 0)
	c.RuntimeOutput([]byte("panic:\n"))
	c.RuntimeOutput([]byte("failure\x00"))
	if string(b.RuntimeText[:16]) != "panic: failure \x00" || b.ExitReason != uint32(ExitRuntime) {
		t.Fatalf("%q", b.RuntimeText[:16])
	}
	long := make([]byte, 256)
	for i := range long {
		long[i] = 'x'
	}
	if allocations := testing.AllocsPerRun(10, func() { c.RuntimeOutput(long) }); allocations != 0 {
		t.Fatal("runtime output allocated")
	}
	if b.RuntimeText[127] != 0 {
		t.Fatal("runtime text is not terminated")
	}
	c.Close()
	if b.Exited != 1 {
		t.Fatal("missing acknowledgement")
	}
	before := *b
	c.Close()
	c.RuntimeOutput([]byte("later"))
	if c.Poll(1) != ExitHostClosed || c.DrainCommands(func(Command) { t.Fatal("command after close") }) != ExitHostClosed || c.Publish(&testSnapshot{}) {
		t.Fatal("active after close")
	}
	if *b != before {
		t.Fatal("touched block after final acknowledgement")
	}
}

func TestText(t *testing.T) {
	var b [8]byte
	Text(b[:], "Aé\nlonger")
	if string(b[:]) != "A??long\x00" {
		t.Fatalf("%q", b)
	}
	Text(b[:], "x")
	if string(b[:]) != "x\x00\x00\x00\x00\x00\x00\x00" {
		t.Fatalf("stale text: %q", b)
	}
	Text(nil, "anything")
	Text(b[:1], "anything")
	if b[0] != 0 {
		t.Fatal("one-byte buffer was not terminated")
	}
}

func TestConcurrentCommands(t *testing.T) {
	c, b := connectTest(t, 0)
	const commands = 4000
	produced := make(chan struct{})
	go func() {
		defer close(produced)
		for i := uint32(1); i <= commands; i++ {
			head := atomic.LoadUint32(&b.Head)
			for head-atomic.LoadUint32(&b.Tail) >= QueueSize {
				runtime.Gosched()
			}
			b.Commands[head%QueueSize] = Command{Op: i, Value: ^i}
			atomic.StoreUint32(&b.Head, head+1)
		}
	}()
	next := uint32(1)
	for next <= commands {
		if c.DrainCommands(func(command Command) {
			if command.Op != next || command.Value != ^next {
				t.Errorf("command %d: %+v", next, command)
			}
			next++
		}) != Running {
			t.Fatal("valid concurrent queue rejected")
		}
		runtime.Gosched()
	}
	<-produced
	c.Close()
}

func TestConcurrentSnapshots(t *testing.T) {
	type snapshot struct {
		Sequence uint32
		Data     [128]uint32
	}
	b := &Block[snapshot]{Control: Control{Magic: 123, Version: 1, Reading: NoBuffer}}
	c, err := Connect(b, 123, 1, 0)
	if err != nil {
		t.Fatal(err)
	}
	const frames = 4000
	produced := make(chan struct{})
	go func() {
		defer close(produced)
		var s snapshot
		for sequence := uint32(1); sequence <= frames; sequence++ {
			s.Sequence = sequence
			for i := range s.Data {
				s.Data[i] = sequence ^ uint32(i)
			}
			for !c.Publish(&s) {
				runtime.Gosched()
			}
		}
	}()
	lastFrame := uint32(0)
	lastSequence := uint32(0)
	for lastFrame != frames {
		frame := atomic.LoadUint32(&b.Frame)
		if frame == lastFrame {
			runtime.Gosched()
			continue
		}
		front := atomic.LoadUint32(&b.Front)
		atomic.StoreUint32(&b.Reading, front)
		if atomic.LoadUint32(&b.Front) != front {
			atomic.StoreUint32(&b.Reading, NoBuffer)
			continue
		}
		// Simulate the window being descheduled while it holds its reader lease.
		runtime.Gosched()
		s := b.Snapshots[front]
		atomic.StoreUint32(&b.Reading, NoBuffer)
		for i, value := range s.Data {
			if value != s.Sequence^uint32(i) {
				t.Errorf("torn snapshot at sequence %d, word %d", s.Sequence, i)
				break
			}
		}
		lastFrame, lastSequence = frame, s.Sequence
	}
	<-produced
	if lastSequence != frames {
		t.Fatalf("last snapshot: %d", lastSequence)
	}
	c.Close()
}
