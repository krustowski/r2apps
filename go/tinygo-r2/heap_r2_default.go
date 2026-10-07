//go:build r2 && !r2largeheap

package runtime

// Small applications keep the collector arena provided by the linker script.
func initR2Heap() {}
