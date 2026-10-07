package main

import (
	"runtime"
	"sync/atomic"

	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
)

const streamJob = 100

type streamChunk struct {
	data             [8192]byte
	n                int
	rate, generation uint32
	duration         uint32
	end              bool
	err              error
}

type netJob struct {
	track      model.Track
	generation uint32
	kind       uint32
	id         string
	offset     int
	position   uint32
}
type netResult struct {
	job       netJob
	playlists []model.Playlist
	tracks    []model.Track
	total     int
	err       error
}
type audioJob struct {
	op       uint32
	track    model.Track
	position uint32
}
type audioEvent struct {
	playing, paused    bool
	position, duration uint32
	track              model.Track
	message            string
}

type app struct {
	config                                       model.Config
	offline                                      []model.Playlist
	playlists                                    []model.Playlist
	tracks                                       []model.Track
	loadedPlaylist                               model.Playlist
	pTotal, tTotal, pOffset, tOffset, pSel, tSel int
	live, busy                                   bool
	store                                        *configStore
	restoring                                    *model.SessionState
	lastSession                                  model.SessionState
	loadedPlaylistIndex                          int
	sessionDirty                                 bool
	status                                       string
	audio                                        audioEvent
	netJobs                                      chan netJob
	netResults                                   chan netResult
	audioJobs                                    chan audioJob
	audioEvents                                  chan audioEvent
	audioFinal                                   chan audioEvent
	done                                         chan struct{}
	audioDone                                    chan struct{}
	networkDone                                  chan struct{}
	memory                                       runtime.MemStats
	volume, muted                                uint32
	streamGeneration                             uint32
	streamPCM                                    chan *streamChunk
	streamPool                                   chan *streamChunk
	netProgress                                  chan string
}

