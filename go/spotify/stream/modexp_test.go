package stream

import (
	"bytes"
	"encoding/hex"
	"math/big"
	"testing"
)

func TestModexpProtocolSizes(t *testing.T) {
	for _, modulus := range []string{dhPrimeHex, serverKeyHex} {
		m, _ := hex.DecodeString(modulus)
		for _, exponent := range [][]byte{{1, 0, 1}, bytes.Repeat([]byte{0x81}, 95)} {
			base := []byte{0x43, 0x76, 0x32, 0x13}
			actual, e := modexp(base, exponent, m)
			if e != nil {
				t.Fatal(e)
			}
			expected := new(big.Int).Exp(new(big.Int).SetBytes(base), new(big.Int).SetBytes(exponent), new(big.Int).SetBytes(m)).FillBytes(make([]byte, len(m)))
			if !bytes.Equal(actual, expected) {
				t.Fatal("modular exponentiation differs at protocol size")
			}
		}
	}
	if _, e := modexp([]byte{13}, []byte{3}, []byte{13}); e == nil {
		t.Fatal("out-of-range base accepted")
	}
}
func TestGIDKnownCaseOrdering(t *testing.T) {
	for _, id := range []string{"000000000000000000000a", "000000000000000000000A", "4uLU6hMCjMI75M1A2tKUQC"} {
		actual, e := GID(id)
		if e != nil {
			t.Fatal(e)
		}
		n, ok := new(big.Int).SetString(id, 62)
		if !ok {
			t.Fatal("invalid fixture")
		}
		if !bytes.Equal(actual, n.FillBytes(make([]byte, 16))) {
			t.Fatal("base62 digit ordering mismatch")
		}
	}
}
