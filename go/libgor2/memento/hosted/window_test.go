package hosted

import (
	"bytes"
	"runtime"
	"sync/atomic"
	"testing"
)

func TestPaintConcurrentPresentation(t *testing.T) {
	b, pixels := newBlock(64, 32)
	c, _ := Connect(b, pixels, b.Magic, 0)
	const total = 2000
	done := make(chan bool, 1)
	go func() {
		valid := true
		for {
			front := atomic.LoadUint32(&b.Front)
			if front >= 2 || !atomic.CompareAndSwapUint32(&b.Frames[front].State, slotReady, slotReading) {
				runtime.Gosched()
				continue
			}
			frame := &b.Frames[front]
			serial := frame.Serial
			for _, pixel := range pixels[int(front)*64*32 : (int(front)+1)*64*32] {
				if pixel != byte(serial%16) {
					valid = false
				}
			}
			atomic.StoreUint32(&frame.State, slotReady)
			if serial == total {
				done <- valid
				return
			}
		}
	}()
	for serial := 1; serial <= total; serial++ {
		for !c.Paint(64, 32, "Concurrent", func(canvas *Canvas) { canvas.Clear(byte(serial % 16)) }) {
			runtime.Gosched()
		}
	}
	if !<-done {
		t.Fatal("host observed a partial frame")
	}
}

func TestPaintLeaseAndClipping(t *testing.T) {
	b, pixels := newBlock(8, 4)
	c, _ := Connect(b, pixels, b.Magic, 0)
	paints := 0
	draw := func(canvas *Canvas) {
		paints++
		if b.Frames[0].State != slotWriting || b.Front != NoFrame {
			t.Fatal("paint was visible before publication or did not hold a lease")
		}
		canvas.Clear(White)
		canvas.FillRect(Rect{-2, -1, 4, 3}, Blue)
		canvas.FillRect(Rect{7, 3, 20, 20}, Black)
		canvas.FillRect(Rect{0, 0, -1, 3}, Black)
	}
	b.Frames[0].State = slotReading
	if c.Paint(8, 4, "busy", draw) || paints != 0 {
		t.Fatal("paint used the host's leased frame")
	}
	b.Frames[0].State = slotReady
	if !c.Paint(8, 4, "Hello", draw) || paints != 1 || b.Serial != 1 || b.Front != 0 {
		t.Fatal("frame not published")
	}
	for y := 0; y < 4; y++ {
		for x := 0; x < 8; x++ {
			want := byte(White)
			if x < 2 && y < 2 {
				want = Blue
			}
			if x == 7 && y == 3 {
				want = Black
			}
			if got := pixels[y*8+x]; got != want {
				t.Fatalf("pixel (%d,%d): %d, want %d", x, y, got, want)
			}
		}
	}
	if len(c.canvas.pixels) != 0 || b.Frames[0].State != slotReady || string(b.Frames[0].Title[:6]) != "Hello\x00" {
		t.Fatal("canvas lease or frame metadata not finalized")
	}
	if !bytes.Equal(pixels[32:], make([]byte, 32)) {
		t.Fatal("paint crossed into the other frame")
	}
	if c.Paint(9, 4, "oversize", draw) || c.Paint(8, 4, "nil", nil) {
		t.Fatal("invalid paint accepted")
	}
}

func TestTextAndButtonClipping(t *testing.T) {
	pixels := make([]byte, 40*24+16)
	for i := 40 * 24; i < len(pixels); i++ {
		pixels[i] = 99
	}
	c := Canvas{pixels: pixels[:40*24], w: 40, h: 24}
	c.Clear(LightGrey)
	c.Text(Rect{-3, -4, 7, 8}, "AB", Black)
	c.Button(Rect{30, 16, 30, 24}, "Hello", false)
	for _, p := range pixels[40*24:] {
		if p != 99 {
			t.Fatal("drawing overran its frame")
		}
	}
	if pixels[16*40+30] != White || pixels[23*40+30] != White {
		t.Fatal("button bevel not clipped to frame")
	}
	// The A glyph from Memento's font is clipped at the rectangle, rather
	// than overwriting the row or drawing its second glyph outside it.
	for y := 0; y < 4; y++ {
		for x := 4; x < 30; x++ {
			if pixels[y*40+x] != LightGrey {
				t.Fatal("text escaped its clip")
			}
		}
	}
}

