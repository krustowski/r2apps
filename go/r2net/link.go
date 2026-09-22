package r2net

import (
	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// The link layer: the two ways a packet can leave an r2 machine.
//
// SLIP is a serial line with no addressing of any kind --- whatever we write
// goes to the one host at the other end, and the kernel does the framing.
// Ethernet is a real network, so it brings ARP with it, and the frame has to
// be built here because the kernel's send syscall takes it ready-made.

// frameLen is the size of every receive buffer in this package.  The kernel
// drops a frame longer than 2048 bytes before we ever see it, and its own
// receive syscall copies as many bytes as the frame is long without being told
// how much room we have, so nothing shorter is safe.
const frameLen = 2048

type link interface {
	// send puts one complete IPv4 packet on the wire.  nextHop is the
	// address whose hardware address the packet should be aimed at, which
	// is the destination on the local link and the gateway otherwise; a
	// link without addressing ignores it.
	send(pkt []byte, nextHop IP) error

	// recv copies the next IPv4 packet into buf and returns its length, or
	// 0 when nothing has arrived.  It never blocks.  Anything that belongs
	// to the link itself --- an ARP request, say --- is dealt with here and
	// reported as 0.
	recv(buf []byte) int

	name() string
}

//
//  SLIP
//

type slipLink struct {
	dec slipDecoder
}

func newSLIPLink() (*slipLink, error) {
	if err := libgor2.SerialInit(); err != nil {
		return nil, err
	}

	return &slipLink{dec: slipDecoder{buf: make([]byte, 0, frameLen)}}, nil
}

func (l *slipLink) name() string { return "slip" }

// send hands the packet to the kernel, which SLIP-encodes it and writes it to
// the UART.  The kernel takes the length from the total_length field of the
// header, so the packet has to be complete before it gets here --- which it
// is: putIPv4 wrote that field itself.
func (l *slipLink) send(pkt []byte, _ IP) error {
	return libgor2.SendPacket(libgor2.PacketIPv4, pkt)
}

// recv drains the UART until a frame completes or the line goes quiet.
//
// One byte per syscall is what the ABI offers, so this is as tight as it can
// be; the loop gives up as soon as a read comes back empty rather than
// spinning, because the caller has other work --- retransmission timers --- to
// get back to.
func (l *slipLink) recv(buf []byte) int {
	for {
		b, ok := libgor2.SerialRead()
		if !ok {
			return 0
		}

		if n := l.dec.feed(b, buf); n > 0 {
			return n
		}
	}
}

//
//  Ethernet
//

const (
	ethHeaderLen = 14

	etherTypeIPv4 = 0x0800
	etherTypeARP  = 0x0806

	arpPacketLen = 28
	arpRequest   = 1
	arpReply     = 2
)

type arpEntry struct {
	ip    IP
	mac   MAC
	valid bool
}

type ethLink struct {
	mac     MAC
	localIP IP

	cache [8]arpEntry

	// Both buffers are fields rather than locals: the ABI wants the address
	// of the first byte as an integer, and TinyGo answers that by moving the
	// local to the heap on every single call.  See "Taking the address of a
	// local costs an allocation" in ../README.md.
	rxFrame [frameLen]byte
	txFrame [frameLen]byte
}

func newEthLink(mac MAC, localIP IP) *ethLink {
	return &ethLink{mac: mac, localIP: localIP}
}

func (l *ethLink) name() string { return "eth" }

func (l *ethLink) setLocalIP(ip IP) { l.localIP = ip }

// send wraps the packet in an Ethernet header and gives it to the kernel,
// which reads the IPv4 total_length field to find where the frame ends.
//
// When the next hop's hardware address is not known yet the frame goes to the
// broadcast address and an ARP request goes out behind it.  Sending it rather
// than queueing it matters here: this stack has one connection's worth of
// state and no retransmission queue of its own, and a host that receives a
// broadcast frame carrying a unicast IP packet still processes it.  The reply
// fills the cache, so only the first frame of a run is ever broadcast.
func (l *ethLink) send(pkt []byte, nextHop IP) error {
	dst, known := l.lookup(nextHop)
	if !known {
		dst = broadcastMAC

		if err := l.sendARPRequest(nextHop); err != nil {
			return err
		}
	}

	if ethHeaderLen+len(pkt) > len(l.txFrame) {
		return ErrTooLong
	}

	copy(l.txFrame[0:6], dst[:])
	copy(l.txFrame[6:12], l.mac[:])

	l.txFrame[12] = etherTypeIPv4 >> 8
	l.txFrame[13] = etherTypeIPv4 & 0xFF

	copy(l.txFrame[ethHeaderLen:], pkt)

	return libgor2.SendPacket(libgor2.PacketEth, l.txFrame[:ethHeaderLen+len(pkt)])
}

// recv takes one frame from the kernel's queue for this process.
//
// ARP is answered and learned from here rather than passed up, because it
// belongs to the link and because the process that holds the NIC is the only
// one that can answer it: while this program runs, it is the machine's
// Ethernet stack, and a machine that does not answer who-has disappears from
// the network it is trying to measure.
func (l *ethLink) recv(buf []byte) int {
	n := libgor2.ReceiveNonBlocking(l.rxFrame[:])
	if n < ethHeaderLen || n > len(l.rxFrame) {
		return 0
	}

	frame := l.rxFrame[:n]

	// Frames from our own hardware address are our own, looped back by the
	// host bridge.  Answering them would be answering ourselves.
	var src MAC
	copy(src[:], frame[6:12])

	if src == l.mac {
		return 0
	}

	switch uint16(frame[12])<<8 | uint16(frame[13]) {
	case etherTypeARP:
		l.onARP(frame)

		return 0

	case etherTypeIPv4:
		payload := frame[ethHeaderLen:]

		pkt, ok := parseIPv4(payload)
		if !ok {
			return 0
		}

		// The sender's hardware address comes free with every frame, so
		// there is no reason to ask for it later.
		l.remember(pkt.src, src)

		return copy(buf, payload[:ipHeaderLen+len(pkt.payload)])
	}

	return 0
}

// onARP answers a request for our own address and learns from everything else.
func (l *ethLink) onARP(frame []byte) {
	if len(frame) < ethHeaderLen+arpPacketLen {
		return
	}

	arp := frame[ethHeaderLen : ethHeaderLen+arpPacketLen]

	// Ethernet over IPv4 only: hardware type 1, protocol type 0x0800.
	if arp[0] != 0 || arp[1] != 1 || arp[2] != 0x08 || arp[3] != 0x00 {
		return
	}

	var (
		op  = uint16(arp[6])<<8 | uint16(arp[7])
		sha MAC
		spa IP
		tpa IP
	)

	copy(sha[:], arp[8:14])
	copy(spa[:], arp[14:18])
	copy(tpa[:], arp[24:28])

	l.remember(spa, sha)

	if op == arpRequest && tpa == l.localIP && !l.localIP.IsZero() {
		l.sendARP(arpReply, spa, sha)
	}
}

// sendARPRequest asks the link who holds ip.
func (l *ethLink) sendARPRequest(ip IP) error {
	l.sendARP(arpRequest, ip, broadcastMAC)

	return nil
}

func (l *ethLink) sendARP(op uint16, targetIP IP, targetMAC MAC) {
	dst := targetMAC
	if op == arpRequest {
		dst = broadcastMAC
	}

	copy(l.txFrame[0:6], dst[:])
	copy(l.txFrame[6:12], l.mac[:])

	l.txFrame[12] = etherTypeARP >> 8
	l.txFrame[13] = etherTypeARP & 0xFF

	arp := l.txFrame[ethHeaderLen : ethHeaderLen+arpPacketLen]

	arp[0], arp[1] = 0, 1       // hardware type: Ethernet
	arp[2], arp[3] = 0x08, 0x00 // protocol type: IPv4
	arp[4], arp[5] = 6, 4       // address lengths
	arp[6], arp[7] = byte(op>>8), byte(op)

	copy(arp[8:14], l.mac[:])
	copy(arp[14:18], l.localIP[:])

	if op == arpReply {
		copy(arp[18:24], targetMAC[:])
	} else {
		for i := 18; i < 24; i++ {
			arp[i] = 0
		}
	}

	copy(arp[24:28], targetIP[:])

	// The kernel knows an ARP frame is 42 bytes and sends exactly that.
	_ = libgor2.SendPacket(libgor2.PacketEth, l.txFrame[:ethHeaderLen+arpPacketLen])
}

func (l *ethLink) lookup(ip IP) (MAC, bool) {
	if ip.IsZero() || ip.IsBroadcast() {
		return broadcastMAC, true
	}

	for i := range l.cache {
		if l.cache[i].valid && l.cache[i].ip == ip {
			return l.cache[i].mac, true
		}
	}

	return MAC{}, false
}

func (l *ethLink) remember(ip IP, mac MAC) {
	if ip.IsZero() || mac.IsZero() || mac == broadcastMAC {
		return
	}

	for i := range l.cache {
		if !l.cache[i].valid || l.cache[i].ip == ip {
			l.cache[i] = arpEntry{ip: ip, mac: mac, valid: true}

			return
		}
	}

	l.cache[0] = arpEntry{ip: ip, mac: mac, valid: true}
}
