//go:build !r2

package main

import (
	"fmt"
	"os"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

// On the build host, the window metadata the Makefile appends to mines.elf.
func main() {
	if len(os.Args) == 2 && os.Args[1] == "--memento-manifest" {
		if err := memento.WriteManifest(os.Stdout, options(), false, false); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	fmt.Println("Build with make build; Memento's Minesweeper icon or Files > Run in new window opens mines.elf.")
}
