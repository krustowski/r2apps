//go:build r2

package main

import (
	"fmt"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func main() {
	a := newApp(Store{Read: r2.ReadFile, Write: r2.WriteFile},
		func(freq, ms uint16) { _ = r2.PlayFreq(freq, ms) }, r2.Ticks()^0x9E3779B97F4A7C15)
	if err := memento.Run(options(), a.hooks()); err != nil {
		fmt.Println("mines.elf is Memento's Minesweeper window: open it from the desktop or Files > Run in new window:", err)
	}
}
