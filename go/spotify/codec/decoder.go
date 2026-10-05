// Package codec decodes Ogg Vorbis to PCM16 stereo using Xiph's integer Tremor
// decoder. Its r2 allocations use the process-owned kernel heap, not Go's arena.
package codec

/*
#cgo LDFLAGS: -L./build -lvorbisr2
#include "decoder.h"
*/
import "C"
import (
	"errors"
	"io"
	"unsafe"
)

type Reader struct {
	source  io.ReaderAt
	decoder *C.r2v_decoder
	offset  int64
	eof     bool
	buffer  [32768]byte
	Rate    uint32
}

func Open(source io.ReaderAt) (*Reader, error) {
	r := &Reader{source: source, decoder: C.r2v_new()}
	if r.decoder == nil {
		return nil, errors.New("Vorbis decoder allocation failed")
	}
	success := false
	defer func() {
		if !success {
			r.Close()
		}
	}()
	// Spotify prepends an Ogg metadata page (first packet type 0x81). Skip that
	// page, preserving byte offsets into the encrypted file for AES-CTR.
	var header [27]byte
	if _, e := source.ReadAt(header[:], 0); e != nil {
		return nil, e
	}
	if string(header[:4]) != "OggS" {
		return nil, errors.New("audio is not Ogg Vorbis")
	}
	var laces [255]byte
	n := int(header[26])
	if _, e := source.ReadAt(laces[:n], 27); e != nil {
		return nil, e
	}
	body := 0
	for _, v := range laces[:n] {
		body += int(v)
	}
	if body > 0 {
		var first [1]byte
		if _, e := source.ReadAt(first[:], int64(27+n)); e != nil {
			return nil, e
		}
		if first[0] == 0x81 {
			r.offset = int64(27 + n + body)
		}
	}
	var probe [4]byte
	for attempts := 0; attempts < 64; attempts++ {
		if e := r.feed(); e != nil {
			return nil, e
		}
		if C.r2v_read(r.decoder, (*C.uchar)(unsafe.Pointer(&probe[0])), 4) < 0 {
			return nil, errors.New("invalid Vorbis headers")
		}
		if rate := C.r2v_rate(r.decoder); rate > 0 {
			r.Rate = uint32(rate)
			success = true
			return r, nil
		}
	}
	return nil, errors.New("Vorbis headers exceed input limit")
}
func (r *Reader) feed() error {
	if r.eof {
		return io.EOF
	}
	input := r.buffer[:]
	// Parse headers incrementally instead of waiting for 32 KiB before startup.
	if r.Rate == 0 {
		input = input[:4096]
	}
	n, e := r.source.ReadAt(input, r.offset)
	if n < 0 || n > len(input) {
		return errors.New("invalid compressed audio count")
	}
	if e != nil && e != io.EOF {
		return e
	}
	if e == io.EOF {
		r.eof = true
	}
	if n == 0 {
		return io.EOF
	}
	r.offset += int64(n)
	if C.r2v_feed(r.decoder, (*C.uchar)(unsafe.Pointer(&r.buffer[0])), C.int(n)) < 0 {
		return errors.New("Vorbis input buffer failed")
	}
	return nil
}
func (r *Reader) Read(p []byte) (int, error) {
	if r.decoder == nil {
		return 0, errors.New("Vorbis reader is closed")
	}
	if len(p) == 0 {
		return 0, nil
	}
	if len(p) < 4 {
		return 0, io.ErrShortBuffer
	}
	filled := 0
	limit := len(p) / 4 * 4
	for filled < limit {
		n := int(C.r2v_read(r.decoder, (*C.uchar)(unsafe.Pointer(&p[filled])), C.int(limit-filled)))
		if n < 0 {
			return filled, errors.New("invalid Vorbis audio packet")
		}
		if n > 0 {
			filled += n
			continue
		}
		// Return already decoded PCM before an input read can block.
		if filled > 0 {
			return filled, nil
		}
		if e := r.feed(); e != nil {
			return 0, e
		}
	}
	return filled, nil
}
func (r *Reader) Close() error {
	if r.decoder != nil {
		C.r2v_close(r.decoder)
		r.decoder = nil
	}
	r.source = nil
	return nil
}

// MemoryStats reports decoder allocations outside Go's collector.
func MemoryStats() (used, peak uint64) {
	var live, high C.r2v_size_t
	C.r2v_memory(&live, &high)
	return uint64(live), uint64(high)
}
