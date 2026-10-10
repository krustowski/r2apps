//go:build r2abimock && !r2

package r2net

import "testing"

type recordLink struct{ sent [][]byte }

func (l *recordLink) send(p []byte, _ IP) error {
	l.sent = append(l.sent, append([]byte(nil), p...))
	return nil
}
func (*recordLink) recv([]byte) int { return -1 }
func (*recordLink) name() string    { return "record" }

// A SYN that lands on the server's TIME_WAIT is answered with the old
// connection's ACK.  The client resets it, and still waits for its SYN-ACK.
func TestSynSentResetsAnOldConnection(t *testing.T) {
	l := &recordLink{}
	s := &Stack{link: l, localIP: IP{10, 3, 4, 2}, netmask: IP{255, 255, 255, 0}, gateway: IP{10, 3, 4, 1}}
	c := &Conn{s: s, remote: IP{149, 154, 166, 110}, lport: 48001, rport: 443, state: stateSynSent, snd: 1001}
	s.conns = []*Conn{c}

	c.onSegment(flagACK, 777, 5555, nil) // the old connection's last ACK
	if c.state != stateSynSent || len(l.sent) != 1 {
		t.Fatalf("state %v, %d segments sent", c.state, len(l.sent))
	}
	tcp := l.sent[0][ipHeaderLen:]
	if tcp[13] != flagRST || uint32From(tcp[4:8]) != 5555 || int(tcp[0])<<8|int(tcp[1]) != 48001 {
		t.Fatalf("flags %02x seq %d", tcp[13], uint32From(tcp[4:8]))
	}

	c.onSegment(flagSYN|flagACK, 9000, 1001, nil) // and then the real answer
	if c.state != stateEstablished || c.rcv != 9001 {
		t.Fatalf("state %v rcv %d", c.state, c.rcv)
	}
}
