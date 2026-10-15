package main

// The rules of Minesweeper, with no drawing, input or files in them.
//
// The three boards everyone knows: 9x9 with 10 mines, 16x16 with 40 and 30x16
// with 99. The first cell opened is never a mine, nor is any cell around it,
// so every game starts with an opening rather than a guess; the mines are laid
// only then.

type Level struct {
	W, H, Mines int
	Name        string
}

var levels = [3]Level{
	{9, 9, 10, "Beginner"},
	{16, 16, 40, "Intermediate"},
	{30, 16, 99, "Expert"},
}

const MaxW, MaxH = 30, 16

type State uint8

const (
	Ready   State = iota // no cell open yet: the mines are not laid
	Playing              // the clock runs
	Won
	Lost
)

type Cell struct {
	Mine, Open, Flag bool
	Around           uint8 // mines in the eight neighbours
}

type Game struct {
	Level         int
	W, H, Mines   int
	Cells         [MaxW * MaxH]Cell
	State         State
	Opened, Flags int
	Boom          int    // the mine that went off, or -1
	StartedAt     uint64 // milliseconds at the first open
	Seconds       uint32 // shown on the clock; frozen when the game ends
	rng           uint64
}

func NewGame(level int, seed uint64) *Game {
	if seed == 0 {
		seed = 0x9E3779B97F4A7C15
	}
	g := &Game{rng: seed}
	g.Reset(level)
	return g
}

// Reset starts a game at level 0..2 on a fresh board.
func (g *Game) Reset(level int) {
	if level < 0 || level >= len(levels) {
		level = 1
	}
	l := levels[level]
	g.Level, g.W, g.H, g.Mines = level, l.W, l.H, l.Mines
	g.Cells = [MaxW * MaxH]Cell{}
	g.State, g.Opened, g.Flags, g.Boom, g.Seconds = Ready, 0, 0, -1, 0
}

func (g *Game) Over() bool           { return g.State == Won || g.State == Lost }
func (g *Game) Inside(x, y int) bool { return x >= 0 && y >= 0 && x < g.W && y < g.H }
func (g *Game) At(x, y int) *Cell    { return &g.Cells[y*g.W+x] }
func (g *Game) MinesLeft() int       { return g.Mines - g.Flags }
func (g *Game) CellAt(i int) *Cell   { return &g.Cells[i] }
func (g *Game) random(bound int) int { g.next(); return int(g.rng % uint64(bound)) }
func (g *Game) next()                { g.rng ^= g.rng << 13; g.rng ^= g.rng >> 7; g.rng ^= g.rng << 17 }
func near(x, y, cx, cy int) bool     { return x >= cx-1 && x <= cx+1 && y >= cy-1 && y <= cy+1 }

// layMines puts the mines anywhere but the 3x3 around the first cell opened.
// The n-th free cell is counted afresh each time: no retry loop to run long
// on a crowded board.
func (g *Game) layMines(fx, fy int) {
	free := 0
	for y := 0; y < g.H; y++ {
		for x := 0; x < g.W; x++ {
			if !near(x, y, fx, fy) {
				free++
			}
		}
	}
	for toLay := min(g.Mines, free); toLay > 0; toLay, free = toLay-1, free-1 {
		n := g.random(free)
		for i := 0; i < g.W*g.H; i++ {
			if g.Cells[i].Mine || near(i%g.W, i/g.W, fx, fy) {
				continue
			}
			if n == 0 {
				g.Cells[i].Mine = true
				break
			}
			n--
		}
	}
	g.countAround()
}

// countAround writes each cell's count of mines in its eight neighbours.
func (g *Game) countAround() {
	for y := 0; y < g.H; y++ {
		for x := 0; x < g.W; x++ {
			n := 0
			for dy := -1; dy <= 1; dy++ {
				for dx := -1; dx <= 1; dx++ {
					if (dx != 0 || dy != 0) && g.Inside(x+dx, y+dy) && g.At(x+dx, y+dy).Mine {
						n++
					}
				}
			}
			g.At(x, y).Around = uint8(n)
		}
	}
}

