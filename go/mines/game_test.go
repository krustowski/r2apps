package main

import (
	"errors"
	"strings"
	"testing"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func keyChar(c byte) memento.Key { return memento.Key{Down: true, IsChar: true, Char: c} }

// A board laid by hand: playing, mines where the string has '*', one row
// per line.
func board(t *testing.T, rows ...string) *Game {
	t.Helper()
	g := NewGame(0, 1)
	g.W, g.H, g.Mines = len(rows[0]), len(rows), 0
	for y, row := range rows {
		for x, ch := range row {
			if ch == '*' {
				g.At(x, y).Mine = true
				g.Mines++
			}
		}
	}
	g.countAround()
	g.State = Playing
	return g
}

func TestFirstOpenIsAnOpening(t *testing.T) {
	for seed := uint64(1); seed <= 200; seed++ {
		g := NewGame(2, seed)
		x, y := int(seed%30), int(seed%16)
		g.Open(x, y, 5000)
		if g.State != Playing && g.State != Won {
			t.Fatalf("seed %d: first open ended the game", seed)
		}
		mines := 0
		for i := 0; i < g.W*g.H; i++ {
			if g.Cells[i].Mine {
				mines++
				if near(i%g.W, i/g.W, x, y) {
					t.Fatalf("seed %d: a mine next to the first cell", seed)
				}
			}
		}
		if mines != 99 || g.Opened < 1 || g.StartedAt != 5000 {
			t.Fatalf("seed %d: %d mines laid, %d opened", seed, mines, g.Opened)
		}
	}
}

func TestFloodChordFlagsAndLoss(t *testing.T) {
	g := board(t,
		"....",
		"....",
		"..*.",
		"....")
	g.Open(0, 0, 0)
	// The zeros flood to every cell but the mine's ring's far side.
	if !g.At(3, 0).Open || !g.At(0, 3).Open || g.At(3, 3).Open || g.At(2, 2).Open {
		t.Fatal("flood fill opened the wrong cells")
	}
	// A chord with the flags not all placed opens nothing.
	g.Chord(1, 2, 0)
	if g.At(3, 3).Open {
		t.Fatal("chord without its flag")
	}
	g.ToggleFlag(2, 2)
	if g.Flags != 1 || g.MinesLeft() != 0 {
		t.Fatal("flag not counted")
	}
	// Now each number round the flag opens the rest of its neighbours.
	g.Chord(1, 2, 0)
	g.Chord(3, 1, 0)
	if !g.At(2, 3).Open || !g.At(3, 2).Open || g.State != Playing {
		t.Fatal("chords did not open round the flag")
	}
	g.Chord(3, 2, 0)
	if g.State != Won || !g.At(3, 3).Open {
		t.Fatalf("chord did not finish the board: state %d", g.State)
	}

	g = board(t, "*.", "..")
	g.ToggleFlag(1, 1) // a wrong flag
	g.At(1, 0).Open = true
	g.Opened = 1
	g.Chord(1, 0, 0) // its one flag is there: opens round it, the mine too
	if g.State != Lost || g.Boom != 0 {
		t.Fatal("a wrong flag's chord should open the mine")
	}
	g.ToggleFlag(1, 1)
	if g.Flags != 1 {
		t.Fatal("flags change after the game ended")
	}
}

func TestWinFlagsTheRestAndStopsTheClock(t *testing.T) {
	g := board(t, "*..", "...", "...")
	g.StartedAt = 1000
	g.Open(2, 2, 4500)
	if g.State != Won || g.Flags != 1 || !g.At(0, 0).Flag || g.Seconds != 3 {
		t.Fatalf("won %v, flags %d, seconds %d", g.State == Won, g.Flags, g.Seconds)
	}
	if g.Tick(99000) || g.Seconds != 3 {
		t.Fatal("the clock ran on after the game")
	}
	g = NewGame(1, 7)
	g.Open(4, 4, 0)
	if g.State == Playing && (!g.Tick(2_000_000) || g.Seconds != 999) {
		t.Fatal("the clock stops at 999")
	}
}

type files map[string]string

func (f files) store() Store {
	return Store{
		Read: func(name string) ([]byte, error) {
			v, ok := f[name]
			if !ok {
				return nil, errors.New("missing")
			}
			return []byte(v), nil
		},
		Write: func(name string, data []byte) error { f[name] = string(data); return nil },
	}
}

func TestRecordsLevelAndInput(t *testing.T) {
	f := files{bestPath: "12 85 0\n" + strings.Repeat("\x00", 40), levelPath: "level=1\n"}
	beeps := 0
	a := newApp(f.store(), func(uint16, uint16) { beeps++ }, 3)
	if a.best != [3]uint32{12, 85, 0} || a.game.Level != 0 {
		t.Fatalf("loaded %v, level %d", a.best, a.game.Level)
	}

	// 3 is expert, and is remembered for the next window.
	a.key(nil, keyChar('3'))
	if a.game.Level != 2 || f[levelPath] != "level=3\n" || a.loadLevel() != 2 {
		t.Fatalf("level %d saved as %q", a.game.Level, f[levelPath])
	}

	// A click maps from pixels to the cell: 2 pixels a unit as it opens.
	k, ox, oy := layout(unitW*2, unitH*2)
	if k != 2 || ox != 0 || oy != 0 {
		t.Fatal("the window opens at 2 pixels a unit")
	}
	if k, _, _ := layout(unitW*3+5, unitH*4); k != 3 {
		t.Fatal("a bigger window draws bigger, in whole pixels")
	}
	a.now = 10_000
	a.click(a.boardX()+cs*3+1, a.boardY()+cs*2+1, false, true)
	if a.game.State == Ready || !a.game.At(3, 2).Open {
		t.Fatal("click did not open its cell")
	}

	// The face: pressed in on the way down, a new game on the way up.
	a.click(faceX()+1, 3, false, true)
	if !a.pressFace || a.game.State == Ready {
		t.Fatal("face press")
	}
	a.click(faceX()+1, 3, false, false)
	if a.pressFace || a.game.State != Ready {
		t.Fatal("face release starts a new game")
	}

	// Winning on beginner beats 12 seconds with 5 and saves it; losing beeps.
	a.newGame(0)
	g := a.game
	g.Cells = [MaxW * MaxH]Cell{}
	g.At(0, 0).Mine = true
	g.Mines = 1
	g.countAround()
	g.State, g.StartedAt = Playing, 0
	a.now = 5_400
	a.act(8, 8, false)
	if g.State != Won || a.best[0] != 5 || !a.newBest || !strings.HasPrefix(f[bestPath], "5 85 0\n") || beeps != 2 {
		t.Fatalf("record %v, file %q, beeps %d", a.best, f[bestPath], beeps)
	}
	a.newGame(0)
	a.game.At(0, 0).Mine = true
	a.game.countAround()
	a.game.State = Playing
	a.act(0, 0, false)
	if a.game.State != Lost || beeps != 3 || a.newBest {
		t.Fatal("losing")
	}

	// Keys: arrows show the cursor first, then move it; F flags under it.
	a.newGame(0)
	a.cx, a.cy, a.showCursor = 0, 0, false
	a.key(nil, memento.Key{Down: true, ArrowRight: true})
	a.key(nil, memento.Key{Down: true, ArrowRight: true})
	a.key(nil, keyChar('f'))
	if !a.showCursor || a.cx != 1 || !a.game.At(a.cx, a.cy).Flag {
		t.Fatalf("cursor at %d,%d", a.cx, a.cy)
	}
}
