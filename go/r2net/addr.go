package r2net

import "strconv"

// IP is an IPv4 address.  There is no net package on r2, so this is the
// currency of the whole package: four bytes, most significant first.
type IP [4]byte

// ParseIP reads a dotted-quad address.  It is deliberately strict --- no
// shorthand, no octal, no hostnames --- because the only other thing a host
// field can be here is a name for the resolver, and a sloppy parser would
// swallow one and produce nonsense.
func ParseIP(s string) (IP, bool) {
	var (
		ip     IP
		n      int // octets seen
		acc    int
		digits int
	)

	for i := 0; i < len(s); i++ {
		c := s[i]

		switch {
		case c >= '0' && c <= '9':
			acc = acc*10 + int(c-'0')
			digits++

			if acc > 255 || digits > 3 {
				return IP{}, false
			}

		case c == '.':
			if digits == 0 || n >= 3 {
				return IP{}, false
			}

			ip[n] = byte(acc)
			n++
			acc, digits = 0, 0

		default:
			return IP{}, false
		}
	}

	if digits == 0 || n != 3 {
		return IP{}, false
	}

	ip[3] = byte(acc)

	return ip, true
}

// String renders the address as a dotted quad.
func (ip IP) String() string {
	return strconv.Itoa(int(ip[0])) + "." +
		strconv.Itoa(int(ip[1])) + "." +
		strconv.Itoa(int(ip[2])) + "." +
		strconv.Itoa(int(ip[3]))
}

// IsZero reports whether the address is 0.0.0.0, which is what every field the
// kernel has not filled in looks like.
func (ip IP) IsZero() bool {
	return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0
}

// IsBroadcast reports whether the address is the all-ones broadcast.
func (ip IP) IsBroadcast() bool {
	return ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255
}

// sameSubnet reports whether a and b share the network given by mask.
func sameSubnet(a, b, mask IP) bool {
	for i := 0; i < 4; i++ {
		if a[i]&mask[i] != b[i]&mask[i] {
			return false
		}
	}

	return true
}

// MAC is an Ethernet hardware address.
type MAC [6]byte

// String renders the address in the usual colon-separated hex.
func (m MAC) String() string {
	const hex = "0123456789abcdef"

	out := make([]byte, 0, 17)
	for i, b := range m {
		if i > 0 {
			out = append(out, ':')
		}

		out = append(out, hex[b>>4], hex[b&0x0F])
	}

	return string(out)
}

// IsZero reports whether the address is all zeroes.
func (m MAC) IsZero() bool {
	for _, b := range m {
		if b != 0 {
			return false
		}
	}

	return true
}

var broadcastMAC = MAC{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
