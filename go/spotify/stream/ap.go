package stream

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/spotify/third_party/shannon"
	"io"
	"time"
)

type Socket interface{ io.ReadWriteCloser }
type Accesspoint struct {
	socket   Socket
	packets  packetConn
	Username string
	Stored   []byte
	seq      uint32
}

func handshake(rw io.ReadWriter, random func([]byte) error) (packetConn, error) {
	var empty packetConn
	var seed [111]byte
	if e := random(seed[:]); e != nil {
		return empty, e
	}
	prime, _ := hex.DecodeString(dhPrimeHex)
	private := seed[:95]
	nonzero := false
	for _, v := range private {
		nonzero = nonzero || v != 0
	}
	if !nonzero {
		return empty, errors.New("empty AP private key")
	}
	public, err := modexp([]byte{2}, private, prime)
	if err != nil {
		return empty, err
	}
	hello := join(blob(10, join(number(10, 0), number(20, 1), number(30, 8), number(40, 127700358))), number(30, 0), blob(50, blob(10, join(blob(10, public), number(20, 1)))), blob(60, seed[95:]), blob(70, []byte{0x1e}))
	packet := make([]byte, 6)
	packet[1] = 4
	binary.BigEndian.PutUint32(packet[2:], uint32(6+len(hello)))
	packet = append(packet, hello...)
	if e := writeAll(rw, packet); e != nil {
		return empty, e
	}
	var header [4]byte
	if _, e := io.ReadFull(rw, header[:]); e != nil {
		return empty, e
	}
	size := binary.BigEndian.Uint32(header[:])
	if size < 4 || size > 65536 {
		return empty, errors.New("invalid AP challenge size")
	}
	response := make([]byte, int(size)-4)
	if _, e := io.ReadFull(rw, response); e != nil {
		return empty, e
	}
	m := parse(response)
	dh := m.sub(10).sub(10).sub(10)
	gs, sig := dh.bytes(10), dh.bytes(30)
	if dh.err != nil || len(gs) == 0 || !verifyAPSignature(gs, sig) {
		return empty, errors.New("AP server signature invalid")
	}
	remote := bytes.TrimLeft(gs, "\x00")
	limit := append([]byte(nil), prime...)
	limit[len(limit)-1]--
	if len(remote) > len(prime) || len(remote) == 0 || (len(remote) == 1 && remote[0] <= 1) || (len(remote) == len(prime) && bytes.Compare(remote, limit) >= 0) {
		return empty, errors.New("invalid AP DH key")
	}
	shared, err := modexp(remote, private, prime)
	if err != nil {
		return empty, err
	}
	shared = bytes.TrimLeft(shared, "\x00")
	transcript := join(packet, header[:], response)
	keys := make([]byte, 0, 100)
	for i := byte(1); i <= 5; i++ {
		h := hmac.New(sha1.New, shared)
		h.Write(transcript)
		h.Write([]byte{i})
		keys = h.Sum(keys)
	}
	h := hmac.New(sha1.New, keys[:20])
	h.Write(transcript)
	reply := join(blob(10, blob(10, blob(10, h.Sum(nil)))), blob(20, nil), blob(30, nil))
	binary.BigEndian.PutUint32(header[:], uint32(4+len(reply)))
	if e := writeAll(rw, join(header[:], reply)); e != nil {
		return empty, e
	}
	return packetConn{rw: rw, send: shannon.New(keys[20:52]), recv: shannon.New(keys[52:84])}, nil
}
func OpenAccesspoint(socket Socket, token, device string, random func([]byte) error) (*Accesspoint, error) {
	refreshSocketDeadline(socket, 60*time.Second)
	pc, e := handshake(socket, random)
	if e != nil {
		socket.Close()
		return nil, e
	}
	refreshSocketDeadline(socket, 30*time.Second)
	a := &Accesspoint{socket: socket, packets: pc}
	credentials := join(number(20, 3), text(30, token))
	sys := join(number(10, 2), number(60, 5), text(90, "rou2exOS Go"), text(100, device))
	if e = a.packets.write(0xab, join(blob(10, credentials), blob(50, sys), text(70, "r2spotify/0.2"))); e != nil {
		a.Close()
		return nil, e
	}
	for i := 0; i < 64; i++ {
		kind, b, e := a.packets.read()
		if e != nil {
			a.Close()
			return nil, e
		}
		if kind == 4 {
			if e = a.packets.write(0x49, b); e != nil {
				a.Close()
				return nil, e
			}
			continue
		}
		m := parse(b)
		if m.err != nil {
			a.Close()
			return nil, m.err
		}
		if kind == 0xac {
			a.Username = string(m.bytes(10))
			a.Stored = append([]byte(nil), m.bytes(40)...)
			if a.Username == "" || len(a.Stored) == 0 {
				a.Close()
				return nil, errors.New("invalid AP welcome")
			}
			return a, nil
		}
		if kind == 0xad {
			a.Close()
			return nil, fmt.Errorf("AP login rejected (%d); reauthorize with streaming scope", m.num(10))
		}
	}
	a.Close()
	return nil, errors.New("AP login response limit exceeded")
}
func (a *Accesspoint) Close() error { return a.socket.Close() }
func (a *Accesspoint) AudioKey(gid, file []byte) ([]byte, error) {
	if len(gid) != 16 || len(file) != 20 {
		return nil, errors.New("invalid audio key request IDs")
	}
	// The AP may have been idle through several HTTPS metadata/login requests.
	// Its previous operation deadline must not expire this new key request.
	refreshSocketDeadline(a.socket, 30*time.Second)
	seq := a.seq
	a.seq++
	b := join(file, gid)
	var tail [6]byte
	binary.BigEndian.PutUint32(tail[:4], seq)
	b = append(b, tail[:]...)
	if e := a.packets.write(0x0c, b); e != nil {
		return nil, e
	}
	for count := 0; count < 256; count++ {
		kind, p, e := a.packets.read()
		if e != nil {
			return nil, e
		}
		if kind == 4 {
			if e = a.packets.write(0x49, p); e != nil {
				return nil, e
			}
			continue
		}
		if kind != 0x0d && kind != 0x0e {
			continue
		}
		if len(p) < 4 {
			return nil, errors.New("truncated audio key response")
		}
		if binary.BigEndian.Uint32(p[:4]) != seq {
			continue
		}
		if kind == 0x0e {
			return nil, errors.New("Spotify denied the audio key (Premium/availability/session)")
		}
		if len(p) != 20 {
			return nil, errors.New("invalid audio key length")
		}
		return append([]byte(nil), p[4:]...), nil
	}
	return nil, errors.New("audio key response limit exceeded")
}

// r2net uses relative deadlines; host diagnostics keep their net.Conn deadline.
func refreshSocketDeadline(socket Socket, timeout time.Duration) {
	if c, ok := socket.(interface{ SetDeadline(time.Duration) }); ok {
		c.SetDeadline(timeout)
	}
}
