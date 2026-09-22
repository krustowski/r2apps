// gfxdemo exercises the r2 graphics ABI from Go: it asks the kernel what
// display it has, gets a moving picture onto it, and says how fast that went.
//
// There are three ways to put pixels on an r2 screen, and the demo will pick
// the best one it can, or take the one it is told:
//
//   - blit: convert the canvas to 0x00RRGGBB and let the kernel scale it over
//     the whole VESA framebuffer (syscall 0x17);
//   - fb: hand the kernel the palette-indexed canvas and let it do the
//     conversion, one pixel per pixel in the corner of the screen (0x13);
//   - vga: drive the hardware directly --- mode 13h, the palette loaded into
//     the DAC by hand, and the canvas copied straight into the video memory
//     the kernel maps for the process (0x14, 0x15, 0x30).
//
// Booted from the text-mode kernel there is no usable framebuffer --- the one
// the kernel reports is the 80x25 text buffer --- so vga is the only path that
// works, and it is what auto picks.
//
// Escape quits early; either way the screen is handed back in text mode.
//
//	gfxdemo             fifteen seconds, best available path
//	gfxdemo 5           five seconds
//	gfxdemo 5 vga       five seconds, forced through mode 13h
package main

import (
	"fmt"
	"unsafe"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

const (
	defaultRunSeconds = 15
	scanEscape        = 0x01

	ballCount  = 6
	ballRadius = 9
)

// The kernel keeps the pointer it is given at subscribe time, so the buffer has
// to outlive the subscription; a package-level array is the simplest way to
// promise that.  keyDrain is package-level for a second reason --- see the
// allocation note in ../README.md --- so that polling it costs nothing.
var (
	keyPipe  [64]byte
	keyDrain [16]byte
)

// display is whatever can get a canvas onto the screen.
type display interface {
	present(c *canvas) error
	name() string
	close()
}

// fbDisplay hands the canvas to the kernel, which walks it against the palette
// and writes the VESA framebuffer (syscall 0x13).
type fbDisplay struct {
	pal *palette
}

func (d *fbDisplay) name() string { return "VESA framebuffer" }

func (d *fbDisplay) present(c *canvas) error {
	return libgor2.WriteVGA(c.pix, d.pal.rgb)
}

// close hands the screen back.  There is no video mode to restore on this
// path, but the last frame would otherwise sit there over whatever the shell
// prints next, so the screen is blacked out by drawing one more frame.
//
// Syscall 0x11 is no use here: it clears the VGA text writer, which on a
// graphical boot is not what is on the screen.
func (d *fbDisplay) close() {
	libgor2.WriteVGA(make([]byte, screenW*screenH), d.pal.rgb)
}

// blitDisplay converts the canvas to 0x00RRGGBB itself and asks the kernel to
// stretch it over the whole framebuffer (syscall 0x17).  That costs a lookup
// per pixel, but 320x200 in the corner of a 1024x768 screen is not much of a
// demo.
type blitDisplay struct {
	pal *palette
	buf []uint32
}

func (d *blitDisplay) name() string { return "VESA framebuffer (scaled blit)" }

func (d *blitDisplay) present(c *canvas) error {
	for i, idx := range c.pix {
		d.buf[i] = d.pal.packed[idx]
	}

	return libgor2.BlitScaled(d.buf, screenW, screenH)
}

func (d *blitDisplay) close() {
	for i := range d.buf {
		d.buf[i] = 0
	}

	libgor2.BlitScaled(d.buf, screenW, screenH)
}

// vgaDisplay drives the VGA itself: mode 13h, the palette in the DAC, and the
// canvas copied into the mapped video memory with no syscall per frame.
type vgaDisplay struct {
	vram []byte
}

func (d *vgaDisplay) name() string { return "VGA mode 13h" }

func (d *vgaDisplay) present(c *canvas) error {
	copy(d.vram, c.pix)

	return nil
}

func (d *vgaDisplay) close() {
	libgor2.SetVideoMode(libgor2.Mode03Text)
}

func openVGA(pal *palette) (display, error) {
	base, err := libgor2.MapVRAM()
	if err != nil {
		return nil, fmt.Errorf("MAP_VRAM: %w", err)
	}

	if base == 0 {
		return nil, fmt.Errorf("MAP_VRAM returned no address")
	}

	if err := libgor2.SetVideoMode(libgor2.Mode13Chunky); err != nil {
		return nil, fmt.Errorf("SET_VIDEO_MODE 0x13: %w", err)
	}

	loadDAC(pal)

	// Mode 13h is linear: one byte per pixel, 320 per row, starting at the
	// base the kernel just mapped for us.
	return &vgaDisplay{
		vram: unsafe.Slice((*byte)(unsafe.Pointer(base)), screenW*screenH),
	}, nil
}

// loadDAC writes the palette into the VGA colour registers.  The DAC takes six
// bits per channel, so the top eight-bit values are shifted down.
func loadDAC(pal *palette) {
	for i := 0; i < 256; i++ {
		libgor2.WritePort(0x3C8, uint32(i))
		libgor2.WritePort(0x3C9, uint32(pal.rgb[i*3])>>2)
		libgor2.WritePort(0x3C9, uint32(pal.rgb[i*3+1])>>2)
		libgor2.WritePort(0x3C9, uint32(pal.rgb[i*3+2])>>2)
	}
}

// usableFB reports whether the framebuffer the kernel describes is one we can
// actually draw on.
//
// It has to be asked, because a framebuffer being reported is not the same as
// a framebuffer existing: booted from the text-mode kernel the answer is the
// 80x25 text buffer, and pushing 320x200 pixels of 32-bit colour at that would
// write a long way past its end.
func usableFB(fb *libgor2.FBInfo, haveFB bool) bool {
	return haveFB && fb.BPP == 32 && fb.Width >= screenW && fb.Height >= screenH
}

// openDisplay opens the requested path, or the best available one for "auto".
func openDisplay(mode string, fb *libgor2.FBInfo, haveFB bool, pal *palette) (display, error) {
	switch mode {
	case "vga":
		return openVGA(pal)

	case "fb", "blit":
		if !usableFB(fb, haveFB) {
			return nil, fmt.Errorf("no usable framebuffer for %q; try vga", mode)
		}

		if mode == "fb" {
			return &fbDisplay{pal: pal}, nil
		}

		return &blitDisplay{pal: pal, buf: make([]uint32, screenW*screenH)}, nil

	case "auto":
		if usableFB(fb, haveFB) {
			return &blitDisplay{pal: pal, buf: make([]uint32, screenW*screenH)}, nil
		}

		return openVGA(pal)

	default:
		return nil, fmt.Errorf("unknown mode %q; want auto, vga, fb or blit", mode)
	}
}

func loadFont() *font {
	buf := make([]byte, 256*32)

	h := libgor2.KernelFont(buf)
	if h <= 0 || h > 32 {
		return nil
	}

	return &font{glyphs: buf, height: h}
}

// escapePressed drains the keyboard pipe and reports whether Escape was among
// the scancodes.  The pipe carries raw PS/2 set 1: bit 7 marks a key release.
func escapePressed() bool {
	for i := range keyDrain {
		keyDrain[i] = 0
	}

	if libgor2.KeyboardRead(keyDrain[:]) != nil {
		return false
	}

	for _, sc := range keyDrain[:] {
		if sc == 0 {
			break
		}

		if sc&0x7f == scanEscape {
			return true
		}
	}

	return false
}

func main() {
	var (
		seconds = defaultRunSeconds
		mode    = "auto"
	)

	if args := libgor2.Args(); len(args) > 1 {
		for _, a := range args[1:] {
			if n := atoi(a); n > 0 {
				seconds = n

				continue
			}

			mode = a
		}
	}

	fmt.Printf("gfxdemo: r2 graphics test\n")

	var fb libgor2.FBInfo
	haveFB := libgor2.GetFBInfo(&fb) == nil
	if haveFB {
		fmt.Printf("  framebuffer: %dx%d, %d bpp, pitch %d\n", fb.Width, fb.Height, fb.BPP, fb.Pitch)
	} else {
		fmt.Printf("  framebuffer: none reported\n")
	}

	pal := newPalette()

	fnt := loadFont()
	if fnt == nil {
		fmt.Printf("  font: GET_KERNEL_FONT failed, drawing without text\n")
	} else {
		fmt.Printf("  font: %d rows per glyph\n", fnt.height)
	}

	d, err := openDisplay(mode, &fb, haveFB, pal)
	if err != nil {
		fmt.Printf("gfxdemo: %v\n", err)

		return
	}

	// Subscribe before the screen goes away, so Escape works throughout.
	keyboard := libgor2.KeyboardSubscribe(keyPipe[:]) == nil

	frames, elapsed := run(d, fnt, seconds, keyboard)

	d.close()

	if keyboard {
		libgor2.KeyboardUnsubscribe(keyPipe[:])
	}

	fmt.Printf("gfxdemo: %s, %d frames in %d ms", d.name(), frames, elapsed)
	if elapsed > 0 {
		fmt.Printf(" (%d fps)", uint64(frames)*1000/elapsed)
	}
	fmt.Printf("\n")
}

// run is the frame loop.  Nothing in it allocates: the canvas, the palette, the
// glyphs and the keyboard buffer were all built before it started, which is
// what keeps the collector out of the frame time.
func run(d display, fnt *font, seconds int, keyboard bool) (frames int, elapsed uint64) {
	c := newCanvas()

	balls := make([]ball, ballCount)
	for i := range balls {
		balls[i] = ball{
			x:      int32(40+i*40) * fixedOne,
			y:      int32(40+(i%3)*50) * fixedOne,
			dx:     int32(fixedOne*3/2) + int32(i)*fixedOne/4,
			dy:     int32(fixedOne) + int32(i%4)*fixedOne/3,
			r:      ballRadius,
			colour: byte(colBallLo + i%colBallN),
		}

		if i%2 == 0 {
			balls[i].dy = -balls[i].dy
		}
	}

	var (
		counter [12]byte
		start   = libgor2.Ticks()
		limit   = uint64(seconds) * 1000
		t       int32
	)

	for {
		elapsed = libgor2.Ticks() - start
		if elapsed >= limit {
			break
		}

		if keyboard && escapePressed() {
			break
		}

		c.plasma(t)

		for i := range balls {
			balls[i].step()
			balls[i].draw(c)
		}

		if fnt != nil {
			fnt.drawString(c, 8, 6, "GO ON R2", colWhite)

			n := itoa(counter[:], frames)
			fnt.drawBytes(c, screenW-8-n*8, 6, counter[:n], colWhite)
		}

		c.border(colWhite)

		if err := d.present(c); err != nil {
			break
		}

		frames++
		t++
	}

	return frames, elapsed
}

// itoa writes n into buf and returns how many digits it took.  It exists so the
// frame counter can be drawn without allocating a string every frame.
func itoa(buf []byte, n int) int {
	if n == 0 {
		buf[0] = '0'

		return 1
	}

	i := len(buf)
	for n > 0 && i > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}

	return copy(buf, buf[i:])
}

func atoi(s string) int {
	n := 0
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0
		}

		n = n*10 + int(s[i]-'0')
	}

	return n
}
