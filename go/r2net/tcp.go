package r2net

import (
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// TCP, client side.
//
// What is here is the smallest thing that can hold a conversation with a real
// server and notice when it cannot: a three-way handshake, in-order data with
// cumulative acknowledgements, one outstanding segment at a time, and a close.
// What is not here is everything that makes TCP fast --- no window beyond one
// segment, no fast retransmit, no congestion control, no selective
// acknowledgement, no reassembly of segments that arrive out of order (they
// are dropped and re-acknowledged, which makes the peer send them again in
// order).
//
// That is the right trade for what this machine does.  A monitoring check is a
// few hundred bytes out and a few kilobytes back; stop-and-wait costs it one
// extra round trip and saves a send queue, a retransmission list and the
// timers that go with them, in a program whose whole heap is 1.5 MiB.

const (
	flagFIN = 0x01
	flagSYN = 0x02
	flagRST = 0x04
	flagPSH = 0x08
	flagACK = 0x10
)

const (
	tcpHeaderLen = 20

	// mss is what we tell the peer it may send us in one segment: a full
	// Ethernet frame's worth, 1514 bytes on the wire, which fits the
	// kernel's 2048-byte frame buffers with room to spare.
	mss = 1460

	// rxCap is how much unread data a connection will hold.  The window we
	// advertise is whatever is left of it, so a peer that respects the
	// window cannot overrun us.  A full window is about eleven segments,
	// well inside the 64 frames the kernel will queue for one process.
	rxCap = 16 * 1024

	// ephemeralBase is where local port numbers start.  Ports below it are
	// where servers live, and on this machine the port registry the kernel
	// keeps is only sixteen entries long, so a program that wants to be
	// runnable twice should keep using the same few.
	ephemeralBase = 40000

	// Retransmission.  The first timeout is generous by LAN standards
	// because the kernel only polls the NIC on a timer tick and a reply can
	// sit in the card for a millisecond or two before anyone looks.
	initialRTO = 400 // ms
	maxRTO     = 3200
	maxRetries = 5

	// How long Close waits for the peer to acknowledge the FIN before
	// giving up on a tidy shutdown.  A one-shot program cannot afford to
	// wait out a real timeout here, and the peer copes.
	finWaitMS = 300
)

type connState int

const (
	stateSynSent connState = iota
	stateEstablished
	stateCloseWait // the peer has sent FIN; we may still have data to read
	stateFinWait
	stateClosed
)

// Conn is an open TCP connection.  It is created by Dial and driven by the
// stack that created it; nothing here is safe to use from another goroutine.
type Conn struct {
	s *Stack

	remote IP
	rport  uint16
	lport  uint16

	state connState
	err   error

	snd    uint32 // next sequence number we will send
	sndUna uint32 // oldest sequence number we have sent and not seen acknowledged
	rcv    uint32 // next sequence number we expect from the peer

	rx      []byte
	peerFIN bool

	// The one unacknowledged segment.  pending is nil when there is
	// nothing outstanding, which is also the signal that Write may send.
	pending   []byte
	pendFlags byte
	pendSeq   uint32
	sentAt    uint64
	rto       uint64
	retries   int

	deadline uint64
}

// Dial opens a connection to port on dst.
func (s *Stack) Dial(dst IP, port uint16, timeout time.Duration) (*Conn, error) {
	lport, err := s.allocPort()
	if err != nil {
		return nil, err
	}

	// A sequence number that moves between runs matters more here than it
	// would elsewhere: a one-shot program reuses the same handful of local
	// ports every time it runs, so the peer may still remember the last
	// connection on this four-tuple.  A number drawn from the clock is
	// almost always ahead of the last one, which is what lets the peer
	// accept the new connection instead of treating it as an old duplicate.
	isn := uint32(libgor2.Ticks() * 1000)

	c := &Conn{
		s:        s,
		remote:   dst,
		rport:    port,
		lport:    lport,
		state:    stateSynSent,
		snd:      isn,
		sndUna:   isn,
		rx:       make([]byte, 0, 2048),
		rto:      initialRTO,
		deadline: deadlineFor(timeout),
	}

	s.conns = append(s.conns, c)

	s.logf("tcp %d -> %v:%d SYN", lport, dst, port)

	if err := c.transmit(flagSYN, nil); err != nil {
		c.drop()

		return nil, err
	}

	err = s.waitUntil(func() bool {
		return c.state != stateSynSent
	}, c.deadline)

	switch {
	case err != nil:
		c.drop()

		return nil, err

	case c.err != nil:
		e := c.err

		c.drop()

		return nil, e

	case c.state == stateClosed:
		c.drop()

		return nil, ErrRefused
	}

	return c, nil
}

// allocPort takes the next local port, binding it when this process is not the
// Ethernet driver and so only receives what it has asked for by port.
func (s *Stack) allocPort() (uint16, error) {
	port := s.nextPort

	s.nextPort++
	if s.nextPort < ephemeralBase || s.nextPort > ephemeralBase+7 {
		s.nextPort = ephemeralBase
	}

	if s.driver || s.bound[port] {
		return port, nil
	}

	if err := libgor2.NetBindPort(port); err != nil {
		return 0, err
	}

	s.bound[port] = true

	return port, nil
}

// SetDeadline gives every later call on this connection a fresh budget.
func (c *Conn) SetDeadline(timeout time.Duration) {
	c.deadline = deadlineFor(timeout)
}

// RemoteIP is the address this connection is to.
func (c *Conn) RemoteIP() IP { return c.remote }

// Write sends b, waiting for each segment to be acknowledged before sending
// the next one.
func (c *Conn) Write(b []byte) (int, error) {
	sent := 0

	for sent < len(b) {
		if err := c.usable(); err != nil {
			return sent, err
		}

		// Stop-and-wait: there is only ever one segment in flight, so
		// the previous one has to be acknowledged first.
		if err := c.s.waitUntil(func() bool {
			return c.pending == nil || c.state == stateClosed
		}, c.deadline); err != nil {
			return sent, err
		}

		if err := c.usable(); err != nil {
			return sent, err
		}

		n := len(b) - sent
		if n > mss {
			n = mss
		}

		if err := c.transmit(flagACK|flagPSH, b[sent:sent+n]); err != nil {
			return sent, err
		}

		sent += n
	}

	// Nothing has really been sent until it has been acknowledged, and a
	// caller that writes a request and then waits for a reply would
	// otherwise never find out that the request never arrived.
	if err := c.s.waitUntil(func() bool {
		return c.pending == nil || c.state == stateClosed
	}, c.deadline); err != nil {
		return sent, err
	}

	return sent, c.usable()
}

// Read copies what has arrived into b.  It blocks until something has, the
// peer closes --- which it reports as ErrClosed --- or the deadline passes.
func (c *Conn) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}

	if err := c.s.waitUntil(func() bool {
		return len(c.rx) > 0 || c.peerFIN || c.state == stateClosed || c.err != nil
	}, c.deadline); err != nil {
		return 0, err
	}

	if len(c.rx) > 0 {
		n := copy(b, c.rx)

		c.rx = c.rx[:copy(c.rx, c.rx[n:])]

		return n, nil
	}

	if c.err != nil {
		return 0, c.err
	}

	return 0, ErrClosed
}

