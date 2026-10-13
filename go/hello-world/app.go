package main

import (
	"strconv"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

// Magic is "GOH1", also written into the ELF's build-time manifest.
// Memento supplies the window frame;
// Go supplies the contents, using the same font and palette as the desktop.
const magic = 0x31484f47

var helloButton = memento.Rect{X: 16, Y: 76, W: 144, H: 28}

func hello() (memento.Options, memento.Hooks) {
	count, status := 0, "Click Say hello, or press Enter."
	return memento.Options{Magic: magic, Title: "Hello / Go", Width: 480, Height: 240}, memento.Hooks{
		OnEvent: func(w *memento.Window, event *memento.Command) {
			key := event.Key()
			if event.Op == memento.OpKey && key.Down && key.Escape {
				w.Close()
				return
			}
			if event.LeftPressed(helloButton) || (event.Op == memento.OpKey && key.Down && key.Enter) {
				count++
				status = "Hello number " + strconv.Itoa(count) + "!"
				if w.Copy("Hello, world from Go!") {
					status += " Greeting copied."
				}
				w.Notify("Hello, world from Go!")
				w.SetTitle("Hello / Go (" + strconv.Itoa(count) + ")")
			}
		},
		OnPaint: func(w *memento.Window, c *memento.Canvas) {
			width, height := w.Size()
			c.FillRect(memento.Rect{W: width, H: 32}, memento.Blue)
			c.Text(memento.Rect{X: 16, Y: 10, W: width - 32, H: 12}, "Hello, world from Go!", memento.White)
			c.Text(memento.Rect{X: 16, Y: 48, W: width - 32, H: 12}, "A Go app in a Memento window.", memento.Black)
			c.Button(helloButton, "Say hello", false)
			c.Text(memento.Rect{X: 16, Y: 120, W: width - 32, H: 12}, status, memento.Black)
			c.Text(memento.Rect{X: 16, Y: height - 24, W: width - 32, H: 12}, "Enter: say hello   Esc: close", memento.DarkGrey)
		},
	}
}
