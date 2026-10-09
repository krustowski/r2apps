//go:build !r2

package media

func alloc(n int) []byte {
	if n <= 0 {
		return nil
	}
	return make([]byte, n)
}

func release([]byte) {}
