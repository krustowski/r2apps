//go:build r2 && r2netcheck

package r2net

import (
	"errors"
	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"sync/atomic"
	"time"
)

// This link is used only by the native regression build. No NIC is required.
type checkLink struct{ window, sent int }

func (l *checkLink) send(p []byte, _ IP) error {
	l.window = int(p[34])<<8 | int(p[35])
	l.sent++
	return nil
}
func (*checkLink) recv([]byte) int { return -1 }
func (*checkLink) name() string    { return "regression" }
func NativeTCPCheck() error {
	l := &checkLink{}
	s := &Stack{link: l}
	c := &Conn{s: s, state: stateEstablished, rcv: 100, rx: make([]byte, 0, rxCap)}
	s.conns = []*Conn{c}
	c.onData(100, make([]byte, rxCap))
	if l.window != 0 {
		return errors.New("TCP window did not close")
	}
	var buf [4096]byte
	if n, e := c.Read(buf[:]); e != nil || n != len(buf) || l.window != 4096 {
		return errors.New("TCP window did not reopen")
	}
	sent := l.sent
	c.tick(c.windowRetryAt)
	if l.sent != sent+1 {
		return errors.New("TCP did not repeat its window update")
	}
	next := c.rcv
	c.onData(next-3, []byte{0, 0, 0, 1, 2, 3, 4})
	if c.rcv != next+4 {
		return errors.New("TCP overlap lost its suffix")
	}
	c.rx = c.rx[:0]
	seq := c.rcv
	c.onData(seq+4, []byte("tail"))
	c.onData(seq, []byte("head"))
	if string(c.rx) != "headtail" || c.reorder.bytes != 0 {
		return errors.New("TCP reordered burst lost data")
	}
	c.rx = c.rx[:0]
	var heartbeats uint32
	done := make(chan struct{})
	exited := make(chan struct{})
	go func() {
		defer close(exited)
		for {
			select {
			case <-done:
				return
			default:
			}
			atomic.AddUint32(&heartbeats, 1)
			time.Sleep(time.Millisecond)
		}
	}()
	c.SetDeadline(40 * time.Millisecond)
	_, e := c.Read(buf[:]) // A quiet TCP read must let another Go task keep running.
	close(done)
	<-exited
	if e != ErrTimeout || atomic.LoadUint32(&heartbeats) < 2 {
		return errors.New("TCP wait starved Go tasks")
	}
	cancelled := errors.New("cancelled")
	s.check = func() error { return cancelled }
	if _, e = c.Read(buf[:]); e != cancelled {
		return errors.New("TCP cancellation failed")
	}
	c.rx = make([]byte, 4096)
	c.reorder.store(c.rcv, c.rcv+2, []byte("retained"), rxCap)
	c.drop()
	if len(s.conns) != 0 || s.conns[:cap(s.conns)][0] != nil || c.rx != nil || c.reorder.bytes != 0 {
		return errors.New("closed TCP connection retained buffers")
	}
	_ = libgor2.Ticks() // The regression runs with the real r2 scheduler/clock.
	return nil
}
