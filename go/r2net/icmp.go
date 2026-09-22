package r2net

import (
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// ICMP: echo requests out, echo replies in, and echo requests answered.
//
// Answering is not optional politeness.  While this program holds the
// Ethernet driver registration it is the only thing on the machine that sees
// an echo request at all, so a machine running it stops replying to ping
// unless this code does it --- and a monitoring tool that makes the host it
// runs on look dead is not much of a monitoring tool.

const (
	icmpEchoReply   = 0
	icmpEchoRequest = 8

	icmpHeaderLen = 8
)

// echoPayload is what Ping sends and expects back.  The reply has to carry the
// request's payload unchanged, so this doubles as a check that the answer is
// the one to our question.
var echoPayload = []byte("r2net echo request")

type echoWaiter struct {
	peer IP
	id   uint16
	seq  uint16

	got bool
	at  uint64
}

// echoSeq counts the requests sent in this process, so two pings in a row
// cannot be confused with each other.
var echoSeq uint16

// Ping sends one ICMP echo request and waits for its reply.
//
// It needs the global Ethernet driver registration, because the kernel routes
// only TCP frames by port and hands everything else --- ICMP included --- to
// the registered driver.  When another process holds it, Ping reports ErrNoICMP
// rather than quietly timing out, which is a different fact about the network.
func (s *Stack) Ping(dst IP, timeout time.Duration) (time.Duration, error) {
	if !s.driver {
		return 0, ErrNoICMP
	}

	echoSeq++

	// The identifier is ours to choose; the clock keeps two runs of the
	// same program from picking the same one.
	w := &echoWaiter{
		peer: dst,
		id:   uint16(libgor2.Ticks()) | 0x8000,
		seq:  echoSeq,
	}

	s.echo = w
	defer func() { s.echo = nil }()

	buf := s.txBuf[ipHeaderLen:]

	buf[0] = icmpEchoRequest
	buf[1] = 0
	buf[2], buf[3] = 0, 0
	buf[4], buf[5] = byte(w.id>>8), byte(w.id)
	buf[6], buf[7] = byte(w.seq>>8), byte(w.seq)

	copy(buf[icmpHeaderLen:], echoPayload)

	msg := buf[:icmpHeaderLen+len(echoPayload)]

	sum := checksum(msg)
	msg[2], msg[3] = byte(sum>>8), byte(sum)

	sent := libgor2.Ticks()

	if err := s.sendIP(dst, protoICMP, len(msg)); err != nil {
		return 0, err
	}

	s.logf("icmp echo request to %v id %d seq %d", dst, w.id, w.seq)

	if err := s.waitUntil(func() bool { return w.got }, deadlineFor(timeout)); err != nil {
		return 0, err
	}

	// The clock this subtraction is made from is the kernel's millisecond
	// tick, so a round trip on a local link reads as 0 or 1 ms.  That is
	// the resolution the machine has.
	return time.Duration(w.at-sent) * time.Millisecond, nil
}

// onICMP answers requests and wakes whoever is waiting for a reply.
func (s *Stack) onICMP(pkt ipPacket) {
	msg := pkt.payload
	if len(msg) < icmpHeaderLen {
		return
	}

	switch msg[0] {
	case icmpEchoRequest:
		s.answerEcho(pkt, msg)

	case icmpEchoReply:
		w := s.echo
		if w == nil || w.got {
			return
		}

		var (
			id  = uint16(msg[4])<<8 | uint16(msg[5])
			seq = uint16(msg[6])<<8 | uint16(msg[7])
		)

		if pkt.src != w.peer || id != w.id || seq != w.seq {
			return
		}

		if !equalBytes(msg[icmpHeaderLen:], echoPayload) {
			return
		}

		w.got = true
		w.at = libgor2.Ticks()
	}
}

// answerEcho turns a request into a reply and sends it back.
func (s *Stack) answerEcho(pkt ipPacket, msg []byte) {
	if len(msg) > len(s.txBuf)-ipHeaderLen {
		return
	}

	buf := s.txBuf[ipHeaderLen:]

	// The payload of a reply is the payload of the request, so the whole
	// message is copied and only the type changes.
	copy(buf, msg)

	buf[0] = icmpEchoReply
	buf[2], buf[3] = 0, 0

	out := buf[:len(msg)]

	sum := checksum(out)
	out[2], out[3] = byte(sum>>8), byte(sum)

	s.logf("icmp echo reply to %v", pkt.src)

	_ = s.sendIP(pkt.src, protoICMP, len(out))
}

func equalBytes(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}

	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}

	return true
}
