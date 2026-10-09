package main

// The colours the window is drawn in: the 16 EGA colours, which mean the
// same on the VGA and on the framebuffer's 256-colour palette.
const (
	black     = 0
	blue      = 1
	lightGrey = 7
	darkGrey  = 8
	yellow    = 14
	white     = 15
)

// canvas is a frame being drawn: one palette index a pixel, as Memento's
// R2_BitmapImpl, with its clip rectangle.
type canvas struct {
	w, h               int
	px                 []byte
	cx0, cy0, cx1, cy1 int // the clip: [cx0, cx1) x [cy0, cy1)
}

func (c *canvas) resize(w, h int) {
	if cap(c.px) < w*h {
		c.px = make([]byte, w*h)
	}
	c.w, c.h, c.px = w, h, c.px[:w*h]
	c.clearClip()
}

func (c *canvas) setClip(x, y, w, h int) {
	c.cx0, c.cy0 = max(x, 0), max(y, 0)
	c.cx1, c.cy1 = min(x+w, c.w), min(y+h, c.h)
}

func (c *canvas) clearClip() { c.cx0, c.cy0, c.cx1, c.cy1 = 0, 0, c.w, c.h }

func (c *canvas) fill(x, y, w, h int, colour byte) {
	x0, y0, x1, y1 := max(x, c.cx0), max(y, c.cy0), min(x+w, c.cx1), min(y+h, c.cy1)
	if x1 <= x0 {
		return
	}
	for ; y0 < y1; y0++ {
		row := c.px[y0*c.w+x0 : y0*c.w+x1]
		for i := range row {
			row[i] = colour
		}
	}
}

// text draws s from (x, y), a glyph a byte, cut at x+w and at the clip.
func (c *canvas) text(x, y, w int, s string, colour byte) {
	right := min(x+w, c.cx1)
	for i := 0; i < len(s) && x < right; i, x = i+1, x+glyphW {
		g := glyphs[int(s[i])*glyphH : (int(s[i])+1)*glyphH]
		for row := 0; row < glyphH; row++ {
			py := y + row
			if py < c.cy0 || py >= c.cy1 || g[row] == 0 {
				continue
			}
			line := c.px[py*c.w : (py+1)*c.w]
			for col := 0; col < 8; col++ {
				if px := x + col; g[row]&(0x80>>col) != 0 && px >= c.cx0 && px < right {
					line[px] = colour
				}
			}
		}
	}
}

// textEnd draws s so that it ends at x+w.
func (c *canvas) textEnd(x, y, w int, s string, colour byte) {
	c.text(x+w-len(s)*glyphW, y, len(s)*glyphW, s, colour)
}

// blit draws a w x h picture of pw x ph pixels at (x, y), scaled to fit
// (nearest pixel), cut at the clip and at right.
func (c *canvas) blit(x, y, w, h int, px []byte, pw, ph int, right int) {
	right = min(right, c.cx1)
	for dy := 0; dy < h; dy++ {
		py := y + dy
		if py < c.cy0 {
			continue
		}
		if py >= c.cy1 {
			break
		}
		src := px[(dy*ph/h)*pw:]
		dst := c.px[py*c.w : (py+1)*c.w]
		for dx := 0; dx < w && x+dx < right; dx++ {
			if x+dx >= c.cx0 {
				sx := dx
				if w != pw {
					sx = dx * pw / w
				}
				dst[x+dx] = src[sx]
			}
		}
	}
}
