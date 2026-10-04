package model

import (
	"encoding/binary"
	"errors"
	"io"
)

type WAV struct {
	Rate          uint32
	Offset, Bytes uint64
}

// ParseWAV walks RIFF chunks without loading audio. Only the kernel's native
// format is accepted: uncompressed 16-bit stereo with a supported sample rate.
func ParseWAV(r io.ReaderAt) (WAV, error) {
	var out WAV
	var h [40]byte
	if _, err := r.ReadAt(h[:12], 0); err != nil {
		return out, err
	}
	if string(h[:4]) != "RIFF" || string(h[8:12]) != "WAVE" {
		return out, errors.New("not a RIFF WAV file")
	}
	end := uint64(binary.LittleEndian.Uint32(h[4:8])) + 8
	var format bool
	for off, chunks := uint64(12), 0; off+8 <= end && chunks < 1024; chunks++ {
		if _, err := r.ReadAt(h[:8], int64(off)); err != nil {
			return out, err
		}
		size := uint64(binary.LittleEndian.Uint32(h[4:8]))
		if size > end-off-8 {
			return out, errors.New("WAV chunk exceeds RIFF size")
		}
		switch string(h[:4]) {
		case "fmt ":
			if size < 16 {
				return out, errors.New("short WAV format chunk")
			}
			if _, err := r.ReadAt(h[:16], int64(off+8)); err != nil {
				return out, err
			}
			rate := binary.LittleEndian.Uint32(h[4:8])
			if binary.LittleEndian.Uint16(h[:2]) != 1 || binary.LittleEndian.Uint16(h[2:4]) != 2 ||
				binary.LittleEndian.Uint16(h[12:14]) != 4 || binary.LittleEndian.Uint16(h[14:16]) != 16 ||
				binary.LittleEndian.Uint32(h[8:12]) != rate*4 || !SupportedRate(rate) {
				return out, errors.New("WAV must be PCM16 stereo at a supported rate")
			}
			out.Rate, format = rate, true
		case "data":
			if !format || size%4 != 0 {
				return out, errors.New("WAV data has no valid format or whole stereo frames")
			}
			out.Offset, out.Bytes = off+8, size
			return out, nil
		}
		off += 8 + size + size%2
	}
	return out, errors.New("WAV has no audio data")
}

func SupportedRate(rate uint32) bool {
	switch rate {
	case 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000:
		return true
	}
	return false
}