// Open opens a cell, and every cell around a zero with it. now is in
// milliseconds; the first open lays the mines and starts the clock.
func (g *Game) Open(x, y int, now uint64) {
	if g.Over() || !g.Inside(x, y) {
		return
	}
	if g.State == Ready {
		g.layMines(x, y)
		g.State, g.StartedAt = Playing, now
	}
	c := g.At(x, y)
	if c.Open || c.Flag {
		return
	}
	if c.Mine {
		c.Open = true
		g.Boom = y*g.W + x
		g.finish(Lost, now)
		return
	}
	// Flood through the zeros with an explicit stack: each cell goes on it once.
	var stack [MaxW * MaxH]int
	top := 0
	c.Open = true
	g.Opened++
	stack[top] = y*g.W + x
	top++
	for top > 0 {
		top--
		i := stack[top]
		if g.Cells[i].Around != 0 {
			continue
		}
		ix, iy := i%g.W, i/g.W
		for dy := -1; dy <= 1; dy++ {
			for dx := -1; dx <= 1; dx++ {
				nx, ny := ix+dx, iy+dy
				if !g.Inside(nx, ny) {
					continue
				}
				n := g.At(nx, ny)
				if n.Open || n.Flag || n.Mine {
					continue
				}
				n.Open = true
				g.Opened++
				stack[top] = ny*g.W + nx
				top++
			}
		}
	}
	if g.Opened == g.W*g.H-g.Mines {
		g.finish(Won, now)
		// Every mine left is flagged for the player: nothing else it could be.
		for i := 0; i < g.W*g.H; i++ {
			if g.Cells[i].Mine && !g.Cells[i].Flag {
				g.Cells[i].Flag = true
				g.Flags++
			}
		}
	}
}

// Chord opens the rest around an open number whose flags are all placed.
func (g *Game) Chord(x, y int, now uint64) {
	c := g.At(x, y)
	if !c.Open || c.Around == 0 || g.State != Playing {
		return
	}
	flags := 0
	for dy := -1; dy <= 1; dy++ {
		for dx := -1; dx <= 1; dx++ {
			if g.Inside(x+dx, y+dy) && g.At(x+dx, y+dy).Flag {
				flags++
			}
		}
	}
	if flags != int(c.Around) {
		return
	}
	for dy := -1; dy <= 1; dy++ {
		for dx := -1; dx <= 1; dx++ {
			if (dx != 0 || dy != 0) && g.Inside(x+dx, y+dy) {
				g.Open(x+dx, y+dy, now)
			}
		}
	}
}

func (g *Game) ToggleFlag(x, y int) {
	if g.Over() || !g.Inside(x, y) {
		return
	}
	c := g.At(x, y)
	if c.Open {
		return
	}
	c.Flag = !c.Flag
	if c.Flag {
		g.Flags++
	} else {
		g.Flags--
	}
}

// Act is a click or Space: open a closed cell, chord an open one.
func (g *Game) Act(x, y int, now uint64) {
	if !g.Inside(x, y) {
		return
	}
	if g.At(x, y).Open {
		g.Chord(x, y, now)
	} else {
		g.Open(x, y, now)
	}
}

// The clock reads whole seconds, up to 999 as it always has.
func (g *Game) clock(now uint64) uint32 {
	if now < g.StartedAt {
		return 0
	}
	return uint32(min((now-g.StartedAt)/1000, 999))
}

// finish ends the game with the clock stopped where it is.
func (g *Game) finish(state State, now uint64) {
	g.Seconds = g.clock(now)
	g.State = state
}

// Tick brings the running clock up to now and reports whether the number
// shown changed.
func (g *Game) Tick(now uint64) bool {
	if g.State != Playing {
		return false
	}
	s := g.clock(now)
	changed := s != g.Seconds
	g.Seconds = s
	return changed
}
