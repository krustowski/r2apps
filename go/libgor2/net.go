package libgor2

import "unsafe"

// Packet kinds. ICMP and TCP select header construction in NewPacket;
// SendPacket transmits IPv4 packets or raw Ethernet frames.
const (
	PacketIPv4 = 0x01 // length taken from the IPv4 header
	PacketICMP = 0x02 // header construction through NewPacket
	PacketTCP  = 0x03 // header construction through NewPacket
	PacketEth  = 0x04 // raw Ethernet frame; length derived from the ethertype
)

// Scratch cells for the syscalls whose argument is a pointer to one scalar.
//
// These are package-level on purpose, and it matters more than it looks.
// Taking the address of a local and handing it to the kernel as an integer
// makes TinyGo's escape analysis move that local to the heap --- on every
// call, because it cannot see where the integer goes.  Serial and port I/O are
// used in polling loops, so that is one allocation per iteration and a
// collection every few thousand; a program written that way spends its life in
// the garbage collector and never gets round to reading the port.  A fixed
// cell in .bss costs nothing and is always at an address the ABI accepts.
//
// None of this is re-entrant, which is safe here: the scheduler is
// cooperative, and a syscall is not a yield point, so no other goroutine can
// run between filling a cell and the syscall that reads it.
var (
	portCell  uint16
	valueCell uint32
	byteCell  uint32

	// sendCell pads a short message for Send, which always reads 512 bytes.
	sendCell [msgLen]byte
)

// msgLen is the size of a message between processes: Send reads exactly this
// many bytes, and NewPacket clears this many of its buffer.
const msgLen = 512

// WritePort writes a byte to an I/O port (syscall 0x30).
//
// The kernel narrows the value to 8 bits on purpose: the VGA registers are
// byte-wide, and a 32-bit write there would be decomposed into four
// consecutive byte writes and corrupt the neighbouring registers.
func WritePort(port uint16, value uint32) error {
	portCell, valueCell = port, value

	return err(Syscall(ScWritePort,
		ptr(unsafe.Pointer(&portCell)),
		ptr(unsafe.Pointer(&valueCell))))
}

// ReadPort reads a 32-bit value from an I/O port (syscall 0x31).
func ReadPort(port uint16) (uint32, error) {
	portCell = port

	if e := err(Syscall(ScReadPort,
		ptr(unsafe.Pointer(&portCell)),
		ptr(unsafe.Pointer(&valueCell)))); e != nil {
		return 0, e
	}

	return valueCell, nil
}

// SerialInit brings up the UART (syscall 0x32, op 0x01).
func SerialInit() error {
	return err(Syscall(ScSerialPort, 0x01, 0))
}

// SerialRead takes one byte from the UART.  The second result is false when no
// byte was ready, which the kernel reports as an error rather than a value.
func SerialRead() (byte, bool) {
	if Syscall(ScSerialPort, 0x02, ptr(unsafe.Pointer(&byteCell))) != 0 {
		return 0, false
	}

	return byte(byteCell), true
}

// SerialWrite puts one byte on the UART.
//
// The kernel reads the byte through a pointer, not as a value --- note that
// c/libcr2's serial_write() passes the value itself and so writes whatever
// happens to live at that address, when the range check lets it through at all.
func SerialWrite(b byte) error {
	byteCell = uint32(b)

	return err(Syscall(ScSerialPort, 0x03, ptr(unsafe.Pointer(&byteCell))))
}

// NewPacket fills in the headers of a packet of the given kind, in place
// (syscall 0x33).  buf must already hold a header skeleton.
//
// The kernel clears the first 512 bytes of buf before writing the packet it
// built back, so anything shorter is refused here rather than written past.
func NewPacket(kind uint8, buf []byte) error {
	if len(buf) < msgLen {
		return EInvalidInput
	}

	return err(Syscall(ScNewPacket, uintptr(kind), ptr(unsafe.Pointer(&buf[0]))))
}

// SendPacket transmits a fully formed packet (syscall 0x34).
//
// The length is taken from the packet itself --- the IPv4 total_length field
// for PacketIPv4, and the ethertype for PacketEth --- so buf must carry a
// header the kernel can believe. Raw Ethernet frames addressed to 127.x or
// this machine's IP use the kernel's loopback device. TCP is routed by
// destination port; local ICMP echo replies go back to the sending process.
func SendPacket(kind uint8, buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScSendPacket, uintptr(kind), ptr(unsafe.Pointer(&buf[0]))))
}

