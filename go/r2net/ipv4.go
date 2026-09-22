package r2net

// IPv4, and the checksum every protocol above it borrows.
//
// The kernel will build an IPv4 packet for us (syscall 0x33), but it insists
// on supplying the addresses from a header whose source and destination it
// then exchanges, and it writes its answer back over the buffer it was given.
// Building the header here instead is fewer moving parts than working out what
// it did, and it is the same code for both links: the SLIP send syscall copies
// total_length bytes onto the wire without looking at anything else, and the
// Ethernet one only reads that same field to find where the frame ends.

const (
	ipHeaderLen  = 20
	ipVersionIHL = 0x45 // version 4, header length 5 words
	ipTTL        = 64

	protoICMP = 1
	protoTCP  = 6
	protoUDP  = 17
)

// putIPv4 writes a header for a packet of payloadLen bytes at the front of
// buf, and returns where the payload goes.  The Don't Fragment bit is set: we
// never reassemble, so a fragment that arrived would be dropped anyway, and it
// is better for the sender to be told.
func putIPv4(buf []byte, src, dst IP, proto byte, payloadLen int, id uint16) int {
	total := ipHeaderLen + payloadLen

	buf[0] = ipVersionIHL
	buf[1] = 0
	buf[2] = byte(total >> 8)
	buf[3] = byte(total)
	buf[4] = byte(id >> 8)
	buf[5] = byte(id)
	buf[6] = 0x40 // DF
	buf[7] = 0
	buf[8] = ipTTL
	buf[9] = proto
	buf[10], buf[11] = 0, 0 // checksum, filled in below

	copy(buf[12:16], src[:])
	copy(buf[16:20], dst[:])

	sum := checksum(buf[:ipHeaderLen])
	buf[10] = byte(sum >> 8)
	buf[11] = byte(sum)

	return ipHeaderLen
}

// ipPacket is a received IPv4 packet, already trimmed to its real length.
type ipPacket struct {
	src     IP
	dst     IP
	proto   byte
	payload []byte
}

// parseIPv4 picks a packet apart, or reports that it is not one.
//
// The length comes from the header rather than from the frame, because a NIC
// pads a short frame out to 60 bytes and those padding bytes would otherwise
// be handed to TCP as data.
func parseIPv4(p []byte) (ipPacket, bool) {
	if len(p) < ipHeaderLen {
		return ipPacket{}, false
	}

	if p[0]>>4 != 4 {
		return ipPacket{}, false
	}

	var (
		hdrLen = int(p[0]&0x0F) * 4
		total  = int(p[2])<<8 | int(p[3])
	)

	if hdrLen < ipHeaderLen || total < hdrLen || total > len(p) {
		return ipPacket{}, false
	}

	// A fragment that is not the first one carries no transport header, and
	// one that is carries only part of the message.  Neither is something
	// this stack can do anything with.
	if p[6]&0x1F != 0 || p[7] != 0 {
		return ipPacket{}, false
	}

	var pkt ipPacket

	copy(pkt.src[:], p[12:16])
	copy(pkt.dst[:], p[16:20])

	pkt.proto = p[9]
	pkt.payload = p[hdrLen:total]

	return pkt, true
}

// checksum is the one's-complement sum of RFC 1071, over big-endian 16-bit
// words, with an odd trailing byte treated as the high half of a word.
func checksum(b []byte) uint16 {
	var sum uint32

	for i := 0; i+1 < len(b); i += 2 {
		sum += uint32(b[i])<<8 | uint32(b[i+1])
	}

	if len(b)%2 == 1 {
		sum += uint32(b[len(b)-1]) << 8
	}

	for sum>>16 != 0 {
		sum = sum&0xFFFF + sum>>16
	}

	return ^uint16(sum)
}

// transportChecksum is the same sum taken over the pseudo-header that TCP and
// UDP prepend to themselves: source, destination, protocol and length.  It is
// what makes a segment delivered to the wrong host detectable.
func transportChecksum(src, dst IP, proto byte, segment []byte) uint16 {
	var sum uint32

	sum += uint32(src[0])<<8 | uint32(src[1])
	sum += uint32(src[2])<<8 | uint32(src[3])
	sum += uint32(dst[0])<<8 | uint32(dst[1])
	sum += uint32(dst[2])<<8 | uint32(dst[3])
	sum += uint32(proto)
	sum += uint32(len(segment))

	for i := 0; i+1 < len(segment); i += 2 {
		sum += uint32(segment[i])<<8 | uint32(segment[i+1])
	}

	if len(segment)%2 == 1 {
		sum += uint32(segment[len(segment)-1]) << 8
	}

	for sum>>16 != 0 {
		sum = sum&0xFFFF + sum>>16
	}

	return ^uint16(sum)
}
