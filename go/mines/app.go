package main

// Minesweeper in a Memento window: the rules (game.go) driven by the
// window's input, drawn with Memento's font and EGA colours.
//
//	left click, Space      open a cell
//	right click, F         flag it (again: take the flag off)
//	click an open number   when its flags are all there, open the rest
//	                       of its neighbours (a "chord")
//	arrows                 move the keyboard cursor
//	N, or the face         a new game
//	1 2 3                  beginner, intermediate, expert
//	Esc                    close
//
// Best times, one per level, are kept in /mnt/fat/MINES.HSC, the file
// Memento's built-in Mines used. The level last chosen is kept on the RAM
// disk, /mnt/tmp/MINES.CFG, until reboot.

import (
	"strconv"
	"strings"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

// Magic is "MNS1", also written into the ELF's window metadata; Memento's
// Minesweeper window supplies the same.
const magic = 0x31534e4d

const (
	bestPath  = "/mnt/fat/MINES.HSC"
	levelPath = "/mnt/tmp/MINES.CFG"
)

// The layout is in units of two pixels, the coordinates the built-in window
// was written in, and drawn k pixels a unit: 2 as the window opens, more in
// a maximised one, always whole pixels so every edge lands on one.
const (
	cs    = 8  // a cell
	top   = 16 // the counter, the face and the clock
	foot  = 11 // the key hints
	unitW = MaxW*cs + 6
	unitH = top + MaxH*cs + 4 + foot
)

func options() memento.Options {
	return memento.Options{Magic: magic, Title: "Minesweeper", Width: unitW * 2, Height: unitH * 2}
}

// Store is where the app keeps its two small files; Beep sounds the speaker.
// Both are the r2 syscalls on r2 and stand-ins in tests.
type Store struct {
	Read  func(name string) ([]byte, error)
	Write func(name string, data []byte) error
}

type app struct {
	game       *Game
	best       [3]uint32 // seconds, 0 for none yet
	newBest    bool
	cx, cy     int  // the keyboard cursor
	showCursor bool // only once a key has moved it
	pressFace  bool
	now        uint64 // milliseconds, as of the last idle turn
	store      Store
	beep       func(freq, ms uint16)
}

func newApp(store Store, beep func(freq, ms uint16), seed uint64) *app {
	a := &app{store: store, beep: beep}
	a.loadBest()
	a.game = NewGame(a.loadLevel(), seed)
	return a
}

func (a *app) hooks() memento.Hooks {
	return memento.Hooks{
		OnEvent: a.event,
		OnPaint: a.paint,
		OnIdle: func(w *memento.Window, now uint64) {
			a.now = now
			if a.game.Tick(now) {
				w.Repaint()
			}
		},
	}
}

// ── Files ────────────────────────────────────────────────────────────────

// numbers reads the unsigned numbers in a file written as one zero-padded
// sector: "12 85 0\n" and NULs after it.
func numbers(b []byte) []uint32 {
	var out []uint32
	for _, f := range strings.Fields(strings.TrimRight(string(b), "\x00")) {
		v, err := strconv.ParseUint(f, 10, 32)
		if err != nil {
			break
		}
		out = append(out, uint32(v))
	}
	return out
}

func (a *app) loadBest() {
	b, err := a.store.Read(bestPath)
	if err != nil {
		return
	}
	for i, v := range numbers(b) {
		if i < len(a.best) {
			a.best[i] = v
		}
	}
}

func (a *app) saveBest() {
	line := strconv.Itoa(int(a.best[0])) + " " + strconv.Itoa(int(a.best[1])) + " " + strconv.Itoa(int(a.best[2])) + "\n"
	_ = a.store.Write(bestPath, []byte(line))
}

// The level is 0..2 here and 1..3 in the file, as the keys say it.
func (a *app) loadLevel() int {
	b, err := a.store.Read(levelPath)
	if err != nil {
		return 1
	}
	for _, line := range strings.Split(strings.TrimRight(string(b), "\x00"), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(line), "level="); ok {
			if n, err := strconv.Atoi(v); err == nil && n >= 1 && n <= 3 {
				return n - 1
			}
		}
	}
	return 1
}

func (a *app) saveLevel() {
	_ = a.store.Write(levelPath, []byte("level="+strconv.Itoa(a.game.Level+1)+"\n"))
}

// ── Play ─────────────────────────────────────────────────────────────────

func (a *app) newGame(level int) {
	a.game.Reset(level)
	a.newBest = false
	a.cx, a.cy = min(a.cx, a.game.W-1), min(a.cy, a.game.H-1)
}

// act does what a click or Space does, then the sounds and the record for
// a game that has just ended.
func (a *app) act(x, y int, flag bool) {
	g := a.game
	was := g.State
	if flag {
		g.ToggleFlag(x, y)
	} else {
		g.Act(x, y, a.now)
	}
	if was == g.State {
		return
	}
	switch g.State {
	case Lost:
		a.beep(262, 80) // C4
	case Won:
		if a.best[g.Level] == 0 || g.Seconds < a.best[g.Level] {
			a.best[g.Level] = g.Seconds
			a.newBest = true
			a.saveBest()
		}
		a.beep(523, 40) // C5
		a.beep(592, 60)
	}
}

