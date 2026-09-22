package r2net

import (
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// A resolver, in as few bytes as a resolver can be written.
//
// One A query, one UDP datagram, first answer wins.  No cache beyond the one
// below, no search domains, no CNAME chasing beyond what the server does for
// us (every real server does), no TCP fallback and no retry on a truncated
// answer --- an A record for one name never comes close to 512 bytes.

const (
	dnsPort = 53

	dnsTypeA     = 1
	dnsClassIN   = 1
	dnsHeaderLen = 12

	maxDNSMessage = 512
)

// resolved remembers what this process has already looked up.  A monitoring
// run checks the same host several times over --- once per protocol --- and
// asking twice would be both slower and a different answer waiting to happen.
type resolved struct {
	name string
	ip   IP
}

// Resolve turns a host name into an address.  A name that is already a dotted
// quad is returned as it is, without asking anybody.
func (s *Stack) Resolve(name string, timeout time.Duration) (IP, error) {
	if ip, ok := ParseIP(name); ok {
		return ip, nil
	}

	for i := range s.cache {
		if s.cache[i].name == name {
			return s.cache[i].ip, nil
		}
	}

	if s.dns.IsZero() {
		return IP{}, ErrNoServer
	}

	var query [maxDNSMessage]byte

	n, ok := buildQuery(query[:], name, uint16(libgor2.Ticks()))
	if !ok {
		return IP{}, ErrNoAnswer
	}

	var answer [maxDNSMessage]byte

	got, err := s.Exchange(s.dns, dnsPort, query[:n], answer[:], timeout)
	if err != nil {
		return IP{}, err
	}

	ip, ok := parseAnswer(answer[:got], query[0], query[1])
	if !ok {
		return IP{}, ErrNoAnswer
	}

	s.logf("resolved %s to %v", name, ip)

	s.cache = append(s.cache, resolved{name: name, ip: ip})

	return ip, nil
}

// buildQuery writes a standard recursive A query for name into buf.
func buildQuery(buf []byte, name string, id uint16) (int, bool) {
	if len(name) == 0 || len(name) > 253 {
		return 0, false
	}

	buf[0], buf[1] = byte(id>>8), byte(id)
	buf[2], buf[3] = 0x01, 0x00 // standard query, recursion desired
	buf[4], buf[5] = 0, 1       // one question
	buf[6], buf[7] = 0, 0
	buf[8], buf[9] = 0, 0
	buf[10], buf[11] = 0, 0

	n := dnsHeaderLen

	// The name goes on the wire as a run of length-prefixed labels.
	start := 0
	for i := 0; i <= len(name); i++ {
		if i < len(name) && name[i] != '.' {
			continue
		}

		label := name[start:i]
		if len(label) == 0 || len(label) > 63 {
			return 0, false
		}

		if n+1+len(label) > len(buf)-5 {
			return 0, false
		}

		buf[n] = byte(len(label))
		n++

		n += copy(buf[n:], label)

		start = i + 1
	}

	buf[n] = 0
	n++

	buf[n], buf[n+1] = 0, dnsTypeA
	buf[n+2], buf[n+3] = 0, dnsClassIN

	return n + 4, true
}

// parseAnswer digs the first A record out of a reply.
func parseAnswer(msg []byte, idHi, idLo byte) (IP, bool) {
	if len(msg) < dnsHeaderLen {
		return IP{}, false
	}

	if msg[0] != idHi || msg[1] != idLo {
		return IP{}, false
	}

	// Bit 15 of the flags is the response bit; the low four bits are the
	// result code, and anything but zero means the server is telling us it
	// could not answer.
	if msg[2]&0x80 == 0 || msg[3]&0x0F != 0 {
		return IP{}, false
	}

	var (
		questions = int(msg[4])<<8 | int(msg[5])
		answers   = int(msg[6])<<8 | int(msg[7])
		off       = dnsHeaderLen
	)

	for i := 0; i < questions; i++ {
		n, ok := skipName(msg, off)
		if !ok || n+4 > len(msg) {
			return IP{}, false
		}

		off = n + 4
	}

	for i := 0; i < answers; i++ {
		n, ok := skipName(msg, off)
		if !ok || n+10 > len(msg) {
			return IP{}, false
		}

		var (
			rrType = int(msg[n])<<8 | int(msg[n+1])
			class  = int(msg[n+2])<<8 | int(msg[n+3])
			length = int(msg[n+8])<<8 | int(msg[n+9])
		)

		off = n + 10

		if off+length > len(msg) {
			return IP{}, false
		}

		if rrType == dnsTypeA && class == dnsClassIN && length == 4 {
			var ip IP

			copy(ip[:], msg[off:off+4])

			return ip, true
		}

		off += length
	}

	return IP{}, false
}

// skipName walks past a name, which may end in a pointer back into the
// message.  It returns where the name ends.
func skipName(msg []byte, off int) (int, bool) {
	for {
		if off >= len(msg) {
			return 0, false
		}

		length := int(msg[off])

		switch {
		case length == 0:
			return off + 1, true

		case length&0xC0 == 0xC0:
			// A pointer is two bytes and always the end of a name,
			// so there is nothing to follow: whatever it points at
			// is somewhere we have already been or will be.
			return off + 2, true

		case length > 63:
			return 0, false

		default:
			off += 1 + length
		}
	}
}
