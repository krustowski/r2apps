// Package r2net is a TCP/IP stack for Go programs on rou2exOS.
//
// The kernel offers a wire and nothing above it: raw IPv4 packets over a
// serial line, or raw Ethernet frames from the NIC.  There is no socket, no
// name resolution and no connection state anywhere in the kernel, so a program
// that wants to speak to a server has to bring all of it.  This package is
// that --- ARP, IPv4, ICMP, UDP, DNS, TCP and an HTTP/1.0 client --- and it is
// to Go what the TCP/IP half of c/libcr2 is to C.
//
//	stack, err := r2net.Open(r2net.Options{Link: "eth", LocalIP: ip})
//	if err != nil {
//		return err
//	}
//	defer stack.Close()
//
//	res, err := stack.Get("http://10.3.4.1/health", nil, 10*time.Second)
//
// # One goroutine
//
// The stack is a single-threaded multiplexer: nothing in it is safe to call
// from two goroutines, and it does not start any.  Every blocking call ---
// Dial, Read, Ping, Resolve --- turns into the same loop over the link, which
// drains what has arrived, answers what the link itself owns (ARP, echo
// requests), advances every open connection's timers and sleeps a millisecond
// when the wire is quiet.  That is a deliberate choice rather than a
// limitation of what TinyGo can do: a goroutine on r2 commits 32 KiB of stack
// before it runs once, the kernel delivers frames one at a time through a
// single shared buffer, and a cooperative scheduler will not preempt a
// goroutine that is spinning on a syscall anyway.  Connections can be open at
// the same time --- the demultiplexer keys on the four-tuple --- but they are
// driven from one place.
//
// # The Ethernet driver registration
//
// The kernel polls the NIC only on behalf of the one process that registered
// as the global Ethernet driver (syscall 0x37), and it delivers everything
// that is not claimed by a bound TCP port to that process.  Open registers
// when nobody else has, and the stack then answers ARP and ICMP for the whole
// machine for as long as the program runs.  When a driver is already
// registered --- eth.elf, garn --- Open falls back to binding the TCP ports it
// needs, which is enough for TCP and HTTP but not for ICMP or DNS, because
// those arrive as something other than a TCP segment and go to the driver.
// CanICMP reports which of the two happened.
//
// The registration is never released, not even when the process exits, so a
// program that registers and then exits leaves the machine without an ARP
// responder until the next boot.
package r2net

