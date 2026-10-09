package media

/*
#cgo LDFLAGS: -L./build -ltgmedia
#include "media.h"
*/
import "C"

import "unsafe"

func cbytes(b []byte) *C.uchar { return (*C.uchar)(unsafe.Pointer(&b[0])) }

// PictureSize reads only the header: false when it is not a picture.
func PictureSize(data []byte) (int, int, bool) {
	if len(data) == 0 || len(data) > 0x7fffffff {
		return 0, 0, false
	}
	var w, h C.int
	ok := C.tg_picture_info(cbytes(data), C.int(len(data)), &w, &h) != 0
	return int(w), int(h), ok
}

func failure() error {
	if C.GoString(C.tg_failure()) == "outofmem" {
		return ErrNoMemory
	}
	return nil
}

// DecodePicture decodes a PNG, JPEG, GIF (its first frame) or BMP to fit
// maxW x maxH.  colours is 16 or 256, bg the colour under transparent pixels
// as 0xRRGGBB.
func DecodePicture(data []byte, maxW, maxH, colours int, bg uint32) (Picture, error) {
	w, h, ok := PictureSize(data)
	if !ok {
		return Picture{}, ErrNotPicture
	}
	// Six million pixels is 24 MiB decoded, before it is made smaller.
	if int64(w)*int64(h) > 6_000_000 {
		return Picture{}, ErrTooBig
	}
	tw, th := FitSize(w, h, maxW, maxH)
	px := alloc(tw * th)
	if px == nil {
		return Picture{}, ErrNoMemory
	}
	var cw, ch C.int
	rgba := C.tg_picture_rgba(cbytes(data), C.int(len(data)), &cw, &ch)
	if rgba == nil {
		release(px)
		if err := failure(); err != nil {
			return Picture{}, err
		}
		return Picture{}, ErrUnreadable
	}
	w, h = int(cw), int(ch)
	dither(unsafe.Slice((*byte)(unsafe.Pointer(rgba)), w*h*4), w, h, tw, th, colours, bg, px)
	C.tg_free(unsafe.Pointer(rgba))
	return Picture{W: tw, H: th, Px: px}, nil
}

// DecodeAnimation is DecodePicture for every frame of a GIF, within budget
// bytes of frames.  One frame is an animation too: it just does not move.
func DecodeAnimation(data []byte, maxW, maxH, colours int, bg uint32, budget int) (Animation, error) {
	w, h, ok := PictureSize(data)
	if !IsGIF(data) || !ok {
		return Animation{}, ErrNotGIF
	}
	// While it is read, stb keeps the picture four times over at four bytes
	// a pixel and once at one: a million pixels is 17 MiB.
	if int64(w)*int64(h) > 1_000_000 {
		return Animation{}, ErrAnimTooBig
	}
	it := C.tg_gif_open(cbytes(data), C.int(len(data)))
	if it == nil {
		return Animation{}, ErrNoMemory
	}
	defer C.tg_gif_close(it)
	b := NewBuilder(maxW, maxH, colours, bg, budget)
	frames := 0
	for {
		var fw, fh, delay C.int
		rgba := C.tg_gif_next(it, &fw, &fh, &delay)
		if rgba == nil {
			break
		}
		frames++
		pixels := unsafe.Slice((*byte)(unsafe.Pointer(rgba)), int(fw)*int(fh)*4)
		if !b.Add(pixels, int(fw), int(fh), shownFor(int(delay))) {
			break
		}
	}
	if b.Err() != nil || frames == 0 {
		b.Cancel()
		if b.Err() != nil {
			return Animation{}, b.Err()
		}
		if err := failure(); err != nil {
			return Animation{}, err
		}
		return Animation{}, ErrGIFBroken
	}
	return b.Finish()
}

// The H.264 decoder of the bridge, for mp4.go.
type h264 struct{ p unsafe.Pointer }

func openH264(file []byte) (h264, bool) {
	p := C.tg_h264_open(cbytes(file), C.ulong(len(file)))
	return h264{unsafe.Pointer(p)}, p != nil
}

// feed decodes the NAL unit at file[off:off+n]: pictures finished, -1 when
// out of memory.
func (d h264) feed(off, n int) int {
	return int(C.tg_h264_feed(d.p, C.ulong(off), C.ulong(n)))
}

func (d h264) flush() int { return int(C.tg_h264_flush(d.p)) }

// file is the decoder's copy of the file, which lives as long as it does.
func (d h264) file() []byte {
	var n C.ulong
	p := C.tg_h264_file(d.p, &n)
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n))
}

// picture is the last picture finished, as RGBA, valid until the next call.
func (d h264) picture() ([]byte, int, int) {
	var w, h C.int
	p := C.tg_h264_rgba(d.p, &w, &h)
	if p == nil || w <= 0 || h <= 0 {
		return nil, 0, 0
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(w)*int(h)*4), int(w), int(h)
}

func (d h264) close() { C.tg_h264_close(d.p) }