// Close shuts the connection down, politely if there is time.
func (c *Conn) Close() error {
	switch c.state {
	case stateEstablished, stateCloseWait:
		c.pending = nil

		if err := c.transmit(flagFIN|flagACK, nil); err != nil {
			c.drop()

			return err
		}

		c.state = stateFinWait

		// Wait, but not for long: the point is to let the peer finish
		// its own close so it does not sit retransmitting into a
		// process that has already exited.
		_ = c.s.waitUntil(func() bool {
			return c.state == stateClosed
		}, libgor2.Ticks()+finWaitMS)

	case stateSynSent:
		_ = c.transmit(flagRST, nil)
	}

	c.drop()

	return nil
}

// usable reports why the connection cannot carry data, if it cannot.
func (c *Conn) usable() error {
	switch {
	case c.err != nil:
		return c.err

	case c.state == stateEstablished || c.state == stateCloseWait:
		return nil

	default:
		return ErrClosed
	}
}

// drop takes the connection off the stack's list and stops its timers.
func (c *Conn) drop() {
	c.state = stateClosed
	c.pending = nil

	for i, other := range c.s.conns {
		if other == c {
			c.s.conns = append(c.s.conns[:i], c.s.conns[i+1:]...)

			break
		}
	}
}

// transmit sends one segment and, when it occupies sequence space, remembers
// it for retransmission.
func (c *Conn) transmit(flags byte, data []byte) error {
	seq := c.snd

	if err := c.send(flags, seq, data); err != nil {
		return err
	}

	// SYN and FIN each consume a sequence number of their own even though
	// they carry nothing.
	used := uint32(len(data))
	if flags&(flagSYN|flagFIN) != 0 {
		used++
	}

	if used == 0 {
		return nil
	}

	c.snd += used

	c.pending = data
	c.pendFlags = flags
	c.pendSeq = seq
	c.sentAt = libgor2.Ticks()
	c.retries = 0
	c.rto = initialRTO

	return nil
}

