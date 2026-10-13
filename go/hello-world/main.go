//go:build r2

package main

import (
	"fmt"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func main() {
	options, hooks := hello()
	if err := memento.Run(options, hooks); err != nil {
		fmt.Println("Launch gohello.elf with Files > Run in new window:", err)
	}
}
