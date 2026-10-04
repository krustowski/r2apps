package stream

import (
	"encoding/binary"
	"errors"
	"github.com/krustowski/rou2exOS-apps/go/spotify/third_party/shannon"
	"io"
)

type packetConn struct {
	rw         io.ReadWriter
	send, recv *shannon.Shannon
	sn, rn     uint32
}

func writeAll(w io.Writer, b []byte) error {
	for len(b) > 0 {
		n, e := w.Write(b)
		if n < 0 || n > len(b) {
			return io.ErrShortWrite
		}
		b = b[n:]
		if e != nil {
			return e
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}
func (c *packetConn) write(kind byte, b []byte) error {
	if len(b) > 65535 {
		return errors.New("AP packet too large")
	}
	p := make([]byte, len(b)+3)
	p[0] = kind
	binary.BigEndian.PutUint16(p[1:3], uint16(len(b)))
	copy(p[3:], b)
	c.send.NonceU32(c.sn)
	c.sn++
	c.send.Encrypt(p)
	var mac [4]byte
	c.send.Finish(mac[:])
	if e := writeAll(c.rw, p); e != nil {
		return e
	}
	return writeAll(c.rw, mac[:])
}
func (c *packetConn) read() (byte, []byte, error) {
	c.recv.NonceU32(c.rn)
	c.rn++
	var h [3]byte
	if _, e := io.ReadFull(c.rw, h[:]); e != nil {
		return 0, nil, e
	}
	c.recv.Decrypt(h[:])
	b := make([]byte, int(binary.BigEndian.Uint16(h[1:])))
	if _, e := io.ReadFull(c.rw, b); e != nil {
		return 0, nil, e
	}
	c.recv.Decrypt(b)
	var mac [4]byte
	if _, e := io.ReadFull(c.rw, mac[:]); e != nil {
		return 0, nil, e
	}
	if e := c.recv.CheckMac(mac[:]); e != nil {
		return 0, nil, errors.New("AP packet authentication failed")
	}
	return h[0], b, nil
}
