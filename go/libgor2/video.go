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
	if len(pixels) == 0 || srcW == 0 || srcH == 0 {
		return EInvalidInput
	}

	return err(Syscall(ScBlitBuffer,
		ptr(unsafe.Pointer(&pixels[0])),
		uintptr(srcW)<<16|uintptr(srcH)))
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
