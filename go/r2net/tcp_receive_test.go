package r2net

import (
	"bytes"
	"testing"
)

func TestReceiveWindowReopens(t *testing.T) {
	for _, x := range []struct {
		old, now int
		want     bool
	}{{0, 2048, true}, {0, 1, true}, {100, 100, false}, {100, 1560, true}, {16000, 16384, false}, {4096, 2048, false}} {
		if tcpWindowNeedsUpdate(x.old, x.now) != x.want {
			t.Fatalf("window %d -> %d", x.old, x.now)
		}
	}
}
func TestOverlappingRetransmissionAfterFullWindow(t *testing.T) {
	segment := []byte("abcdefgh")
	first := tcpPayload(100, 100, segment, 3)
	second := tcpPayload(103, 100, segment, 10)
	if !bytes.Equal(append(append([]byte(nil), first...), second...), segment) {
		t.Fatal("overlapping retransmission lost data")
	}
	if len(tcpPayload(100, 101, segment, 8)) != 0 || len(tcpPayload(108, 100, segment, 8)) != 0 {
		t.Fatal("gap or duplicate accepted")
	}
	if got := tcpPayload(1, 0xfffffffe, segment, 8); string(got) != "defgh" {
		t.Fatalf("sequence wrap: %q", got)
	}
}

func TestReorderedReceiveBurst(t *testing.T) {
	var q tcpReorder
	q.store(100, 108, []byte("ijkl"), 12)
	q.store(100, 104, []byte("efgh"), 12)
	q.store(100, 104, []byte("efgh"), 12) // A retransmission must not consume capacity twice.
	q.store(100, 103, []byte("overlap"), 12)
	if q.bytes != 8 || q.take(100) != nil {
		t.Fatal("gap or duplicate mishandled")
	}
	if got := q.take(104); string(got) != "efgh" {
		t.Fatalf("first suffix: %q", got)
	}
	if got := q.take(108); string(got) != "ijkl" {
		t.Fatalf("second suffix: %q", got)
	}
	if q.bytes != 0 {
		t.Fatal("receive queue leaked bytes")
	}
}
func TestReorderBoundsAndWrap(t *testing.T) {
	var q tcpReorder
	q.store(0xfffffffe, 1, []byte("abc"), 5)
	if q.bytes != 2 {
		t.Fatal("receive right edge exceeded")
	}
	if got := q.take(2); string(got) != "b" {
		t.Fatalf("wrap overlap: %q", got)
	}
	q.store(10, 10, []byte("same"), 4)
	q.store(10, 14, []byte("outside"), 4)
	if q.bytes != 0 {
		t.Fatal("in-order or out-of-window segment retained")
	}
	q.store(10, 11, []byte("abc"), 4)
	if q.take(14) != nil || q.bytes != 0 {
		t.Fatal("obsolete bytes not discarded")
	}
}
