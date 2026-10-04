// Package stream implements a bounded native Spotify playback session. Protocol
// field numbers are interoperable with librespot/go-librespot's published schemas.
package stream

import (
	"encoding/binary"
	"errors"
)

type field struct {
	id   int
	num  uint64
	data []byte
}
type message struct {
	fields []field
	err    error
}

func parse(b []byte) message {
	m := message{}
	if len(b) > 65536 {
		m.err = errors.New("protobuf message too large")
		return m
	}
	for len(b) > 0 {
		tag, n := binary.Uvarint(b)
		if n <= 0 || tag>>3 == 0 || tag>>3 > 0x1fffffff {
			m.err = errors.New("invalid protobuf tag")
			break
		}
		b = b[n:]
		f := field{id: int(tag >> 3)}
		switch tag & 7 {
		case 0:
			v, n := binary.Uvarint(b)
			if n <= 0 {
				m.err = errors.New("invalid protobuf integer")
				return m
			}
			f.num = v
			b = b[n:]
		case 2:
			size, n := binary.Uvarint(b)
			if n <= 0 || size > uint64(len(b)-n) {
				m.err = errors.New("truncated protobuf bytes")
				return m
			}
			b = b[n:]
			f.data = b[:int(size)]
			b = b[int(size):]
		case 1, 5:
			size := 8
			if tag&7 == 5 {
				size = 4
			}
			if len(b) < size {
				m.err = errors.New("truncated protobuf fixed field")
				return m
			}
			b = b[size:]
		default:
			m.err = errors.New("unsupported protobuf wire type")
			return m
		}
		m.fields = append(m.fields, f)
		if len(m.fields) > 2048 {
			m.err = errors.New("too many protobuf fields")
			return m
		}
	}
	return m
}
func (m message) bytes(id int) []byte {
	for _, f := range m.fields {
		if f.id == id {
			return f.data
		}
	}
	return nil
}
func (m message) num(id int) uint64 {
	for _, f := range m.fields {
		if f.id == id {
			return f.num
		}
	}
	return 0
}
func (m message) all(id int) [][]byte {
	var out [][]byte
	for _, f := range m.fields {
		if f.id == id {
			out = append(out, f.data)
		}
	}
	return out
}
func (m message) sub(id int) message {
	if m.err != nil {
		return message{err: m.err}
	}
	return parse(m.bytes(id))
}
func vint(v uint64) []byte {
	var a [10]byte
	n := binary.PutUvarint(a[:], v)
	return append([]byte(nil), a[:n]...)
}
func number(id int, v uint64) []byte { return append(vint(uint64(id)<<3), vint(v)...) }
func blob(id int, b []byte) []byte {
	a := append(vint(uint64(id)<<3|2), vint(uint64(len(b)))...)
	return append(a, b...)
}
func text(id int, s string) []byte { return blob(id, []byte(s)) }
func join(parts ...[]byte) []byte {
	var b []byte
	for _, p := range parts {
		b = append(b, p...)
	}
	return b
}
