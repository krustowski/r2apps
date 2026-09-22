// icmpresp answers ICMP echo requests that arrive as SLIP frames on the serial
// port.  It is a port of c/icmpresp, and a demonstration that a Go program can
// be a real r2 service and not just a hello world.
//
//	icmpresp          quiet
//	icmpresp debug    log every request and reply, with a timestamp
package main

import (
	"fmt"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// SLIP framing (RFC 1055).
const (
	slipEnd    = 0xC0
	slipEsc    = 0xDB
	slipEscEnd = 0xDC
	slipEscEsc = 0xDD
)

const (
	protoICMP = 1

	icmpEchoRequest = 8
	icmpEchoReply   = 0

	icmpHeaderLen = 8
	ipv4MinHeader = 20

	// The kernel zeroes a fixed 512 bytes of the buffer handed to NewPacket
	// before writing the packet it built, so anything shorter than that gets
	// written past its end.
	frameBufferLen = 2048
)

var debug bool

func main() {
	if args := libgor2.Args(); len(args) > 1 {
		fmt.Printf("icmpresp: started with args: %v\n", args[1:])

		for _, a := range args[1:] {
			if a == "debug" {
				debug = true
			}
		}
	}

	fmt.Printf("-> icmpresp service start\n")

	if err := libgor2.SerialInit(); err != nil {
		fmt.Printf("-> serial port could not be initialized: %v\n", err)
		return
	}

	var (
		dec   = &slipDecoder{buf: make([]byte, 0, frameBufferLen)}
		frame = make([]byte, frameBufferLen)
	)

	for {
		b, ok := libgor2.SerialRead()
		if !ok {
			continue
		}

		n := dec.feed(b, frame)
		if n <= 0 {
			continue
		}

		if err := respond(frame[:n]); err != nil && debug {
			fmt.Printf("-> %v\n", err)
		}
	}
}

// respond turns one received IPv4 packet into an echo reply, when that is what
// it is, and puts it back on the wire.
func respond(packet []byte) error {
	ip, ok := parseIPv4(packet)
	if !ok || ip.protocol != protoICMP {
		return nil
	}

	icmp := packet[ip.headerLen:]
	if len(icmp) < icmpHeaderLen {
		return nil
	}

	if icmp[0] != icmpEchoRequest || icmp[1] != 0 {
		if debug {
			fmt.Printf("-> unknown ICMP type %d code %d\n", icmp[0], icmp[1])
		}

		return nil
	}

	logf(">> received an ICMP Echo Request")

	// Flip the type and recompute the checksum over the whole ICMP message,
	// in place: the payload of an echo reply is the payload of the request.
	icmp[0] = icmpEchoReply
	icmp[2], icmp[3] = 0, 0

	sum := checksum(icmp)
	icmp[2] = byte(sum >> 8)
	icmp[3] = byte(sum)

	// NewPacket rebuilds the IPv4 header around this payload with the source
	// and destination addresses exchanged, then SendPacket puts it out.
	if err := libgor2.NewPacket(libgor2.PacketIPv4, packet); err != nil {
		return fmt.Errorf("IPv4 packet creation failed: %w", err)
	}

	if err := libgor2.SendPacket(libgor2.PacketIPv4, packet); err != nil {
		return fmt.Errorf("failed to send the IPv4 packet: %w", err)
	}

	logf("<< echo reply sent")

	return nil
}

// logf prints a line with a timestamp, but only when started with `debug`.
func logf(msg string) {
	if !debug {
		return
	}

	var t libgor2.RTC
	if err := libgor2.ReadRTC(&t); err == nil {
		fmt.Printf("%02d:%02d:%02d %04d-%02d-%02d ",
			t.Hours, t.Minutes, t.Seconds, t.Year(), t.Month, t.Day)
	}

	fmt.Printf("%s\n", msg)
}

// slipDecoder reassembles SLIP frames one byte at a time.
//
// The C original re-scans everything received so far on every byte, which
// costs more the longer the frame gets; keeping the state here is both cheaper
// and shorter.
type slipDecoder struct {
	buf    []byte
	escape bool
}

// feed adds one received byte and, when that byte completes a frame, copies it
// into out and returns its length.  It returns 0 while a frame is still coming
// in, and -1 on a framing error.
func (d *slipDecoder) feed(b byte, out []byte) int {
	switch {
	case b == slipEnd:
		// A leading END, or the END of an empty frame, is not a frame.
		if len(d.buf) == 0 {
			return 0
		}

		n := copy(out, d.buf)
		d.reset()

		return n

	case b == slipEsc:
		d.escape = true

		return 0

	case d.escape:
		d.escape = false

		switch b {
		case slipEscEnd:
			b = slipEnd
		case slipEscEsc:
			b = slipEsc
		default:
			// Not a valid escape: drop what we have and resynchronise on
			// the next END.
			d.reset()

			return -1
		}
	}

	if len(d.buf) >= cap(d.buf) || len(d.buf) >= len(out) {
		d.reset()

		return -1
	}

	d.buf = append(d.buf, b)

	return 0
}

func (d *slipDecoder) reset() {
	d.buf = d.buf[:0]
	d.escape = false
}

// ipv4Header is as much of an IPv4 header as this program cares about.
type ipv4Header struct {
	headerLen int
	totalLen  int
	protocol  byte
}

func parseIPv4(p []byte) (ipv4Header, bool) {
	if len(p) < ipv4MinHeader {
		return ipv4Header{}, false
	}

	h := ipv4Header{
		headerLen: int(p[0]&0x0F) * 4,
		totalLen:  int(p[2])<<8 | int(p[3]),
		protocol:  p[9],
	}

	if h.headerLen < ipv4MinHeader || h.headerLen > len(p) {
		return ipv4Header{}, false
	}

	return h, true
}

// checksum is the one's-complement sum from RFC 1071, as the rest of the ABI
// expects it: computed over big-endian 16-bit words.
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
