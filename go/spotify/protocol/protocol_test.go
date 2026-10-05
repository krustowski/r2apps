package protocol

import (
	"testing"
	"unsafe"
)

func TestSharedOffsets(t *testing.T) {
	b := Block{}
	s := Snapshot{}
	if unsafe.Sizeof(b) != 4040 || unsafe.Offsetof(b.ExitReason) != 3908 || unsafe.Offsetof(b.RuntimeText) != 3912 || unsafe.Offsetof(b.Snapshots) != 300 || unsafe.Offsetof(b.Commands) != 44 || unsafe.Offsetof(s.Playlists) != 284 || unsafe.Offsetof(s.Tracks) != 1044 {
		t.Fatal("shared ABI offset mismatch")
	}
	var text [8]byte
	Text(text[:], "Aé\nlonger")
	if string(text[:]) != "A??long\x00" {
		t.Fatalf("%q", text)
	}
}
