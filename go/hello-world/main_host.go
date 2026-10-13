//go:build !r2

package main

import (
	"fmt"
	"os"

	memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--memento-manifest" {
		options, _ := hello()
		if err := memento.WriteManifest(os.Stdout, options, false, true); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	fmt.Println("Build with make build, then select gohello.elf in Files and Run in new window.")
}
