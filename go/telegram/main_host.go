//go:build !r2

package main

import "fmt"

// Off r2 there is nothing to host the window; the client is exercised by
// its tests (make test).
func main() { fmt.Println("telegram.elf runs on r2, hosted by Memento: make builds it.") }