func (a *app) offlinePlaylists(offset int) {
	if offset < 0 {
		offset = 0
	}
	if offset >= len(a.offline) {
		return
	}
	end := offset + protocol.Visible
	if end > len(a.offline) {
		end = len(a.offline)
	}
	a.playlists = a.offline[offset:end]
	a.pTotal = len(a.offline)
	a.pOffset = offset
}
func (a *app) selectedTrack() (model.Track, bool) {
	i := a.tSel - a.tOffset
	if i < 0 || i >= len(a.tracks) {
		return model.Track{}, false
	}
	return a.tracks[i], true
}
func (a *app) request(job netJob) {
	if a.busy {
		a.status = "Wait for the current Spotify request."
		return
	}
	select {
	case a.netJobs <- job:
		a.busy = true
		a.status = "Connecting to Spotify..."
	default:
		a.status = "Network command queue is busy; retry."
	}
}
func (a *app) selectPlaylist(index int) {
	if index >= a.pOffset && index < a.pOffset+len(a.playlists) {
		a.pSel = index
		a.restoring = nil
	}
}
func (a *app) openPlaylist(index int) {
	if a.busy {
		a.status = "Wait for the current request before loading a playlist."
		return
	}
	if index < a.pOffset || index >= a.pOffset+len(a.playlists) {
		return
	}
	a.pSel = index
	a.tSel = 0
	a.tOffset = 0
	a.tracks = nil
	a.tTotal = 0
	p := a.playlists[index-a.pOffset]
	a.loadedPlaylist = p
	a.loadedPlaylistIndex = index
	a.restoring = nil
	if a.live {
		a.request(netJob{kind: protocol.SelectPlaylist, id: p.ID})
		return
	}
	end := len(p.Tracks)
	if end > protocol.Visible {
		end = protocol.Visible
	}
	a.tracks = p.Tracks[:end]
	a.tTotal = len(p.Tracks)
}
func (a *app) sendAudio(job audioJob) {
	select {
	case a.audioJobs <- job:
	default:
		a.status = "Audio command queue is full; retry."
	}
}
func (a *app) command(c protocol.Command) {
	switch c.Op {
	case protocol.SelectPlaylist:
		a.selectPlaylist(int(c.Value))
	case protocol.OpenPlaylist:
		a.openPlaylist(int(c.Value))
	case protocol.SelectTrack:
		if int(c.Value) >= a.tOffset && int(c.Value) < a.tOffset+len(a.tracks) {
			a.tSel = int(c.Value)
			a.restoring = nil
		}
	case protocol.PlayPause:
		if a.restoring != nil && a.busy {
			a.status = "Wait for Spotify to restore the previous selection."
			return
		}
		t, ok := a.selectedTrack()
		if !ok {
			return
		}
		a.restoring = nil
		if a.audio.playing && a.audio.track.ID == t.ID && a.audio.track.File == t.File {
			a.sendAudio(audioJob{op: protocol.PlayPause})
			return
		}
		a.sendAudio(audioJob{op: protocol.SelectTrack, track: t, position: model.ResumePosition(a.audio.track, a.audio.position, t)})
	case protocol.Previous, protocol.Next:
		delta := 1
		if c.Op == protocol.Previous {
			delta = -1
		}
		i := a.tSel + delta
		if i < a.tOffset || i >= a.tOffset+len(a.tracks) {
			a.status = "Use the track page buttons to browse more."
			return
		}
		a.tSel = i
		a.restoring = nil
		t, _ := a.selectedTrack()
		a.sendAudio(audioJob{op: protocol.SelectTrack, track: t})
	case protocol.VolumeDown, protocol.VolumeUp:
		volume := atomic.LoadUint32(&a.volume)
		if c.Op == protocol.VolumeDown {
			if volume >= 10 {
				volume -= 10
			} else {
				volume = 0
			}
		} else if volume <= 90 {
			volume += 10
		} else {
			volume = 100
		}
		atomic.StoreUint32(&a.volume, volume)
	case protocol.Mute:
		atomic.StoreUint32(&a.muted, 1-atomic.LoadUint32(&a.muted))
	case protocol.Collect:
		runtime.GC()
		a.status = "Go garbage collection completed."
	case protocol.Stop:
		a.sendAudio(audioJob{op: protocol.Stop})
	case protocol.Refresh:
		if a.busy {
			return
		}
		if a.config.AccessToken == "" && a.config.RefreshToken == "" {
			a.status = "Add Spotify credentials to SPOTIFY.CFG, then reopen this window."
			return
		}
		offset := 0
		if a.restoring != nil {
			offset = pageOffset(a.restoring.Playlist.Index)
		}
		a.request(netJob{kind: protocol.Refresh, offset: offset})
	case protocol.PlaylistPage:
		if a.busy {
			return
		}
		offset := a.pOffset + int(int32(c.Value))*protocol.Visible
		if offset < 0 || offset >= a.pTotal {
			return
		}
		a.restoring = nil
		if a.live {
			a.request(netJob{kind: protocol.PlaylistPage, offset: offset})
		} else {
			a.offlinePlaylists(offset)
			a.selectPlaylist(offset)
		}
	case protocol.TrackPage:
		if a.busy {
			return
		}
		offset := a.tOffset + int(int32(c.Value))*protocol.Visible
		if offset < 0 || offset >= a.tTotal {
			return
		}
		a.restoring = nil
		if a.live {
			p := a.loadedPlaylist
			a.request(netJob{kind: protocol.TrackPage, id: p.ID, offset: offset})
		} else {
			p := a.loadedPlaylist
			end := offset + protocol.Visible
			if end > len(p.Tracks) {
				end = len(p.Tracks)
			}
			a.tracks = p.Tracks[offset:end]
			a.tOffset = offset
			a.tSel = offset
		}
	}
}
func (a *app) received(r netResult) {
	a.busy = false
	if r.err != nil {
		a.status = r.err.Error()
		return
	}
	a.live = true
	a.status = "Songs loaded. Select a song and press Play."
	if r.job.kind == protocol.Refresh || r.job.kind == protocol.PlaylistPage {
		a.playlists = r.playlists
		a.pTotal = r.total
		a.pOffset = r.job.offset
		a.pSel = a.pOffset
		a.tracks = nil
		a.tTotal = 0
		a.tSel = 0
		a.tOffset = 0
		a.loadedPlaylist = model.Playlist{}
		a.status = "Select a playlist and press Enter to load songs."
	} else {
		a.tracks = r.tracks
		a.tTotal = r.total
		a.tOffset = r.job.offset
		a.tSel = a.tOffset
	}
	a.restoreResult(r)
}
