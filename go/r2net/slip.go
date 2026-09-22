package r2net

// SLIP framing, RFC 1055.  Only the decoder lives here: the kernel encodes
// what it sends for us, inside the send syscall.

const (
	slipEnd    = 0xC0
	slipEsc    = 0xDB
	slipEscEnd = 0xDC
	slipEscEsc = 0xDD
)

// slipDecoder reassembles frames one byte at a time, which is the only rate
// the serial ABI offers.
type slipDecoder struct {
	buf    []byte
	escape bool
}

// feed adds one received byte and, when that byte completes a frame, copies it
// into out and returns its length.  It returns 0 while a frame is still coming
// in and -1 on a framing error, having resynchronised.
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
