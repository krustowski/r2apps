package shannon

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"testing"
)

// This full-size AP packet vector was recorded before the allocation changes.
func TestLargePacketVectorAndFragments(t *testing.T) {
	plain := make([]byte, 65535)
	for i := range plain {
		plain[i] = byte(i*17 + 3)
	}
	cipher := append([]byte(nil), plain...)
	s := New(key)
	s.NonceU32(33)
	s.Encrypt(cipher)
	var mac [16]byte
	s.Finish(mac[:])
	if fmt.Sprintf("%x", sha256.Sum256(cipher)) != "11c9f94857231b7a95c767df97ef264a26ba4f8a9cc343b97ae854f047b55e6d" || fmt.Sprintf("%x", mac) != "ce77b433a0f8a90a1be591760f11ca80" {
		t.Fatal("full-size packet vector changed")
	}
	for _, size := range []int{1, 2, 3, 4, 7, 4096, 16384} {
		decoded := append([]byte(nil), cipher...)
		s := New(key)
		s.NonceU32(33)
		for offset := 0; offset < len(decoded); offset += size {
			end := offset + size
			if end > len(decoded) {
				end = len(decoded)
			}
			s.Decrypt(decoded[offset:end])
		}
		if !bytes.Equal(decoded, plain) {
			t.Fatalf("fragment size %d: plaintext mismatch", size)
		}
		var actual [16]byte
		s.Finish(actual[:])
		if actual != mac {
			t.Fatalf("fragment size %d: MAC mismatch", size)
		}
	}
}

func TestPacketProcessingDoesNotAllocate(t *testing.T) {
	buf := make([]byte, 65535)
	s := New(key)
	var mac [16]byte
	allocations := testing.AllocsPerRun(20, func() {
		s.NonceU32(33)
		s.Encrypt(buf)
		s.Finish(mac[:])
		s.NonceU32(33)
		s.Decrypt(buf)
		s.Finish(mac[:])
	})
	if allocations != 0 {
		t.Fatalf("packet processing allocates %.0f objects", allocations)
	}
}