// layout is where the units are drawn in a window of this size: k pixels a
// unit from (ox, oy), centred.
func layout(width, height int) (k, ox, oy int) {
	k = max(1, min(width/unitW, height/unitH))
	return k, max(0, (width-unitW*k)/2), max(0, (height-unitH*k)/2)
}

// Where the board is drawn: centred in the layout, whatever its size.
func (a *app) boardX() int { return (unitW - a.game.W*cs) / 2 }
func (a *app) boardY() int { return top + 2 + (MaxH-a.game.H)*cs/2 }
func faceX() int           { return unitW/2 - 8 }

// unit maps a pixel to a unit, rounding down also left of and above the origin.
func unit(p, origin, k int) int {
	if p < origin {
		return -1
	}
	return (p - origin) / k
}

func (a *app) event(w *memento.Window, cmd *memento.Command) {
	switch cmd.Op {
	case memento.OpMouseButton:
		width, height := w.Size()
		k, ox, oy := layout(width, height)
		a.click(unit(int(cmd.X), ox, k), unit(int(cmd.Y), oy, k), cmd.Value != 0, cmd.Extra != 0)
	case memento.OpKey:
		a.key(w, cmd.Key())
	}
}

func (a *app) click(mx, my int, right, pressed bool) {
	// The face: pressed in on the way down, a new game on the way up.
	onFace := my >= 2 && my < 14 && mx >= faceX() && mx < faceX()+16
	if !right && onFace {
		a.pressFace = pressed
		if !pressed {
			a.newGame(a.game.Level)
		}
		return
	}
	a.pressFace = false
	if !pressed || mx < a.boardX() || my < a.boardY() {
		return
	}
	x, y := (mx-a.boardX())/cs, (my-a.boardY())/cs
	if !a.game.Inside(x, y) {
		return
	}
	a.showCursor = false
	a.cx, a.cy = x, y
	a.act(x, y, right)
}

func (a *app) key(w *memento.Window, k memento.Key) {
	if !k.Down {
		return
	}
	c := k.Char
	if !k.IsChar {
		c = 0
	}
	if c >= 'A' && c <= 'Z' {
		c += 'a' - 'A'
	}
	g := a.game
	switch {
	case k.Escape:
		w.Close()
	case c >= '1' && c <= '3':
		a.newGame(int(c - '1'))
		a.saveLevel()
	case c == 'n':
		a.newGame(g.Level)
	case k.ArrowLeft || k.ArrowRight || k.ArrowUp || k.ArrowDown:
		if a.showCursor {
			switch {
			case k.ArrowLeft:
				a.cx = (a.cx + g.W - 1) % g.W
			case k.ArrowRight:
				a.cx = (a.cx + 1) % g.W
			case k.ArrowUp:
				a.cy = (a.cy + g.H - 1) % g.H
			default:
				a.cy = (a.cy + 1) % g.H
			}
		}
		a.showCursor = true
	case c == ' ' || k.Space || k.Enter:
		a.showCursor = true
		a.act(a.cx, a.cy, false)
	case c == 'f':
		a.showCursor = true
		a.act(a.cx, a.cy, true)
	}
}

// ── Drawing ──────────────────────────────────────────────────────────────

type painter struct {
	c          *memento.Canvas
	k, ox, oy  int
	glyphScale int
}

func (p *painter) rect(x, y, w, h int, colour byte) {
	p.c.FillRect(memento.Rect{X: p.ox + x*p.k, Y: p.oy + y*p.k, W: w * p.k, H: h * p.k}, colour)
}

type align int

const (
	begin align = iota
	middle
	end
)

// text puts a line in a box of units, centred from top to bottom and cut to
// the box; a line wider than its box starts at its left.
func (p *painter) text(x, y, w, h int, s string, colour byte, a align) {
	box := memento.Rect{X: p.ox + x*p.k, Y: p.oy + y*p.k, W: w * p.k, H: h * p.k}
	tw, th := len(s)*memento.FontWidth*p.glyphScale, memento.FontHeight*p.glyphScale
	tx, ty := box.X, box.Y+max(0, (box.H-th)/2)
	switch a {
	case middle:
		tx += max(0, (box.W-tw)/2)
	case end:
		tx += max(0, box.W-tw)
	}
	p.c.TextScaled(memento.Rect{X: tx, Y: ty, W: box.X + box.W - tx, H: box.Y + box.H - ty}, s, colour, p.glyphScale)
}

// raised is a key that has not been pressed: light top and left, dark
// bottom and right.
func (p *painter) raised(x, y, w, h int) {
	p.rect(x, y, w, h, memento.LightGrey)
	p.rect(x, y, w, 1, memento.White)
	p.rect(x, y, 1, h, memento.White)
	p.rect(x, y+h-1, w, 1, memento.DarkGrey)
	p.rect(x+w-1, y, 1, h, memento.DarkGrey)
}

