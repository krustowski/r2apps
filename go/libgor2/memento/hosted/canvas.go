package hosted

// The first sixteen palette indices are identical on Memento's VGA and
// framebuffer backends.
const (
	Black     = 0
	Blue      = 1
	LightGrey = 7
	DarkGrey  = 8
	White     = 15
)

// Rect is a rectangle in client pixels, with exclusive right/bottom edges.
type Rect struct{ X, Y, W, H int }

func (r Rect) Contains(x, y int) bool {
	return r.W > 0 && r.H > 0 && x >= r.X && y >= r.Y && x-r.X < r.W && y-r.Y < r.H
}

// LeftPressed reports a left mouse press inside r, in the same coordinates
// used by Canvas. Releases and right clicks do not activate a button.
func (cmd *Command) LeftPressed(r Rect) bool {
	return cmd.Op == OpMouseButton && cmd.Value == 0 && cmd.Extra != 0 && r.Contains(int(cmd.X), int(cmd.Y))
}

// Canvas draws directly into a leased host frame. It is valid only during
// Paint/OnPaint; do not retain it or use it from another goroutine.
// Text uses Memento's 6x12 Terminus font and code page 437, a glyph per byte.
type Canvas struct {
	pixels []byte
	w, h   int
}

func (c *Canvas) Size() (int, int) { return c.w, c.h }

func (c *Canvas) Clear(colour byte) {
	for i := range c.pixels {
		c.pixels[i] = colour
	}
}

// FillRect clips to the client area, including partially visible controls.
func (c *Canvas) FillRect(r Rect, colour byte) {
	if r.W <= 0 || r.H <= 0 {
		return
	}
	x0, y0 := max(r.X, 0), max(r.Y, 0)
	x1, y1 := min(r.X+r.W, c.w), min(r.Y+r.H, c.h)
	if x0 >= x1 || y0 >= y1 {
		return
	}
	for y := y0; y < y1; y++ {
		for x := x0; x < x1; x++ {
			c.pixels[y*c.w+x] = colour
		}
	}
}

// Text draws one line, clipped to r. Its background remains untouched.
func (c *Canvas) Text(r Rect, text string, colour byte) {
	if r.W <= 0 || r.H <= 0 {
		return
	}
	right, bottom := min(r.X+r.W, c.w), min(r.Y+r.H, c.h)
	for i, x := 0, r.X; i < len(text) && x < right; i, x = i+1, x+FontWidth {
		glyph := glyphs[int(text[i])*FontHeight : (int(text[i])+1)*FontHeight]
		for row := 0; row < FontHeight && r.Y+row < bottom; row++ {
			y := r.Y + row
			if y < 0 {
				continue
			}
			for col := 0; col < FontWidth && x+col < right; col++ {
				if px := x + col; px >= 0 && glyph[row]&(0x80>>col) != 0 {
					c.pixels[y*c.w+px] = colour
				}
			}
		}
	}
}

// Button paints a bevel and centered label in Memento's standard colours.
// Use Command.LeftPressed to activate it, or handle keyboard shortcuts.
func (c *Canvas) Button(r Rect, label string, pressed bool) {
	if r.W < 4 || r.H < 4 {
		return
	}
	c.FillRect(r, LightGrey)
	light, dark, offset := byte(White), byte(DarkGrey), 0
	if pressed {
		light, dark, offset = dark, light, 1
	}
	c.FillRect(Rect{r.X, r.Y, r.W, 1}, light)
	c.FillRect(Rect{r.X, r.Y, 1, r.H}, light)
	c.FillRect(Rect{r.X, r.Y + r.H - 1, r.W, 1}, dark)
	c.FillRect(Rect{r.X + r.W - 1, r.Y, 1, r.H}, dark)
	x := r.X + max(2, (r.W-len(label)*FontWidth)/2) + offset
	y := r.Y + max(2, (r.H-FontHeight)/2) + offset
	c.Text(Rect{x, y, max(0, r.X+r.W-2-x), max(0, r.Y+r.H-2-y)}, label, Black)
}
