//go:build r2

package main

import (
	"errors"
	"io"
	"sync/atomic"
	"time"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
	"github.com/krustowski/rou2exOS-apps/go/r2tls"
	"github.com/krustowski/rou2exOS-apps/go/telegram/media"
)

// worker owns the network stack: r2net drives it from one goroutine, this
// one.  It takes a job, does it, and hands back the result; the client's
// goroutine runs meanwhile, since the stack yields while it waits.
type worker struct {
	jobs     chan job
	results  chan result
	done     chan struct{}
	stop     uint32 // the job in flight is to stop
	portBase uint16
	stack    *r2net.Stack
	tls      *r2tls.Client
	c        client
}

func newWorker(portBase uint16) *worker {
	w := &worker{jobs: make(chan job, 1), results: make(chan result, 1), done: make(chan struct{}), portBase: portBase}
	w.c = client{dial: w.dial, now: r2.Ticks, alloc: media.Alloc, free: media.Free}
	return w
}

// check runs inside every wait of the stack and the TLS engine.
func (w *worker) check() error {
	if atomic.LoadUint32(&w.stop) != 0 {
		return errStopped
	}
	select {
	case <-w.done:
		return errStopped
	default:
	}
	return nil
}

// cancel stops the job in flight; called from the client's goroutine.
func (w *worker) cancel() { atomic.StoreUint32(&w.stop, 1) }

func (w *worker) run() {
	defer func() {
		w.c.drop()
		if w.stack != nil {
			w.stack.Close()
		}
	}()
	for {
		select {
		case j := <-w.jobs:
			atomic.StoreUint32(&w.stop, 0)
			r := w.c.do(j)
			// A connection that could not be made may be the stack's fault:
			// opened before the Ethernet driver had published its address
			// and DNS server, it never learns them.  The next request opens
			// it again, unless this process is the driver itself.
			if r.err != nil && r.fresh && !errors.Is(r.err, errStopped) && w.stack != nil && !w.stack.CanICMP() {
				w.c.drop()
				w.stack.Close()
				w.stack, w.tls = nil, nil
			}
			select {
			case w.results <- r:
			case <-w.done:
				r.free()
				return
			}
		case <-w.done:
			return
		}
	}
}

func (w *worker) dial(secure bool, host string, port uint16, timeout time.Duration) (conn, error) {
	if w.stack == nil {
		// Four of the 32 ports Memento set aside for this window: one
		// connection at a time, one for DNS, and room for TIME_WAIT.
		s, err := r2net.Open(r2net.Options{Check: w.check, PortBase: w.portBase, PortCount: 4})
		if err != nil {
			return nil, err
		}
		w.stack, w.tls = s, &r2tls.Client{Stack: s, Check: w.check}
	}
	if secure {
		c, err := w.tls.Dial(host, port, timeout)
		if err != nil {
			return nil, err
		}
		return c, nil
	}
	ip, err := w.stack.ResolveTCP(host, 15*time.Second)
	if err != nil {
		return nil, err
	}
	c, err := w.stack.Dial(ip, port, 15*time.Second)
	if err != nil {
		return nil, err
	}
	return &plainConn{c: c, idle: timeout}, nil
}

// plainConn is a TCP connection with an idle timeout rather than a deadline,
// for an http:// test server.
type plainConn struct {
	c    *r2net.Conn
	idle time.Duration
}

func (p *plainConn) SetTimeout(d time.Duration) { p.idle = d }
func (p *plainConn) Close() error               { return p.c.Close() }

func (p *plainConn) Write(b []byte) (int, error) {
	p.c.SetDeadline(p.idle)
	return p.c.Write(b)
}

func (p *plainConn) Read(b []byte) (int, error) {
	p.c.SetDeadline(p.idle)
	n, err := p.c.Read(b)
	if errors.Is(err, r2net.ErrClosed) {
		err = io.EOF
	}
	return n, err
}