func (p *painter) flag(x, y int) {
	p.rect(x+4, y+2, 1, 5, memento.Black) // the pole
	p.rect(x+2, y+2, 2, 1, memento.Red)   // the flag, a small pennant
	p.rect(x+1, y+3, 3, 1, memento.Red)
	p.rect(x+2, y+4, 2, 1, memento.Red)
	p.rect(x+2, y+6, 5, 1, memento.Black) // its foot
}

func (p *painter) mine(x, y int) {
	p.rect(x+2, y+2, 4, 4, memento.Black)
	p.rect(x+1, y+3, 6, 2, memento.Black)
	p.rect(x+3, y+1, 2, 6, memento.Black)
	p.rect(x+3, y+3, 1, 1, memento.White) // the glint
}

var ink = [9]byte{0, memento.Blue, memento.Green, memento.Red, memento.LightBlue, memento.Brown,
	memento.Cyan, memento.Black, memento.DarkGrey}

func (a *app) cell(p *painter, i, x, y int) {
	g := a.game
	c := g.CellAt(i)
	if !c.Open && !(g.State == Lost && c.Mine && !c.Flag) {
		p.raised(x, y, cs, cs)
		if c.Flag {
			p.flag(x, y)
			// A flag on a cell that had no mine, once the game is lost.
			if g.State == Lost && !c.Mine {
				p.rect(x+1, y+1, cs-2, 1, memento.Red)
				p.rect(x+1, y+cs-2, cs-2, 1, memento.Red)
			}
		}
		return
	}
	// Open: flat, with a line on the top and left against its neighbours.
	floor := byte(memento.LightGrey)
	if i == g.Boom {
		floor = memento.LightRed
	}
	p.rect(x, y, cs, cs, floor)
	p.rect(x, y, cs, 1, memento.DarkGrey)
	p.rect(x, y, 1, cs, memento.DarkGrey)
	if c.Mine {
		p.mine(x, y)
	} else if c.Around != 0 {
		p.text(x, y, cs+1, cs, string(rune('0'+c.Around)), ink[c.Around], middle)
	}
}

func (a *app) paint(w *memento.Window, c *memento.Canvas) {
	width, height := w.Size()
	k, ox, oy := layout(width, height)
	// Glyphs grow in whole steps: twice their size from 1.5 times the layout.
	p := &painter{c: c, k: k, ox: ox, oy: oy, glyphScale: max(1, (k+1)/2)}
	g := a.game

	// The top: mines left, the face, the clock.
	p.rect(4, 2, 28, 12, memento.Black)
	p.text(4, 2, 28, 12, strconv.Itoa(g.MinesLeft()), memento.LightRed, middle)
	p.rect(unitW-32, 2, 28, 12, memento.Black)
	p.text(unitW-32, 2, 28, 12, strconv.Itoa(int(g.Seconds)), memento.LightRed, middle)

	face := ":)"
	switch g.State {
	case Lost:
		face = "x("
	case Won:
		face = "B)"
	}
	fx := faceX()
	if a.pressFace {
		p.rect(fx, 2, 16, 12, memento.DarkGrey)
		p.rect(fx+1, 3, 15, 11, memento.LightGrey)
	} else {
		p.raised(fx, 2, 16, 12)
	}
	p.rect(fx+3, 4, 10, 8, memento.Yellow)
	p.text(fx+3, 4, 11, 8, face, memento.Black, middle)

	// The level and its best time, either side of the face.
	p.text(36, 2, fx-40, 12, levels[g.Level].Name, memento.Black, begin)
	best := "best -"
	if a.best[g.Level] != 0 {
		best = "best " + strconv.Itoa(int(a.best[g.Level]))
	}
	bestInk := byte(memento.Black)
	if a.newBest {
		bestInk = memento.Red
	}
	p.text(fx+20, 2, unitW-36-fx-24, 12, best, bestInk, end)

	// The board, sunk into the window.
	bx, by := a.boardX(), a.boardY()
	p.rect(bx-1, by-1, g.W*cs+2, g.H*cs+2, memento.DarkGrey)
	for y := 0; y < g.H; y++ {
		for x := 0; x < g.W; x++ {
			a.cell(p, y*g.W+x, bx+x*cs, by+y*cs)
		}
	}
	if a.showCursor && !g.Over() {
		x, y := bx+a.cx*cs, by+a.cy*cs
		p.rect(x, y, cs, 1, memento.Blue)
		p.rect(x, y+cs-1, cs, 1, memento.Blue)
		p.rect(x, y, 1, cs, memento.Blue)
		p.rect(x+cs-1, y, 1, cs, memento.Blue)
	}

	// What the keys do, or how it ended.
	hint, hintInk := "right click/F flag   N new   1 2 3 level   Esc close", byte(memento.Black)
	switch g.State {
	case Won:
		hint = "Cleared in " + strconv.Itoa(int(g.Seconds)) + " s   N plays again"
		if a.newBest {
			hint = "Cleared in " + strconv.Itoa(int(g.Seconds)) + " s - a new best!   N plays again"
		}
	case Lost:
		hint, hintInk = "Boom.   N or the face plays again", memento.Red
	}
	p.text(4, unitH-foot, unitW-8, foot-1, hint, hintInk, middle)
}
