// Package protocol defines the pointer-free shared block used by spotify.elf
// and Memento. host.h is its C++ counterpart. Only atomic control words are
// shared concurrently; snapshots are protected by front/reading ownership.
package protocol

import "unsafe"

const (
	Magic     = 0x50533252 // "R2SP"
	Version   = 3
	NoBuffer  = 2
	QueueSize = 32
	Visible   = 10
)

const (
	SelectPlaylist = iota + 1
	SelectTrack
	PlayPause
	Previous
	Next
	Refresh
	PlaylistPage
	TrackPage
	Stop
	Collect
	VolumeDown
	VolumeUp
	Mute
	OpenPlaylist
)

type Command struct{ Op, Value uint32 }

type Row struct {
	Index uint32
	Text  [72]byte
}

type Snapshot struct {
	PlaylistCount, TrackCount               uint32
	PlaylistSelected, TrackSelected         uint32
	PlaylistOffset, TrackOffset             uint32
	Playing, Paused, PositionMS, DurationMS uint32
	HeapBytes, StackBytes, Goroutines       uint32
	Volume, Muted                           uint32
	Status                                  [128]byte
	NowPlaying                              [96]byte
	Playlists                               [Visible]Row
	Tracks                                  [Visible]Row
}

type Block struct {
	Magic, Version                         uint32
	ClientBeat, Frame, Front, Exited, Tail uint32 // client writes
	HostBeat, Reading, Quit, Head          uint32 // host writes
	Commands                               [QueueSize]Command
	Snapshots                              [2]Snapshot
}

func Text(dst []byte, s string) {
	for i := range dst {
		dst[i] = 0
	}
	// Memento's small font is single-byte; do not split UTF-8 into garbage.
	n := 0
	for _, r := range s {
		if n == len(dst)-1 {
			break
		}
		if r < 32 || r > 126 {
			r = '?'
		}
		dst[n] = byte(r)
		n++
	}
}

const (
	_ = uint(unsafe.Sizeof(Row{}) - 76)
	_ = uint(76 - unsafe.Sizeof(Row{}))
	_ = uint(unsafe.Sizeof(Snapshot{}) - 1804)
	_ = uint(1804 - unsafe.Sizeof(Snapshot{}))
	_ = uint(unsafe.Sizeof(Block{}) - 3908)
	_ = uint(3908 - unsafe.Sizeof(Block{}))
)
