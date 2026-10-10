//go:build r2abimock && !r2

package r2net

import (
	"bytes"
	"testing"
	"time"
)

// uploadLink models a routed connection with delayed ACKs and a smaller MTU.
// Oversized DF packets disappear, as they do when ICMP cannot reach this app.
type uploadLink struct {
	c          *Conn
	now        uint64
	maxPayload int
	delay      uint64
	packets    [][]byte
	got        []byte
	acks       []uint32
	blackhole  bool
	partialACK bool
}

func (l *uploadLink) send(p []byte, _ IP) error {
	tcp := p[ipHeaderLen:]
	n := int(tcp[12]>>4) * 4
	data := tcp[n:]
	if len(data) == 0 {
		return nil
	}
	l.packets = append(l.packets, append([]byte(nil), data...))
	if l.blackhole || len(data) > l.maxPayload {
		return nil
	}
	l.got = append(l.got, data...)
	seq := uint32From(tcp[4:8])
	if l.partialACK {
		l.acks = append(l.acks, seq+uint32(len(data)/4), seq+uint32(len(data)/2), seq+uint32(len(data)/2))
	}
	l.acks = append(l.acks, seq+uint32(len(data)))
	return nil
}

func (l *uploadLink) recv([]byte) int {
	l.now += l.delay
	if len(l.acks) > 0 {
		ack := l.acks[0]
		l.acks = l.acks[1:]
		l.c.onAck(ack)
	}
	return -1
}
func (*uploadLink) name() string { return "upload" }

func uploadConn(t *testing.T, payload int) (*Conn, *uploadLink) {
	t.Helper()
	l := &uploadLink{maxPayload: payload, delay: 4}
	previous := ticksNow
	ticksNow = func() uint64 { return l.now }
	t.Cleanup(func() { ticksNow = previous })
	s := &Stack{link: l}
	c := &Conn{s: s, state: stateEstablished, snd: 100, sndUna: 100, peerMSS: mss}
	l.c, s.conns = c, []*Conn{c}
	return c, l
}

func TestUploadThroughSmallerMTU(t *testing.T) {
	c, l := uploadConn(t, 1024)
	c.SetDeadline(time.Second)
	body := bytes.Repeat([]byte("screenshot"), 7000)
	if n, err := c.Write(body); err != nil || n != len(body) {
		t.Fatalf("upload: %d/%d bytes, %v", n, len(body), err)
	}
	if !bytes.Equal(l.got, body) {
		t.Fatal("upload was truncated or corrupted")
	}
}

func TestUploadRespectsPeerMSS(t *testing.T) {
	c, l := uploadConn(t, 700)
	c.state, c.snd = stateSynSent, 101
	tcp := make([]byte, 28)
	tcp[12], tcp[13] = 7<<4, flagSYN|flagACK
	putUint32(tcp[4:8], 900)
	putUint32(tcp[8:12], 101)
	// NOPs before the MSS and EOL after it.
	copy(tcp[20:], []byte{1, 1, 2, 4, 2, 188, 0, 0})
	c.lport, c.rport = 1234, 443
	tcp[0], tcp[1], tcp[2], tcp[3] = 1, 187, 4, 210
	c.s.onTCP(ipPacket{payload: tcp})
	c.SetDeadline(time.Second)
	body := bytes.Repeat([]byte{42}, 8192)
	if n, err := c.Write(body); err != nil || n != len(body) {
		t.Fatalf("peer MSS: %d bytes, %v", n, err)
	}
	if !bytes.Equal(l.got, body) {
		t.Fatal("peer did not receive the whole upload")
	}
}

func TestUploadIdleTimeout(t *testing.T) {
	c, l := uploadConn(t, mss)
	body := bytes.Repeat([]byte{42}, 70*1024)
	c.SetTimeout(10 * time.Millisecond)
	if n, err := c.Write(body); err != nil || n != len(body) {
		t.Fatalf("active upload timed out: %d/%d bytes, %v", n, len(body), err)
	}
	if l.now <= 10 || !bytes.Equal(l.got, body) {
		t.Fatal("did not test a long, complete upload")
	}
	// No ACKs: an idle timeout still terminates the write.
	l.blackhole = true
	start := l.now
	if _, err := c.Write(body); err != ErrTimeout || l.now-start > 16 {
		t.Fatalf("stalled upload: %v, %d ms", err, l.now-start)
	}
}

func TestUploadAbsoluteDeadline(t *testing.T) {
	c, _ := uploadConn(t, mss)
	c.SetTimeout(time.Second)
	c.SetDeadline(10 * time.Millisecond)
	if _, err := c.Write(make([]byte, 70*1024)); err != ErrTimeout {
		t.Fatalf("absolute deadline: %v", err)
	}
}

func TestUploadPartialACKProgress(t *testing.T) {
	c, l := uploadConn(t, mss)
	l.partialACK = true
	c.SetTimeout(10 * time.Millisecond)
	if n, err := c.Write(make([]byte, 1024)); err != nil || n != 1024 {
		t.Fatalf("partial ACK progress: %d bytes, %v", n, err)
	}
}

func TestUploadDoesNotExtendTimeoutForInvalidACK(t *testing.T) {
	c, l := uploadConn(t, mss)
	c.SetTimeout(10 * time.Millisecond)
	l.now = 8
	for _, ack := range []uint32{99, 100, 101} {
		c.onAck(ack)
	}
	if c.deadline != 10 || c.sndUna != 100 {
		t.Fatal("invalid/duplicate ACK kept an idle connection alive")
	}
}

func TestPeerMSSOptions(t *testing.T) {
	for _, tt := range []struct {
		options []byte
		want    int
	}{
		{nil, 536}, {[]byte{0, 2, 4, 1, 0}, 536},
		{[]byte{1, 2, 4, 2, 188}, 700}, {[]byte{2, 4, 0, 0}, 536},
		{[]byte{2, 0}, 536}, {[]byte{2, 4, 1}, 536},
		{[]byte{3, 3, 7, 2, 4, 5, 180}, 1460},
	} {
		if got := tcpPeerMSS(tt.options); got != tt.want {
			t.Errorf("%v: %d, want %d", tt.options, got, tt.want)
		}
	}
}