// send writes one segment onto the wire, with no memory of it.
func (c *Conn) send(flags byte, seq uint32, data []byte) error {
	buf := c.s.txBuf[ipHeaderLen:]

	hdrLen := tcpHeaderLen
	if flags&flagSYN != 0 {
		hdrLen += 4 // the maximum segment size option
	}

	if hdrLen+len(data) > len(buf) {
		return ErrTooLong
	}

	window := rxCap - len(c.rx)
	if window < 0 {
		window = 0
	}

	buf[0], buf[1] = byte(c.lport>>8), byte(c.lport)
	buf[2], buf[3] = byte(c.rport>>8), byte(c.rport)

	putUint32(buf[4:8], seq)
	putUint32(buf[8:12], c.rcv)

	buf[12] = byte(hdrLen/4) << 4
	buf[13] = flags
	buf[14], buf[15] = byte(window>>8), byte(window)
	buf[16], buf[17] = 0, 0 // checksum
	buf[18], buf[19] = 0, 0 // urgent pointer

	if flags&flagSYN != 0 {
		buf[20], buf[21] = 2, 4 // option 2, length 4
		buf[22], buf[23] = byte(mss>>8), byte(mss&0xFF)
	}

	copy(buf[hdrLen:], data)

	segment := buf[:hdrLen+len(data)]

	sum := transportChecksum(c.s.localIP, c.remote, protoTCP, segment)
	segment[16], segment[17] = byte(sum>>8), byte(sum)

	c.s.logf("tx %v:%d -> %v:%d flags %02x seq %d ack %d len %d",
		c.s.localIP, c.lport, c.remote, c.rport, flags, seq, c.rcv, len(data))

	return c.s.sendIP(c.remote, protoTCP, len(segment))
}

// tick retransmits the outstanding segment when its timer has run out, and
// gives up when it has run out too often.
func (c *Conn) tick(now uint64) {
	if c.pending == nil && c.pendFlags == 0 {
		return
	}

	if c.state == stateClosed || now < c.sentAt+c.rto {
		return
	}

	if c.retries >= maxRetries {
		c.err = ErrTimeout
		c.state = stateClosed
		c.pending = nil

		return
	}

	c.retries++

	// Exponential backoff, which is the one piece of congestion control
	// that matters even for a stack this small: a host that is down should
	// not be shouted at.
	c.rto *= 2
	if c.rto > maxRTO {
		c.rto = maxRTO
	}

	c.sentAt = now

	c.s.logf("retransmit seq %d, try %d", c.pendSeq, c.retries)

	_ = c.send(c.pendFlags, c.pendSeq, c.pending)
}

// onTCP routes a segment to the connection that owns it.
func (s *Stack) onTCP(pkt ipPacket) {
	seg := pkt.payload
	if len(seg) < tcpHeaderLen {
		return
	}

	var (
		srcPort = uint16(seg[0])<<8 | uint16(seg[1])
		dstPort = uint16(seg[2])<<8 | uint16(seg[3])
		hdrLen  = int(seg[12]>>4) * 4
		flags   = seg[13]
	)

	if hdrLen < tcpHeaderLen || hdrLen > len(seg) {
		return
	}

	for _, c := range s.conns {
		if c.lport == dstPort && c.rport == srcPort && c.remote == pkt.src {
			c.onSegment(flags, uint32From(seg[4:8]), uint32From(seg[8:12]), seg[hdrLen:])

			return
		}
	}

	// A segment for a connection we do not have.  Answering with a reset
	// tells the peer at once instead of leaving it to retransmit into
	// silence --- which is what a process that has just exited leaves
	// behind, and there is usually one of those on this machine.
	if flags&flagRST == 0 {
		s.sendReset(pkt.src, dstPort, srcPort, uint32From(seg[4:8]), uint32From(seg[8:12]), flags, len(seg)-hdrLen)
	}
}

// sendReset answers a segment that belongs to nobody.
func (s *Stack) sendReset(dst IP, lport, rport uint16, seq, ack uint32, flags byte, dataLen int) {
	buf := s.txBuf[ipHeaderLen:]

	var (
		outSeq   uint32
		outAck   uint32
		outFlags byte = flagRST
	)

	// RFC 793: a reset for a segment that was acknowledging something takes
	// its sequence number from that acknowledgement, and otherwise
	// acknowledges everything the segment occupied.
	if flags&flagACK != 0 {
		outSeq = ack
	} else {
		used := uint32(dataLen)
		if flags&(flagSYN|flagFIN) != 0 {
			used++
		}

		outAck = seq + used
		outFlags |= flagACK
	}

	buf[0], buf[1] = byte(lport>>8), byte(lport)
	buf[2], buf[3] = byte(rport>>8), byte(rport)

	putUint32(buf[4:8], outSeq)
	putUint32(buf[8:12], outAck)

	buf[12] = (tcpHeaderLen / 4) << 4
	buf[13] = outFlags
	buf[14], buf[15] = 0, 0
	buf[16], buf[17] = 0, 0
	buf[18], buf[19] = 0, 0

	segment := buf[:tcpHeaderLen]

	sum := transportChecksum(s.localIP, dst, protoTCP, segment)
	segment[16], segment[17] = byte(sum>>8), byte(sum)

	_ = s.sendIP(dst, protoTCP, tcpHeaderLen)
}

