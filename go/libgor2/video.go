package libgor2

import "unsafe"

// VGA modes accepted by SetVideoMode.
const (
	Mode03Text   = 0x03 // 80x25 colour text; restores the kernel shell
	Mode0DPlanar = 0x0D // 320x200, 16 colours, 4 planes
	Mode12Planar = 0x12 // 640x480, 16 colours
	Mode13Chunky = 0x13 // 320x200, 256 colours (classic mode 13h)
)

// WritePixel draws one pixel in 0x00RRGGBB (syscall 0x12).
func WritePixel(x, y uint16, colour uint32) error {
	return err(Syscall(ScWritePixel, uintptr(x)<<16|uintptr(y), uintptr(colour)))
}

// WriteVGA renders a 320x200 palette-indexed buffer to the framebuffer
// (syscall 0x13).  buf must be 64000 bytes.  palette is 256 RGB triplets (768
// bytes), or nil for the default VGA palette.
func WriteVGA(buf []byte, palette []byte) error {
	if len(buf) < 64000 {
		return EInvalidInput
	}

	var palPtr uintptr
	if len(palette) >= 768 {
		palPtr = ptr(unsafe.Pointer(&palette[0]))
	}

	return err(Syscall(ScWriteVGA, ptr(unsafe.Pointer(&buf[0])), palPtr))
}

// MapVRAM maps VGA graphics RAM (0xA0000-0xAFFFF) into this process at virtual
// 0xA00000 and returns that address (syscall 0x14).  It is idempotent.
func MapVRAM() (uintptr, error) {
	// Package-level for the same reason as the cells in net.go: the address of
	// a local passed as an integer makes TinyGo heap-allocate it per call.
	if e := err(Syscall(ScMapVram, 0, ptr(unsafe.Pointer(&vramBase)))); e != nil {
		return 0, e
	}

	return uintptr(vramBase), nil
}

var vramBase uint64

// SetVideoMode programs the VGA hardware registers (syscall 0x15).  Pass
// Mode03Text on the way out to give the shell its screen back.
func SetVideoMode(mode uint8) error {
	return err(Syscall(ScSetVideoMode, uintptr(mode), 0))
}

// GetFBInfo describes the VESA linear framebuffer the bootloader set up
// (syscall 0x16).  It reports an error when there is no framebuffer.
func GetFBInfo(info *FBInfo) error {
	return err(Syscall(ScGetFBInfo, ptr(unsafe.Pointer(info)), 0))
}

// Blit copies a 32bpp 0x00RRGGBB buffer of exactly fb.Width x fb.Height pixels
// to the framebuffer: one call per frame (syscall 0x17).
func Blit(pixels []uint32) error {
	if len(pixels) == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScBlitBuffer, ptr(unsafe.Pointer(&pixels[0])), 0))
}

// BlitScaled is Blit for a buffer that is not the size of the screen: the
// kernel stretches srcW x srcH to fill the framebuffer, nearest-neighbour.
func BlitScaled(pixels []uint32, srcW, srcH uint16) error {
	if srcW == 0 || srcH == 0 || uint64(len(pixels)) < uint64(srcW)*uint64(srcH) {
		return EInvalidInput
	}

	return err(Syscall(ScBlitBuffer,
		ptr(unsafe.Pointer(&pixels[0])),
		uintptr(srcW)<<16|uintptr(srcH)))
}

// IndexedAvailable reports whether the kernel can draw indexed frames.
func IndexedAvailable() bool {
	return Syscall(ScBlitIndexed, 0, 0) == 0
}

// IndexedPresentAvailable reports whether indexed presentation transactions
// are supported. Check IndexedAvailable first: a missing framebuffer also
// returns 1 from the protocol probe.
func IndexedPresentAvailable() bool {
	return IndexedAvailable() && Syscall(ScBlitIndexed, 0, 4) == 1
}

// BeginIndexedPresent begins a presentation transaction. Finish it with
// EndIndexedPresent, or CancelIndexedPresent if drawing fails. EBusy means
// another process is presenting. Probe IndexedPresentAvailable before use
// on older kernels.
func BeginIndexedPresent() error {
	return err(Syscall(ScBlitIndexed, 0, 2))
}