// Receive waits for a frame and copies it into buf, returning its length
// (syscall 0x35).
//
// Every queued frame has a kernel buffer of its own until it is read, and up
// to 64 wait in a process's queue, so frames that arrive while the process is
// busy are kept rather than overwritten.  The kernel copies as many bytes as
// the frame is long (up to 2048), or a fixed 512 when the queued message
// carries no length, without being told how big buf is, so buf should be 2048
// bytes.  When the queue is empty the
// process is suspended; on being woken it returns 0, so a caller that must
// have a frame should loop.
func Receive(kind uint8, buf []byte) int {
	if len(buf) == 0 {
		return 0
	}

	// arg1 == 0 means non-blocking, so a blocking read cannot pass kind 0.
	if kind == 0 {
		kind = 1
	}

	return int(Syscall(ScReceivePort, uintptr(kind), ptr(unsafe.Pointer(&buf[0]))))
}

// ReceiveNonBlocking is Receive without the wait: it returns 0 straight away
// when nothing is queued.
func ReceiveNonBlocking(buf []byte) int {
	if len(buf) == 0 {
		return 0
	}

	return int(Syscall(ScReceivePort, 0, ptr(unsafe.Pointer(&buf[0]))))
}

// Send pushes a 512-byte message onto another process's queue and wakes it
// (syscall 0x36).  pid is the target process.
//
// The kernel always reads 512 bytes, so a shorter buf is padded with zeroes
// and a longer one is cut to 512.
func Send(pid uint64, buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	if len(buf) < msgLen {
		n := copy(sendCell[:], buf)
		clear(sendCell[n:])

		buf = sendCell[:]
	}

	return err(Syscall(ScSendPort, uintptr(pid), ptr(unsafe.Pointer(&buf[0]))))
}

// NetRegister makes this process the global Ethernet driver: the kernel brings
// up a supported NIC (RTL8139 or E1000) and delivers every frame no port binding
// has claimed, including ARP and ICMP, here (syscall 0x37). Registration and
// port bindings are released when the process exits, is killed or crashes.
func NetRegister() error {
	return err(Syscall(ScNetRegister, 0, 0))
}

// NetBindPort asks for TCP frames addressed to one port (syscall 0x37).  The
// global Ethernet driver has to be running already to handle ARP and ICMP.
func NetBindPort(port uint16) error {
	if port == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScNetRegister, uintptr(port), 0))
}

// NetUnbindPort releases this process's TCP port binding (syscall 0x37).
func NetUnbindPort(port uint16) error {
	if port == 0 {
		return EInvalidInput
	}
	return err(Syscall(ScNetRegister, uintptr(port), 1))
}

// ReadNetConfig fills cfg with the network configuration the Ethernet driver
// published: address, netmask, gateway and its MAC, DNS (syscall 0x3d).  An
// error on a kernel without the syscall.
func ReadNetConfig(cfg *NetConfig) error {
	return err(Syscall(ScNetConfig, 0x01, ptr(unsafe.Pointer(cfg))))
}

// WriteNetConfig publishes the network configuration (syscall 0x3d, op 0x02).
// Only the registered global Ethernet driver may set it. MAC is ignored;
// EBusy means retry because the system configuration is locked.
func WriteNetConfig(cfg *NetConfig) error {
	return err(Syscall(ScNetConfig, 0x02, ptr(unsafe.Pointer(cfg))))
}

// ReadNetStatus fills ns with the MAC, the IP, whether the driver is up and
// the table of bound ports (syscall 0x38).
func ReadNetStatus(ns *NetStatus) error {
	return err(Syscall(ScNetStatus, ptr(unsafe.Pointer(ns)), 0))
}

// ReadNetStats reads physical NIC totals and their sample timestamp (0x45).
// Rates in bytes/second are delta bytes * 1000 / delta TimestampMS.
// An older kernel returns EInvalidSyscall. Nil output is EInvalidInput.
func ReadNetStats(stats *NetStats) error {
	if stats == nil {
		return EInvalidInput
	}
	return err(Syscall(ScNetStats, ptr(unsafe.Pointer(stats)), 0))
}

// ReadNetPorts reads TCP owners and the global driver's PID (0x46).
// EBusy means retry; on an error the output is untouched.
func ReadNetPorts(table *NetPortTable) error {
	if table == nil {
		return EInvalidInput
	}
	return err(Syscall(ScNetPorts, ptr(unsafe.Pointer(table)), 0))
}

// NetPortOwner returns a current binding, false when port is unbound, and an
// error if the kernel is busy or does not support the registry syscall.
func NetPortOwner(port uint16) (NetPortBinding, bool, error) {
	if port == 0 {
		return NetPortBinding{}, false, EInvalidInput
	}
	var table NetPortTable
	if e := ReadNetPorts(&table); e != nil {
		return NetPortBinding{}, false, e
	}
	if table.NPorts > 16 {
		return NetPortBinding{}, false, EInvalidInput
	}
	for i := uint8(0); i < table.NPorts; i++ {
		if table.Bindings[i].Port == port {
			return table.Bindings[i], true, nil
		}
	}
	return NetPortBinding{}, false, nil
}
