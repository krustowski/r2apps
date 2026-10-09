// Package media turns the pictures Telegram sends into what Memento shows:
// one palette index a pixel.  It is Memento's web/image.cpp and web/mp4.cpp
// in Go, over the same C decoders (stb_image and h264bsd, in native/).
//
// A picture is made to fit a box --- scaled down, never up, keeping its
// shape, each pixel the average of those it stands for --- laid over a
// background where it is transparent, and put into the screen's colours with
// a 4x4 ordered dither: the 16 EGA colours, or with 256 the 6x6x6 cube the
// palette has at 16..231.  The dither is by position alone, so a pixel that
// stays one colour from frame to frame keeps its index and nothing crawls.
//
// Pixels live outside the Go heap (the kernel's user heap on r2) and are
// given back with Release.
package media

import "errors"

var (
	ErrNotPicture = errors.New("not a picture this client reads (PNG, JPEG, GIF, BMP)")
	ErrTooBig     = errors.New("too big to decode")
	ErrNoMemory   = errors.New("out of memory")
	ErrUnreadable = errors.New("the picture could not be read")
	ErrNotGIF     = errors.New("not a GIF")
	ErrAnimTooBig = errors.New("too big to animate")
	ErrGIFBroken  = errors.New("the GIF could not be read")
	ErrNoFrames   = errors.New("no pictures in it")
)

// Picture is W x H palette indices, row by row.
type Picture struct {
	W, H int
	Px   []byte
}

func (p *Picture) Release() {
	release(p.Px)
	*p = Picture{}
}

// Animation is Frames pictures of W x H one after another, each shown for
// Delay milliseconds; Length is all of them once round.
type Animation struct {
	W, H, Frames int
	Px           []byte
	Delay        []uint16
	Length       uint32
}

func (a *Animation) Frame(i int) []byte {
	n := a.W * a.H
	return a.Px[i*n : (i+1)*n]
}

// FrameAt is the frame to show ms into the loop (any number: it goes round).
func (a *Animation) FrameAt(ms uint64) int {
	if a.Frames < 2 || a.Length == 0 {
		return 0
	}
	t := uint32(ms % uint64(a.Length))
	for i := 0; i < a.Frames; i++ {
		if t < uint32(a.Delay[i]) {
			return i
		}
		t -= uint32(a.Delay[i])
	}
	return a.Frames - 1
}

// Still is frame i as a picture of its own.
func (a *Animation) Still(i int) (Picture, error) {
	px := alloc(a.W * a.H)
	if px == nil {
		return Picture{}, ErrNoMemory
	}
	copy(px, a.Frame(i))
	return Picture{W: a.W, H: a.H, Px: px}, nil
}

func (a *Animation) Release() {
	release(a.Px)
	*a = Animation{}
}

func IsGIF(b []byte) bool {
	return len(b) >= 6 && string(b[:4]) == "GIF8" && (b[4] == '7' || b[4] == '9') && b[5] == 'a'
}

