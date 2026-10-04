package r2net

import (
	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"time"
)

// ResolveTCP resolves an A record over DNS/TCP. Unlike Resolve's UDP query,
// this works when another process (the normal eth driver) owns datagrams:
// the kernel can route TCP replies to our bound local port.
func (s *Stack) ResolveTCP(name string, timeout time.Duration) (IP, error) {
	if ip, ok := ParseIP(name); ok {
		return ip, nil
	}
	for _, entry := range s.cache {
		if entry.name == name {
			return entry.ip, nil
		}
	}
	if s.dns.IsZero() {
		return IP{}, ErrNoServer
	}
	deadline := deadlineFor(timeout)
	var query [maxDNSMessage]byte
	n, ok := buildQuery(query[:], name, uint16(libgor2.Ticks()))
	if !ok {
		return IP{}, ErrNoAnswer
	}
	conn, err := s.Dial(s.dns, dnsPort, remaining(deadline))
	if err != nil {
		return IP{}, err
	}
	defer conn.Close()
	conn.deadline = deadline
	var answer [4096]byte
	got, err := dnsStreamExchange(conn, query[:n], answer[:])
	if err != nil {
		return IP{}, err
	}
	ip, ok := parseAnswer(answer[:got], query[0], query[1])
	if !ok {
		return IP{}, ErrNoAnswer
	}
	s.cache = append(s.cache, resolved{name: name, ip: ip})
	return ip, nil
}
