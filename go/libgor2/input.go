package libgor2

import "unsafe"

// Keyboard and mouse events reach a process through the kernel's pipe
// mechanism (syscall 0x03): the process registers a buffer, the kernel fills
// it as events arrive, and the process drains it.
//
// The kernel keeps hold of the buffer, so it has to stay reachable for as long
// as the subscription lasts.  Keep it in a package-level variable, or in
// something that outlives the subscription --- a buffer that only the kernel
// still points at is not one the collector knows about.

// KeyboardSubscribe registers buf to receive keystrokes (syscall 0x03, op 0x01).
func KeyboardSubscribe(buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScPipeSubscribe, 0x01, ptr(unsafe.Pointer(&buf[0]))))
}

// KeyboardUnsubscribe stops the subscription (syscall 0x03, op 0x02).
func KeyboardUnsubscribe(buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScPipeSubscribe, 0x02, ptr(unsafe.Pointer(&buf[0]))))
}

// KeyboardRead drains pending keystrokes into buf (syscall 0x03, op 0x03).
func KeyboardRead(buf []byte) error {
	if len(buf) == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScPipeSubscribe, 0x03, ptr(unsafe.Pointer(&buf[0]))))
}

// MouseSubscribe asks for PS/2 mouse packets (syscall 0x03, op 0x04).  It
// fails when the kernel's subscriber table is full.
func MouseSubscribe() error {
	return err(Syscall(ScPipeSubscribe, 0x04, 0))
}

// MouseUnsubscribe stops the subscription (syscall 0x03, op 0x06).
func MouseUnsubscribe() error {
	return err(Syscall(ScPipeSubscribe, 0x06, 0))
}

// MouseRead drains up to five queued mouse packets (syscall 0x03, op 0x05).
func MouseRead() []MousePacket {
	var buf [5]MousePacket

	n := int(Syscall(ScPipeSubscribe, 0x05, ptr(unsafe.Pointer(&buf[0]))))

	// The kernel answers in bytes, and a packet is three of them.
	n /= int(unsafe.Sizeof(MousePacket{}))
	if n <= 0 || n > len(buf) {
		return nil
	}

	out := make([]MousePacket, n)
	copy(out, buf[:n])

	return out
}
