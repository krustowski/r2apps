// Package protocol defines the pointer-free shared block used by spotify.elf
// and Memento. host.h is its C++ counterpart. The window transport lives in
// libgor2/memento; only Spotify's operations and snapshot belong here.
package protocol

import (
	"unsafe"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento"
)

const (
	Magic     = 0x50533252 // "R2SP"
	Version   = 4
	NoBuffer  = memento.NoBuffer
	QueueSize = memento.QueueSize
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

const (
	ExitHostClosed  = memento.ExitHostClosed
	ExitHostTimeout = memento.ExitHostTimeout
	ExitBadQueue    = memento.ExitBadQueue
	ExitRuntime     = memento.ExitRuntime
)

type Command = memento.Command

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

type Block = memento.Block[Snapshot]

func Text(dst []byte, s string) {
	memento.Text(dst, s)
}

const (
	_ = uint(unsafe.Sizeof(Row{}) - 76)
	_ = uint(76 - unsafe.Sizeof(Row{}))
	_ = uint(unsafe.Sizeof(Snapshot{}) - 1804)
	_ = uint(1804 - unsafe.Sizeof(Snapshot{}))
	_ = uint(unsafe.Sizeof(Block{}) - 4040)
	_ = uint(4040 - unsafe.Sizeof(Block{}))
)
