package main

// The drawing side of the demo: a palette-indexed canvas the size of a mode 13h
// screen, a palette to go with it, and just enough shapes to put something
// moving on it.
//
// Everything here is integer arithmetic.  That is not nostalgia: the kernel's
// timer interrupt saves the general-purpose registers and nothing else, so the
// SSE state a Go program would use for float64 is not preserved across a
// context switch, and a demo that runs alongside the shell is exactly the case
// where that goes wrong.

const (
	screenW = 320
	screenH = 200

	// Palette layout.  The plasma gets the long smooth ramp; the balls and the
	// text get solid entries at the top where the ramp cannot wander into them.
	colBlack   = 0
	plasmaLo   = 1
	plasmaHi   = 239
	colBallLo  = 240 // 240..247
	colBallN   = 8
	colWhite   = 255
	plasmaSpan = plasmaHi - plasmaLo + 1
)

// sine, one period over 64 entries, 0..63.  The same table c/gfxtest uses.
var sin64 = [64]int32{
	32, 35, 38, 41, 44, 47, 49, 51, 53, 55, 57, 58, 59, 60, 61, 62,
	63, 62, 61, 60, 59, 58, 57, 55, 53, 51, 49, 47, 44, 41, 38, 35,
	32, 29, 26, 23, 20, 17, 15, 13, 11, 9, 7, 6, 5, 4, 3, 2,
	1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 15, 17, 20, 23, 26, 29,
}

// canvas is a 320x200 image of palette indices --- the shape mode 13h wants,
// and the shape syscall 0x13 takes, so neither display has to convert it.
type canvas struct {
	pix []byte
}

func newCanvas() *canvas {
	return &canvas{pix: make([]byte, screenW*screenH)}
}

// plasma fills the whole canvas with the classic sum-of-sines, moved along by t.
//
// Animating the pattern rather than rotating the palette is deliberate: the
// palette lives in the VGA DAC, and rotating it would mean a thousand port
// syscalls per frame.
func (c *canvas) plasma(t int32) {
	for y := 0; y < screenH; y++ {
		var (
			row = y * screenW
			a   = sin64[(int32(y)/2+t/2)&63]
		)

		for x := 0; x < screenW; x++ {
			v := sin64[(int32(x)/3+t)&63] +
				a +
				sin64[((int32(x)+int32(y))/4-t)&63]

			// v is 0..189; spread it over the plasma part of the palette.
			c.pix[row+x] = byte(plasmaLo + v*plasmaSpan/190)
		}
	}
}

// fillCircle draws a filled circle, clipped to the canvas.
func (c *canvas) fillCircle(cx, cy, r int, colour byte) {
	for y := cy - r; y <= cy+r; y++ {
		if y < 0 || y >= screenH {
			continue
		}

		dy := y - cy
		row := y * screenW

		for x := cx - r; x <= cx+r; x++ {
			if x < 0 || x >= screenW {
				continue
			}

			dx := x - cx
			if dx*dx+dy*dy <= r*r {
				c.pix[row+x] = colour
			}
		}
	}
}

// border draws a one-pixel frame, which is how you tell at a glance that the
// whole visible area really is yours.
func (c *canvas) border(colour byte) {
	for x := 0; x < screenW; x++ {
		c.pix[x] = colour
		c.pix[(screenH-1)*screenW+x] = colour
	}

	for y := 0; y < screenH; y++ {
		c.pix[y*screenW] = colour
		c.pix[y*screenW+screenW-1] = colour
	}
}

// palette is 256 RGB triplets, in the 8-bit-per-channel form syscall 0x13
// takes.  The VGA DAC wants six bits, and openVGA shifts them down on the way.
//
// packed holds the same colours as 0x00RRGGBB words, which is what the blitting
// syscall takes: a display that has to convert the canvas every frame should
// not be doing three byte loads and two shifts per pixel to do it.
type palette struct {
	rgb    []byte
	packed [256]uint32
}

func newPalette() *palette {
	p := &palette{rgb: make([]byte, 256*3)}

	// A rainbow across the plasma range.
	for i := plasmaLo; i <= plasmaHi; i++ {
		r, g, b := hue((i - plasmaLo) * 1536 / plasmaSpan)
		p.set(byte(i), r, g, b)
	}

	// Solid colours for the balls, evenly spaced around the same wheel but at
	// full brightness so they stand out against the plasma behind them.
	for i := 0; i < colBallN; i++ {
		r, g, b := hue(i * 1536 / colBallN)
		p.set(byte(colBallLo+i), r, g, b)
	}

	p.set(colBlack, 0, 0, 0)
	p.set(colWhite, 255, 255, 255)

	return p
}

func (p *palette) set(i, r, g, b byte) {
	p.rgb[int(i)*3] = r
	p.rgb[int(i)*3+1] = g
	p.rgb[int(i)*3+2] = b
	p.packed[i] = uint32(r)<<16 | uint32(g)<<8 | uint32(b)
}

// hue walks the six segments of the colour wheel; h is 0..1535.
func hue(h int) (r, g, b byte) {
	h %= 1536
	f := byte(h % 256)

	switch h / 256 {
	case 0:
		return 255, f, 0
	case 1:
		return 255 - f, 255, 0
	case 2:
		return 0, 255, f
	case 3:
		return 0, 255 - f, 255
	case 4:
		return f, 0, 255
	default:
		return 255, 0, 255 - f
	}
}

// font is the kernel's own PSF1 glyphs, fetched through syscall 0x18: one byte
// per row, eight pixels wide, most significant bit leftmost.
type font struct {
	glyphs []byte
	height int
}

func (f *font) drawString(c *canvas, x, y int, s string, colour byte) {
	for i := 0; i < len(s); i++ {
		f.drawGlyph(c, x+i*8, y, s[i], colour)
	}
}

func (f *font) drawGlyph(c *canvas, x, y int, ch byte, colour byte) {
	base := int(ch) * f.height

	for row := 0; row < f.height; row++ {
		py := y + row
		if py < 0 || py >= screenH {
			continue
		}

		bits := f.glyphs[base+row]

		for col := 0; col < 8; col++ {
			if bits&(0x80>>col) == 0 {
				continue
			}

			px := x + col
			if px < 0 || px >= screenW {
				continue
			}

			c.pix[py*screenW+px] = colour
		}
	}
}

// ball is a bouncing disc.  Position and velocity are 16.16 fixed point, so
// that a ball can move by a fraction of a pixel per frame without a float.
type ball struct {
	x, y   int32
	dx, dy int32
	r      int
	colour byte
}

const fixedOne = 1 << 16

func (b *ball) step() {
	b.x += b.dx
	b.y += b.dy

	if lo := int32(b.r) * fixedOne; b.x < lo {
		b.x, b.dx = lo, -b.dx
	} else if hi := int32(screenW-b.r) * fixedOne; b.x > hi {
		b.x, b.dx = hi, -b.dx
	}

	if lo := int32(b.r) * fixedOne; b.y < lo {
		b.y, b.dy = lo, -b.dy
	} else if hi := int32(screenH-b.r) * fixedOne; b.y > hi {
		b.y, b.dy = hi, -b.dy
	}
}

func (b *ball) draw(c *canvas) {
	c.fillCircle(int(b.x/fixedOne), int(b.y/fixedOne), b.r, b.colour)
}

// drawBytes is drawString for text that was built at runtime, so that a caller
// does not have to allocate a string to put a number on the screen.
func (f *font) drawBytes(c *canvas, x, y int, b []byte, colour byte) {
	for i := 0; i < len(b); i++ {
		f.drawGlyph(c, x+i*8, y, b[i], colour)
	}
}