// The 16 colours as the palette has them, and the tables the dither uses.
var ega = [16][3]int{
	{0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170}, {170, 0, 0}, {170, 0, 170},
	{170, 85, 0}, {170, 170, 170}, {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
	{255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
}

var (
	nearest        [16 * 16 * 16]byte // the EGA colour nearest each RGB at four bits a channel
	bayer, bayer6  [4][4]int          // offsets for steps of 85 (16 colours) and 51 (the cube)
	q6             [256]byte
	tablesBuilt    bool
	bayerPositions = [4][4]int{{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}}
)

func buildTables() {
	if tablesBuilt {
		return
	}
	for r := 0; r < 16; r++ {
		for g := 0; g < 16; g++ {
			for b := 0; b < 16; b++ {
				R, G, B, best, bestD := r*17, g*17, b*17, 0, 1<<30
				for k, c := range ega {
					dr, dg, db := R-c[0], G-c[1], B-c[2]
					if d := 3*dr*dr + 4*dg*dg + 2*db*db; d < bestD { // green counts most
						bestD, best = d, k
					}
				}
				nearest[r<<8|g<<4|b] = byte(best)
			}
		}
	}
	for y := 0; y < 4; y++ {
		for x := 0; x < 4; x++ {
			bayer[y][x] = bayerPositions[y][x]*85/16 - 40
			bayer6[y][x] = bayerPositions[y][x]*51/16 - 24
		}
	}
	for v := range q6 {
		q6[v] = byte((v*5 + 127) / 255)
	}
	tablesBuilt = true
}

func clamp255(v int) int {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return v
}

// FitSize is the size a w x h picture is shown at in maxW x maxH: the same
// factor both ways, never enlarged.
func FitSize(w, h, maxW, maxH int) (int, int) {
	if maxW < 1 {
		maxW = 1
	}
	if maxH < 1 {
		maxH = 1
	}
	tw, th := w, h
	if tw > maxW {
		th = int(int64(th) * int64(maxW) / int64(tw))
		tw = maxW
	}
	if th > maxH {
		tw = int(int64(tw) * int64(maxH) / int64(th))
		th = maxH
	}
	return max(tw, 1), max(th, 1)
}

// dither makes w x h of RGBA into tw x th palette indices in px.
func dither(rgba []byte, w, h, tw, th, colours int, bg uint32, px []byte) {
	buildTables()
	bgR, bgG, bgB := int(bg>>16)&255, int(bg>>8)&255, int(bg)&255
	cube := colours >= 256
	for ty := 0; ty < th; ty++ {
		sy0, sy1 := ty*h/th, (ty+1)*h/th
		if sy1 <= sy0 {
			sy1 = sy0 + 1
		}
		d4 := &bayer[ty&3]
		if cube {
			d4 = &bayer6[ty&3]
		}
		o := px[ty*tw : (ty+1)*tw]
		for tx := 0; tx < tw; tx++ {
			sx0, sx1 := tx*w/tw, (tx+1)*w/tw
			if sx1 <= sx0 {
				sx1 = sx0 + 1
			}
			// The average of the pixels this one stands for, each laid
			// over the background by its alpha first.
			var sr, sg, sb, n int
			for sy := sy0; sy < sy1; sy++ {
				p := rgba[(sy*w+sx0)*4 : (sy*w+sx1)*4]
				for i := 0; i < len(p); i += 4 {
					a := int(p[i+3])
					sr += (int(p[i])*a + bgR*(255-a)) / 255
					sg += (int(p[i+1])*a + bgG*(255-a)) / 255
					sb += (int(p[i+2])*a + bgB*(255-a)) / 255
					n++
				}
			}
			d := d4[tx&3]
			r, g, b := clamp255(sr/n+d), clamp255(sg/n+d), clamp255(sb/n+d)
			if cube {
				o[tx] = byte(16 + 36*int(q6[r]) + 6*int(q6[g]) + int(q6[b]))
			} else {
				o[tx] = nearest[(r>>4)<<8|(g>>4)<<4|b>>4]
			}
		}
	}
}

// Builder puts an Animation together a frame at a time from RGBA, as a
// decoder hands them over: each made to fit maxW x maxH (the first one's shape
// sets the size) and dithered, within budget bytes.  Where there are more
// frames than the budget holds, every other one is left out (and the time it
// was shown given to the one before), as often as it takes: the whole loop at
// a lower frame rate, not the first second of it.
type Builder struct {
	a                   Animation
	maxW, maxH, colours int
	bg                  uint32
	budget              int
	cap, stride         int
	seen                int
	why                 error
}

func NewBuilder(maxW, maxH, colours int, bg uint32, budget int) *Builder {
	return &Builder{maxW: maxW, maxH: maxH, colours: colours, bg: bg, budget: budget, stride: 1}
}

func addDelay(d *uint16, ms int) {
	*d = uint16(min(int(*d)+ms, 65535))
}

// Add takes the next frame, shown for ms.  False when it could not be taken
// and no more will be (Err says why).
func (b *Builder) Add(rgba []byte, w, h, ms int) bool {
	if b.why != nil {
		return false
	}
	ms = max(ms, 1)
	a := &b.a
	if a.Px == nil {
		a.W, a.H = FitSize(w, h, b.maxW, b.maxH)
		fb := a.W * a.H
		n := min(b.budget/fb, 1024)
		b.cap = n &^ 1
		if b.cap < 2 {
			b.why = ErrAnimTooBig
			return false
		}
		if a.Px = alloc(fb * b.cap); a.Px == nil {
			b.why = ErrNoMemory
			return false
		}
		a.Delay = make([]uint16, b.cap)
	}
	b.seen++
	if (b.seen-1)%b.stride != 0 {
		addDelay(&a.Delay[a.Frames-1], ms)
		return true
	}
	fb := a.W * a.H
	if a.Frames == b.cap {
		// Full: every other frame goes, and the rest are kept at twice the
		// distance.  cap is even, so this frame is one of those.
		n := 0
		for i := 0; i < a.Frames; i, n = i+2, n+1 {
			if n != i {
				copy(a.Px[fb*n:fb*(n+1)], a.Px[fb*i:fb*(i+1)])
			}
			d := a.Delay[i]
			if i+1 < a.Frames {
				addDelay(&d, int(a.Delay[i+1]))
			}
			a.Delay[n] = d
		}
		a.Frames = n
		b.stride *= 2
	}
	dither(rgba, w, h, a.W, a.H, b.colours, b.bg, a.Px[fb*a.Frames:fb*(a.Frames+1)])
	a.Delay[a.Frames] = uint16(min(ms, 65535))
	a.Frames++
	return true
}

func (b *Builder) Err() error { return b.why }

// Finish hands over the animation, or says why there is none.
func (b *Builder) Finish() (Animation, error) {
	a := b.a
	b.a = Animation{}
	if a.Frames == 0 {
		a.Release()
		if b.why != nil {
			return Animation{}, b.why
		}
		return Animation{}, ErrNoFrames
	}
	// Give back the room that was not needed.
	if a.Frames < b.cap {
		if px := alloc(a.W * a.H * a.Frames); px != nil {
			copy(px, a.Px)
			release(a.Px)
			a.Px = px
		}
	}
	a.Delay = a.Delay[:a.Frames]
	a.Length = 0
	for _, d := range a.Delay {
		a.Length += uint32(d)
	}
	return a, nil
}

// Cancel lets go of whatever was built.
func (b *Builder) Cancel() {
	b.a.Release()
}

// shownFor is what browsers do with a GIF delay of 0 or 10 ms, which many
// GIFs have and none means: 100.
func shownFor(ms int) int {
	if ms <= 10 {
		return 100
	}
	return ms
}