// onSegment is the state machine.
func (c *Conn) onSegment(flags byte, seq, ack uint32, data []byte) {
	c.s.logf("rx %v:%d flags %02x seq %d ack %d len %d",
		c.remote, c.rport, flags, seq, ack, len(data))

	if flags&flagRST != 0 {
		// A reset during the handshake is a closed port; later on it is
		// a connection the peer has thrown away.  The distinction is
		// worth keeping: one says the service is not listening, the
		// other says it stopped listening halfway through.
		if c.state == stateSynSent {
			c.err = ErrRefused
		} else {
			c.err = ErrReset
		}

		c.state = stateClosed
		c.pending = nil

		return
	}

	switch c.state {
	case stateSynSent:
		if flags&flagSYN == 0 || flags&flagACK == 0 {
			return
		}

		if ack != c.snd {
			// Not an answer to the SYN we sent.
			return
		}

		c.rcv = seq + 1
		c.sndUna = ack
		c.pending = nil
		c.pendFlags = 0
		c.state = stateEstablished

		_ = c.send(flagACK, c.snd, nil)

		return

	case stateEstablished, stateCloseWait, stateFinWait:
		if flags&flagACK != 0 {
			c.onAck(ack)
		}

		if len(data) > 0 {
			c.onData(seq, data)
		}

		if flags&flagFIN != 0 {
			c.onFIN(seq, len(data))
		}

		if c.state == stateFinWait && c.pending == nil && c.peerFIN {
			// Our FIN is acknowledged and theirs has been answered:
			// there is nothing left for this connection to do.
			c.state = stateClosed
		}
	}
}

// onAck releases the outstanding segment once the peer has taken it.
func (c *Conn) onAck(ack uint32) {
	if seqLess(c.sndUna, ack) {
		c.sndUna = ack
	}

	if c.pending == nil && c.pendFlags == 0 {
		return
	}

	used := uint32(len(c.pending))
	if c.pendFlags&(flagSYN|flagFIN) != 0 {
		used++
	}

	if !seqLess(ack, c.pendSeq+used) {
		c.pending = nil
		c.pendFlags = 0
	}
}

// signalGap tells the peer that something it sent never arrived, with one
// duplicate acknowledgement per out-of-order segment, as RFC 5681 has it.
//
// The segments behind a gap do arrive now --- the kernel queues every frame in
// a buffer of its own instead of overwriting one shared buffer each tick --- so
// they produce the sender's three duplicates by themselves and it retransmits
// in one round trip.  This used to send all three at once, because behind a
// gap there was usually nothing: a burst was lost wholesale.  On a link that
// only reorders, that forced a retransmission nobody needed.
func (c *Conn) signalGap() {
	_ = c.send(flagACK, c.snd, nil)
}

// onData takes in-order data and acknowledges it, and tells the peer about the
// gap when there is one.
func (c *Conn) onData(seq uint32, data []byte) {
	if seq != c.rcv {
		c.signalGap()

		return
	}

	space := rxCap - len(c.rx)

	n := len(data)
	if n > space {
		// Only acknowledge what actually fit.  The peer will send the
		// rest when Read has drained the buffer and the window we
		// advertise reopens.
		n = space
	}

	c.rx = append(c.rx, data[:n]...)
	c.rcv += uint32(n)

	_ = c.send(flagACK, c.snd, nil)
}

// onFIN acknowledges the peer's half of the close.
func (c *Conn) onFIN(seq uint32, dataLen int) {
	if seq+uint32(dataLen) != c.rcv {
		// The FIN sits after data we never saw, so acknowledging it
		// would claim that data arrived.  Say what is missing instead.
		c.signalGap()

		return
	}

	c.peerFIN = true
	c.rcv++

	_ = c.send(flagACK, c.snd, nil)

	if c.state == stateEstablished {
		// Half closed.  The connection stays readable: an HTTP/1.0
		// server closes as soon as it has sent the response, and the
		// response is the whole point.
		c.state = stateCloseWait
	}
}

// seqLess compares sequence numbers the way TCP requires, so that the
// comparison keeps working when they wrap past four billion.
func seqLess(a, b uint32) bool {
	return int32(a-b) < 0
}

func uint32From(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

func putUint32(b []byte, v uint32) {
	b[0] = byte(v >> 24)
	b[1] = byte(v >> 16)
	b[2] = byte(v >> 8)
	b[3] = byte(v)
}
