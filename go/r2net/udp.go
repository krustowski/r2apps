package r2net

import "time"

// UDP, just enough of it to ask a question and hear the answer.  The only
// caller in this package is the resolver, but a datagram exchange is a useful
// thing to have on a machine with no sockets, so it is exported.

const udpHeaderLen = 8

type dgramWaiter struct {
	lport uint16
	peer  IP
	rport uint16

	out []byte
	n   int
	got bool
}

// Exchange sends one datagram to dst:port and waits for one back.  It returns
// how much of the reply fitted in reply.
//
// Like Ping, this needs the global Ethernet driver registration: the kernel
// routes frames to a process by TCP port, and a UDP datagram has none as far
// as it is concerned, so it goes to the driver.
func (s *Stack) Exchange(dst IP, port uint16, payload, reply []byte, timeout time.Duration) (int, error) {
	if !s.driver {
		return 0, ErrNoDatagram
	}

	if udpHeaderLen+len(payload) > len(s.txBuf)-ipHeaderLen {
		return 0, ErrTooLong
	}

	lport, err := s.allocPort()
	if err != nil {
		return 0, err
	}

	w := &dgramWaiter{lport: lport, peer: dst, rport: port, out: reply}

	s.dgram = w
	defer func() { s.dgram = nil }()

	buf := s.txBuf[ipHeaderLen:]

	length := udpHeaderLen + len(payload)

	buf[0], buf[1] = byte(lport>>8), byte(lport)
	buf[2], buf[3] = byte(port>>8), byte(port)
	buf[4], buf[5] = byte(length>>8), byte(length)
	buf[6], buf[7] = 0, 0 // checksum

	copy(buf[udpHeaderLen:], payload)

	datagram := buf[:length]

	sum := transportChecksum(s.localIP, dst, protoUDP, datagram)
	if sum == 0 {
		// Zero means "no checksum" in IPv4 UDP, so a sum that comes out
		// zero is sent as its other representation.
		sum = 0xFFFF
	}

	datagram[6], datagram[7] = byte(sum>>8), byte(sum)

	if err := s.sendIP(dst, protoUDP, length); err != nil {
		return 0, err
	}

	s.logf("udp %d -> %v:%d, %d bytes", lport, dst, port, len(payload))

	if err := s.waitUntil(func() bool { return w.got }, deadlineFor(timeout)); err != nil {
		return 0, err
	}

	return w.n, nil
}

// onUDP hands a datagram to whoever is waiting for one.
func (s *Stack) onUDP(pkt ipPacket) {
	dgram := pkt.payload
	if len(dgram) < udpHeaderLen {
		return
	}

	w := s.dgram
	if w == nil || w.got {
		return
	}

	var (
		src    = uint16(dgram[0])<<8 | uint16(dgram[1])
		dst    = uint16(dgram[2])<<8 | uint16(dgram[3])
		length = int(dgram[4])<<8 | int(dgram[5])
	)

	if pkt.src != w.peer || src != w.rport || dst != w.lport {
		return
	}

	if length < udpHeaderLen || length > len(dgram) {
		length = len(dgram)
	}

	w.n = copy(w.out, dgram[udpHeaderLen:length])
	w.got = true
}
