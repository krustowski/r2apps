package r2net

import (
	"bytes"
	"io"
	"testing"
)

type fragmented struct {
	read    *bytes.Reader
	written bytes.Buffer
}

func (f *fragmented) Read(b []byte) (int, error) {
	if len(b) > 1 {
		b = b[:1]
	}
	return f.read.Read(b)
}
func (f *fragmented) Write(b []byte) (int, error) {
	if len(b) > 2 {
		b = b[:2]
	}
	return f.written.Write(b)
}
func TestDNSStreamFragmentation(t *testing.T) {
	f := &fragmented{read: bytes.NewReader(append([]byte{0, 12}, make([]byte, 12)...))}
	answer := make([]byte, 16)
	n, err := dnsStreamExchange(f, []byte{1, 2, 3}, answer)
	if err != nil || n != 12 || !bytes.Equal(f.written.Bytes(), []byte{0, 3, 1, 2, 3}) {
		t.Fatalf("%d %v %v", n, err, f.written.Bytes())
	}
}
func TestDNSStreamBounds(t *testing.T) {
	for _, wire := range [][]byte{{0, 11}, {0, 17}, {0, 12, 1}} {
		f := &fragmented{read: bytes.NewReader(wire)}
		if _, err := dnsStreamExchange(f, []byte{1}, make([]byte, 16)); err == nil {
			t.Fatal("invalid length/truncation accepted")
		}
	}
}

var _ io.ReadWriter = (*fragmented)(nil)