// CancelIndexedPresent ends a transaction without publishing a snapshot.
func CancelIndexedPresent() error {
	return err(Syscall(ScBlitIndexed, 0, 3))
}

// indexedFrame is scratch storage to avoid allocating an argument block on
// every blit. Like the other scratch cells in this package, it is not re-entrant.
var indexedFrame IndexedFrame

func prepareIndexedFrame(pixels, palette []byte, width, height uint32) error {
	if width == 0 || height == 0 || uint64(len(pixels)) < uint64(width)*uint64(height) || len(palette) < 768 {
		return EInvalidInput
	}
	indexedFrame = IndexedFrame{
		Pixels:  uint64(ptr(unsafe.Pointer(&pixels[0]))),
		Palette: uint64(ptr(unsafe.Pointer(&palette[0]))),
		Width:   width,
		Height:  height,
		Rows:    height,
	}
	return nil
}

// BlitIndexed draws a band of an 8-bit indexed frame (syscall 0x19).
// The kernel centers the frame and scales it by the largest integer that fits.
// pixels holds the whole width * height frame, palette holds 768 RGB bytes,
// and firstRow/rows select the band. clear clears the screen before drawing.
// The frame must fit the framebuffer even before scaling.
func BlitIndexed(pixels, palette []byte, width, height, firstRow, rows uint32, clear bool) error {
	if firstRow >= height || rows == 0 || rows > height-firstRow {
		return EInvalidInput
	}
	if e := prepareIndexedFrame(pixels, palette, width, height); e != nil {
		return e
	}
	indexedFrame.FirstRow = firstRow
	indexedFrame.Rows = rows
	var op uintptr
	if clear {
		op = 1
	}
	return err(Syscall(ScBlitIndexed, ptr(unsafe.Pointer(&indexedFrame)), op))
}

// EndIndexedPresent publishes the complete composed frame as a stable
// 640x480 RGB24 snapshot and ends the transaction. Supply the full frame even
// if BlitIndexed only drew dirty row bands. If local validation fails, the
// transaction remains open; fix the input or call CancelIndexedPresent.
func EndIndexedPresent(pixels, palette []byte, width, height uint32) error {
	if e := prepareIndexedFrame(pixels, palette, width, height); e != nil {
		return e
	}
	return err(Syscall(ScBlitIndexed, ptr(unsafe.Pointer(&indexedFrame)), 3))
}

// CaptureFramebuffer copies the framebuffer to tightly packed 0x00RRGGBB
// pixels (syscall 0x1c). EBusy means presentation overlapped the capture;
// discard the contents and retry. This syscall assumes a 32bpp framebuffer.
func CaptureFramebuffer(pixels []uint32) error {
	var info FBInfo
	if e := GetFBInfo(&info); e != nil {
		return e
	}
	if info.Width == 0 || info.Height == 0 || info.BPP != 32 || uint64(len(pixels)) < uint64(info.Width)*uint64(info.Height) {
		return EInvalidInput
	}
	return err(Syscall(ScCaptureFB, ptr(unsafe.Pointer(&pixels[0])), 0))
}

// CaptureFramebufferRGB24Scaled captures and nearest-neighbour scales the
// framebuffer into width * height * 3 RGB bytes (syscall 0x1d). Dimensions
// must be in 1..65535. EBusy means discard the contents and retry. At 640x480,
// kernels with the presentation protocol can use a completed indexed snapshot.
func CaptureFramebufferRGB24Scaled(rgb []byte, width, height uint32) error {
	if width == 0 || height == 0 || width > 0xffff || height > 0xffff || uint64(len(rgb)) < uint64(width)*uint64(height)*3 {
		return EInvalidInput
	}
	return err(Syscall(ScCaptureFBRGB24Scaled, ptr(unsafe.Pointer(&rgb[0])), uintptr(width)<<16|uintptr(height)))
}

// KernelFont copies the kernel's embedded PSF1 glyphs into buf and returns the
// glyph height in bytes (syscall 0x18).  Glyph n is buf[n*h : (n+1)*h]; each
// row is one byte, 8 pixels wide, most significant bit leftmost.
func KernelFont(buf []byte) int {
	if len(buf) == 0 {
		return 0
	}

	return int(Syscall(ScGetKernelFont,
		ptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf))))
}
