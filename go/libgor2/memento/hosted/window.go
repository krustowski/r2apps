package hosted

import "errors"

// Options identifies the host protocol and the initial client size in pixels.
// Magic must match the C++ HostedWindow wrapper. The host supplies subsequent
// sizes; an initial size larger than its screen is clamped to fit.
type Options struct {
	Magic         uint32
	Title         string
	Width, Height int
}

// Hooks run on the window goroutine. OnEvent receives decoded transport
// commands after resizing. OnPaint draws only when a frame can be leased.
// OnIdle may call Repaint after changing app state. OnClose runs before the
// final shutdown acknowledgement, so it can stop workers or persist state.
type Hooks struct {
	OnOpen  func(*Window)
	OnEvent func(*Window, *Command)
	OnPaint func(*Window, *Canvas)
	OnIdle  func(*Window, uint64)
	OnClose func(*Window, ExitReason)
}

// Window wraps a hosted Client with input/paint hooks and automatic resize,
// heartbeat, retry and shutdown handling. Use Run to drive it on r2.
type Window struct {
	client         *Client
	hooks          Hooks
	width, height  int
	title          string
	dirty, closing bool
	command        Command
}

var ErrWindowSize = errors.New("hosted: initial window size must be positive")

func newWindow(client *Client, options Options, hooks Hooks) (*Window, error) {
	if options.Width <= 0 || options.Height <= 0 {
		return nil, ErrWindowSize
	}
	mw, mh := client.MaxSize()
	w := &Window{client: client, hooks: hooks, width: min(options.Width, mw),
		height: min(options.Height, mh), title: options.Title, dirty: true}
	if hooks.OnOpen != nil {
		hooks.OnOpen(w)
	}
	return w, nil
}

func (w *Window) Size() (int, int) { return w.width, w.height }
func (w *Window) Repaint()         { w.dirty = true }
func (w *Window) SetTitle(title string) {
	if w.title != title {
		w.title, w.dirty = title, true
	}
}

// Close requests shutdown after the current hook returns.
func (w *Window) Close()                  { w.closing = true }
func (w *Window) Colours() int            { return w.client.Colours() }
func (w *Window) PortBase() uint16        { return w.client.PortBase() }
func (w *Window) Copy(text string) bool   { return w.client.Copy(text) }
func (w *Window) Open(url string) bool    { return w.client.Open(url) }
func (w *Window) Notify(text string) bool { return w.client.Notify(text) }
func (w *Window) Notifications() bool     { return w.client.Notifications() }
func (w *Window) Attention()              { w.client.Attention() }

// Fail stops the app and leaves a message in its window.
func (w *Window) Fail(message string) {
	w.client.Fail(message)
	w.Close()
}

func (w *Window) paint(canvas *Canvas) {
	canvas.Clear(LightGrey)
	if w.hooks.OnPaint != nil {
		w.hooks.OnPaint(w, canvas)
	}
}

func (w *Window) step(now uint64) ExitReason {
	if w.closing {
		return ExitHostClosed
	}
	if reason := w.client.Poll(now); reason != Running {
		return reason
	}
	// Bound a turn even when the host keeps adding input while it is drained.
	for n := 0; n < QueueSize && !w.closing && w.client.Next(&w.command); n++ {
		cmd := &w.command
		if cmd.Op == OpResize {
			if !w.client.Fits(int(cmd.X), int(cmd.Y)) {
				continue
			}
			w.width, w.height = int(cmd.X), int(cmd.Y)
			w.dirty = true
		}
		if w.hooks.OnEvent != nil {
			w.hooks.OnEvent(w, cmd)
			w.dirty = true
		}
	}
	if w.client.reason != Running {
		return w.client.reason
	}
	if !w.closing && w.hooks.OnIdle != nil {
		w.hooks.OnIdle(w, now)
	}
	if w.closing {
		return ExitHostClosed
	}
	if w.dirty {
		w.dirty = false
		if !w.client.Paint(w.width, w.height, w.title, w.paint) {
			w.dirty = true
		}
	}
	return Running
}