func TestWindowHooksResizeAndRetry(t *testing.T) {
	b, pixels := newBlock(64, 32)
	c, _ := Connect(b, pixels, b.Magic, 0)
	paints, events, idle := 0, 0, 0
	w, err := newWindow(c, Options{Width: 100, Height: 50, Title: "Hello"}, Hooks{
		OnOpen: func(w *Window) { w.SetTitle("Opened") },
		OnEvent: func(w *Window, cmd *Command) {
			events++
			if cmd.Op == OpResize {
				if x, y := w.Size(); x != 32 || y != 16 {
					t.Fatal("event ran before resize")
				}
			}
		},
		OnPaint: func(w *Window, canvas *Canvas) {
			paints++
			if x, y := canvas.Size(); x != w.width || y != w.height {
				t.Fatal("paint size differs from window")
			}
			canvas.FillRect(Rect{0, 0, 1, 1}, Blue)
		},
		OnIdle: func(w *Window, now uint64) { idle++ },
	})
	if err != nil {
		t.Fatal(err)
	}
	if x, y := w.Size(); x != 64 || y != 32 {
		t.Fatal("initial size not clamped")
	}
	if w.step(1) != Running || paints != 1 || idle != 1 {
		t.Fatal("first paint missing")
	}
	if w.step(2) != Running || paints != 1 {
		t.Fatal("clean window repainted")
	}
	b.Commands[0] = Command{Op: OpResize, X: 65, Y: 16} // invalid: ignored
	b.Commands[1] = Command{Op: OpResize, X: 32, Y: 16}
	b.Head = 2
	b.Frames[1].State = slotReading
	if w.step(3) != Running || events != 1 || paints != 1 || !w.dirty {
		t.Fatal("resize / busy frame handling")
	}
	b.Frames[1].State = slotReady
	if w.step(4) != Running || paints != 2 || w.dirty || b.Frames[1].Width != 32 {
		t.Fatal("resize was not retried")
	}
	w.SetTitle("Renamed")
	if w.step(5) != Running || paints != 3 || string(b.Frames[0].Title[:8]) != "Renamed\x00" {
		t.Fatal("title did not request paint")
	}
	w.Close()
	if w.step(6) != ExitHostClosed || paints != 3 {
		t.Fatal("close did not stop hooks")
	}
	if _, err := newWindow(c, Options{Width: 0, Height: 1}, Hooks{}); err != ErrWindowSize {
		t.Fatal("invalid size accepted")
	}
}

func TestWindowStopsBeforePainting(t *testing.T) {
	for _, stop := range []string{"quit", "timeout", "queue", "event"} {
		t.Run(stop, func(t *testing.T) {
			b, pixels := newBlock(4, 4)
			c, _ := Connect(b, pixels, b.Magic, 0)
			w, _ := newWindow(c, Options{Width: 4, Height: 4}, Hooks{
				OnEvent: func(w *Window, _ *Command) { w.Close() },
				OnPaint: func(*Window, *Canvas) { t.Fatal("paint after stop") },
			})
			now, reason := uint64(1), ExitHostClosed
			switch stop {
			case "quit":
				b.Quit = 1
			case "timeout":
				now, reason = HostTimeoutMS+1, ExitHostTimeout
			case "queue":
				b.Head, reason = QueueSize+1, ExitBadQueue
			case "event":
				b.Commands[0].Op, b.Head = OpKey, 1
			}
			if got := w.step(now); got != reason {
				t.Fatalf("reason %d, want %d", got, reason)
			}
		})
	}
}

func TestPaintDoesNotAllocateAndClosedClient(t *testing.T) {
	b, pixels := newBlock(8, 4)
	c, _ := Connect(b, pixels, b.Magic, 0)
	draw := func(canvas *Canvas) { canvas.Clear(Blue) }
	if allocations := testing.AllocsPerRun(100, func() { c.Paint(8, 4, "Hello", draw) }); allocations != 0 {
		t.Fatalf("paint allocated %.0f objects per frame", allocations)
	}
	c.Close()
	if c.Paint(8, 4, "closed", draw) || c.Copy("closed") || c.Open("about:home") || c.Fits(1, 1) || c.Colours() != 0 || c.PortBase() != 0 {
		t.Fatal("closed client used shared memory")
	}
	c.Attention()
	c.Fail("closed")
	if x, y := c.MaxSize(); x != 0 || y != 0 {
		t.Fatal("closed size")
	}
}

func TestLeftPressed(t *testing.T) {
	r := Rect{2, 3, 4, 5}
	cmd := Command{Op: OpMouseButton, X: 2, Y: 3, Extra: 1}
	if !cmd.LeftPressed(r) {
		t.Fatal("left press missed")
	}
	for _, c := range []Command{
		{Op: OpMouseButton, X: 6, Y: 3, Extra: 1},
		{Op: OpMouseButton, X: 2, Y: 8, Extra: 1},
		{Op: OpMouseButton, X: 2, Y: 3, Extra: 1, Value: 1},
		{Op: OpMouseButton, X: 2, Y: 3},
		{Op: OpMouseMove, X: 2, Y: 3, Extra: 1},
	} {
		if c.LeftPressed(r) {
			t.Fatal("non-activation accepted")
		}
	}
}
