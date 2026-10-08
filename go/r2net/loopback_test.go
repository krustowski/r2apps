//go:build r2abimock && !r2

package r2net

import "testing"

func TestLoopbackRoutesWithoutGateway(t *testing.T) {
	s := &Stack{localIP: IP{10, 3, 4, 2}, netmask: IP{255, 255, 255, 0}, gateway: IP{10, 3, 4, 1}}
	for _, dst := range []IP{{127, 0, 0, 1}, {127, 255, 255, 254}, s.localIP, {10, 3, 4, 5}} {
		if got := s.nextHop(dst); got != dst {
			t.Fatalf("local route %v via %v", dst, got)
		}
	}
	if dst := (IP{1, 1, 1, 1}); s.nextHop(dst) != s.gateway {
		t.Fatal("remote route lost gateway")
	}
	if (IP{126, 0, 0, 1}).IsLoopback() || (IP{128, 0, 0, 1}).IsLoopback() {
		t.Fatal("loopback range too broad")
	}
}

func TestLoopbackMACCanBeLearned(t *testing.T) {
	l := newEthLink(MAC{1, 2, 3, 4, 5, 6}, IP{10, 3, 4, 2})
	for _, ip := range []IP{{127, 0, 0, 1}, l.localIP} {
		l.remember(ip, MAC{})
		if mac, known := l.lookup(ip); !known || !mac.IsZero() {
			t.Fatal("zero loopback MAC rejected")
		}
	}
	remote := IP{10, 3, 4, 99}
	l.remember(remote, MAC{})
	if _, known := l.lookup(remote); known {
		t.Fatal("zero MAC accepted for a remote host")
	}
}

func TestLoopbackReceiveAliases(t *testing.T) {
	peer := IP{127, 0, 0, 1}
	for _, c := range []struct {
		dst            IP
		ethernet, want bool
	}{
		{IP{127, 0, 0, 2}, true, true},
		{IP{127, 0, 0, 2}, false, false},
		{IP{10, 3, 4, 99}, true, false},
	} {
		w := &echoWaiter{peer: peer, id: 1, seq: 2}
		s := &Stack{localIP: IP{10, 3, 4, 2}, echo: w}
		if c.ethernet {
			s.eth = &ethLink{}
		}
		packet := make([]byte, ipHeaderLen+icmpHeaderLen+len(echoPayload))
		putIPv4(packet, peer, c.dst, protoICMP, len(packet)-ipHeaderLen, 1)
		packet[ipHeaderLen+5], packet[ipHeaderLen+7] = 1, 2
		copy(packet[ipHeaderLen+icmpHeaderLen:], echoPayload)
		s.deliver(packet)
		if w.got != c.want {
			t.Fatalf("destination %v, Ethernet %v: received %v", c.dst, c.ethernet, w.got)
		}
	}
}