import (
	"errors"
	"fmt"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// Errors returned by this package.  They are values rather than formatted
// strings so that a caller can tell a timeout from a refusal.
var (
	ErrTimeout    = errors.New("timed out")
	ErrTooLong    = errors.New("packet too long for the link")
	ErrClosed     = errors.New("connection closed")
	ErrRefused    = errors.New("connection refused")
	ErrReset      = errors.New("connection reset by peer")
	ErrNoICMP     = errors.New("ICMP needs the global Ethernet driver, which another process holds")
	ErrNoDatagram = errors.New("UDP needs the global Ethernet driver, which another process holds")
	ErrNoServer   = errors.New("no DNS server configured")
	ErrNoAnswer   = errors.New("name not found")
)

// defaultMAC is the address QEMU gives an RTL8139 and the one c/libcr2 assumes.
// It is only used when the kernel cannot tell us the real one.
var defaultMAC = MAC{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}

// defaultIP is the address this repository's Ethernet setup gives the guest,
// and what c/libcr2 compiles in.  r2 has no DHCP client of its own and no
// netmask anywhere in the ABI, so something has to be assumed.
var (
	defaultIP      = IP{10, 3, 4, 2}
	defaultNetmask = IP{255, 255, 255, 0}
)

// rxBudget is how many frames one turn of the loop takes off the queue before
// the timers get a look in, so that a burst cannot starve a retransmission.
const rxBudget = 8

// Options configures a stack.  The zero value asks for Ethernet with
// everything guessed, which is right on the machines this repository sets up
// and wrong on any other, so a caller with a configuration file should pass
// what it was told.
type Options struct {
	// Link is "eth" (the default) or "slip".
	Link string

	// LocalIP is this machine's address.  When it is zero the kernel's
	// system information block is consulted, and failing that 10.3.4.2 is
	// assumed.
	LocalIP IP

	// Netmask decides which destinations are on the local link.  Zero means
	// 255.255.255.0.
	Netmask IP

	// Gateway is where packets for everywhere else are sent.  Zero means
	// the .1 of the local network.
	Gateway IP

	// DNS is the resolver Resolve should ask.  Zero disables resolution.
	DNS IP

	// Trace, when set, is called with one line per packet sent or
	// received.  It is a function rather than a flag because the
	// application owns the question of where a log line goes --- on this
	// machine that may be the VGA console, the serial port, or both --- and
	// a stack that printed for itself would have to know.
	Trace func(string)
}

// Stack is an open network stack.  Create one with Open.
type Stack struct {
	link link
	eth  *ethLink // nil on SLIP

	localIP IP
	netmask IP
	gateway IP
	dns     IP

	driver bool // we hold the global Ethernet driver registration
	trace  func(string)

	conns []*Conn

	echo  *echoWaiter
	dgram *dgramWaiter

	cache []resolved

	// Buffers reused across every packet.  The ABI takes an address as an
	// integer, and a local whose address goes that way is moved to the heap
	// on every call; a field is not.
	rxBuf [frameLen]byte
	txBuf [frameLen]byte

	ipID     uint16
	nextPort uint16
	bound    map[uint16]bool
}

// Open brings up a stack on the requested link.
func Open(opts Options) (*Stack, error) {
	s := &Stack{
		localIP:  opts.LocalIP,
		netmask:  opts.Netmask,
		gateway:  opts.Gateway,
		dns:      opts.DNS,
		trace:    opts.Trace,
		ipID:     uint16(libgor2.Ticks()),
		nextPort: ephemeralBase,
		bound:    make(map[uint16]bool),
	}

	if s.netmask.IsZero() {
		s.netmask = defaultNetmask
	}

	if s.localIP.IsZero() {
		s.localIP = localAddressFromKernel()
	}

	if s.gateway.IsZero() {
		// The .1 of our own network: a guess, but the right one on every
		// setup this repository describes, and better than dropping the
		// packet on the floor.
		for i := 0; i < 4; i++ {
			s.gateway[i] = s.localIP[i] & s.netmask[i]
		}

		s.gateway[3] |= 1
	}

	switch opts.Link {
	case "slip":
		l, err := newSLIPLink()
		if err != nil {
			return nil, err
		}

		s.link = l

		// A serial line has one host at the other end and no addressing,
		// so every destination is reached the same way.
		s.driver = true

	default:
		var ns libgor2.NetStatus
		if err := libgor2.ReadNetStatus(&ns); err != nil {
			return nil, err
		}

		if ns.DrvActive == 0 {
			// Nobody is driving the NIC, so the kernel is not even
			// polling it.  Take the job: it is the only way to get
			// ARP and ICMP, and the kernel initialises the card as
			// a side effect.
			if err := libgor2.NetRegister(); err != nil {
				return nil, err
			}

			s.driver = true

			// The kernel reads the card's address when the driver
			// registers, so this is the first moment it knows it.
			_ = libgor2.ReadNetStatus(&ns)
		}

		mac := MAC(ns.MAC)
		if mac.IsZero() {
			mac = defaultMAC
		}

		eth := newEthLink(mac, s.localIP)

		s.eth = eth
		s.link = eth
	}

	s.logf("link %s, ip %v, mask %v, gw %v, driver %v",
		s.link.name(), s.localIP, s.netmask, s.gateway, s.driver)

	return s, nil
}

// localAddressFromKernel reads the address the Ethernet driver published, if
// one ever did.
func localAddressFromKernel() IP {
	var info libgor2.SysInfo
	if err := libgor2.ReadSysInfo(&info); err == nil {
		if ip := IP(info.IP); !ip.IsZero() {
			return ip
		}
	}

	var ns libgor2.NetStatus
	if err := libgor2.ReadNetStatus(&ns); err == nil {
		if ip := IP(ns.IP); !ip.IsZero() {
			return ip
		}
	}

	return defaultIP
}

// Close releases what can be released.  The Ethernet driver registration is
// not one of those things --- the kernel holds it until the machine reboots
// --- so this only drops the connections.
func (s *Stack) Close() {
	// Over a copy: Close takes each connection off this same list.
	open := make([]*Conn, len(s.conns))
	copy(open, s.conns)

	for _, c := range open {
		if c.state != stateClosed {
			c.Close()
		}
	}

	s.conns = nil
}

// LocalIP is the address this stack sends from.
func (s *Stack) LocalIP() IP { return s.localIP }

// Link is the name of the link layer in use, "eth" or "slip".
func (s *Stack) Link() string { return s.link.name() }

// CanICMP reports whether ICMP echo and UDP --- and so Ping and Resolve ---
// can work.  They cannot when another process holds the Ethernet driver
// registration, because the kernel delivers everything that is not a bound TCP
// port to that process instead of to this one.
func (s *Stack) CanICMP() bool { return s.driver }

// nextHop is the address whose hardware address a packet for dst should be
// aimed at: the destination itself on the local link, the gateway otherwise.
func (s *Stack) nextHop(dst IP) IP {
	if sameSubnet(dst, s.localIP, s.netmask) {
		return dst
	}

	return s.gateway
}

// sendIP builds a header around payload and puts the packet on the wire.  The
// payload has to already be sitting in s.txBuf at ipHeaderLen.
func (s *Stack) sendIP(dst IP, proto byte, payloadLen int) error {
	s.ipID++

	putIPv4(s.txBuf[:], s.localIP, dst, proto, payloadLen, s.ipID)

	return s.link.send(s.txBuf[:ipHeaderLen+payloadLen], s.nextHop(dst))
}

// step is one turn of the stack: take what has arrived, run the timers, and
// give the CPU back for a tick.
//
// How much arrives per turn is not this loop's decision.  The kernel takes one
// frame off the NIC per timer tick, copies it into a single global buffer and
// queues a message pointing at that buffer, so a message that is not read
// before the next tick hands back whatever frame arrived last instead of the
// one it was queued for.  Meanwhile the scheduler is a strict round robin that
// gives every runnable process exactly one tick, so how often this loop gets to
// look is a property of how many other processes are running, and no amount of
// spinning here changes it --- measured, spinning made a run slower, not
// faster.
//
// So frames are lost in bursts, and the answer is not to poll harder but to
// recover quickly: see signalGap in tcp.go, which turns a gap into three
// duplicate acknowledgements and the peer's retransmission timer into one round
// trip.  With that in place a sleep of one tick here costs nothing and leaves
// the machine to everybody else.
func (s *Stack) step() {
	for i := 0; i < rxBudget; i++ {
		n := s.link.recv(s.rxBuf[:])
		if n <= 0 {
			break
		}

		s.deliver(s.rxBuf[:n])
	}

	now := libgor2.Ticks()

	for _, c := range s.conns {
		c.tick(now)
	}

	libgor2.SleepMS(1)
}

// waitUntil turns the loop until cond is true or the deadline passes.
func (s *Stack) waitUntil(cond func() bool, deadline uint64) error {
	for {
		if cond() {
			return nil
		}

		if libgor2.Ticks() >= deadline {
			return ErrTimeout
		}

		s.step()
	}
}

// deliver hands one received IPv4 packet to whatever is waiting for it.
func (s *Stack) deliver(raw []byte) {
	pkt, ok := parseIPv4(raw)
	if !ok {
		return
	}

	// On Ethernet the card may hand us frames for other hosts; on SLIP the
	// far end may be talking to an address we do not have.  Either way, a
	// packet that is not ours is not ours.
	if !s.localIP.IsZero() && pkt.dst != s.localIP && !pkt.dst.IsBroadcast() {
		return
	}

	s.logf("rx %v -> %v proto %d, %d bytes", pkt.src, pkt.dst, pkt.proto, len(pkt.payload))

	switch pkt.proto {
	case protoICMP:
		s.onICMP(pkt)

	case protoTCP:
		s.onTCP(pkt)

	case protoUDP:
		s.onUDP(pkt)
	}
}

// deadlineFor turns a timeout into a tick count, treating a non-positive
// timeout as "one second", because a deadline in the past would make every
// call fail before it started.
func deadlineFor(timeout time.Duration) uint64 {
	ms := timeout.Milliseconds()
	if ms <= 0 {
		ms = 1000
	}

	return libgor2.Ticks() + uint64(ms)
}

// logf hands one trace line to the application, when it asked for them.
func (s *Stack) logf(format string, v ...any) {
	if s.trace == nil {
		return
	}

	s.trace(fmt.Sprintf(format, v...))
}
