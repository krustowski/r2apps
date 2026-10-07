package main

import (
	"sync/atomic"

	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
)

func pageOffset(index int) int {
	if index < 0 {
		return 0
	}
	return index / protocol.Visible * protocol.Visible
}

func playlistIndex(list []model.Playlist, saved model.PlaylistSelection, offset int) int {
	for i, p := range list {
		if saved.ID != "" && p.ID == saved.ID {
			return offset + i
		}
	}
	if saved.Index >= offset && saved.Index < offset+len(list) {
		return saved.Index
	}
	return offset
}

func trackIndex(list []model.Track, saved model.Track, index, offset int) int {
	for i, t := range list {
		if (saved.ID != "" || saved.File != "") && t.ID == saved.ID && t.File == saved.File {
			return offset + i
		}
	}
	if index >= offset && index < offset+len(list) {
		return index
	}
	return offset
}

func (a *app) restoreSession() {
	if a.config.Session == nil {
		return
	}
	saved := *a.config.Session
	a.lastSession = saved
	volume := saved.Volume
	if volume > 100 {
		volume = 100
	}
	atomic.StoreUint32(&a.volume, volume)
	if saved.Muted {
		atomic.StoreUint32(&a.muted, 1)
	}
	a.audio = audioEvent{track: saved.LastPlayed, position: saved.PositionMS, duration: saved.LastPlayed.DurationMS}
	if saved.Live && (a.config.AccessToken != "" || a.config.RefreshToken != "") {
		a.restoring = &saved
		a.request(netJob{kind: protocol.Refresh, offset: pageOffset(saved.Playlist.Index)})
		return
	}
	pSel := playlistIndex(a.offline, saved.Playlist, 0)
	a.offlinePlaylists(pageOffset(pSel))
	a.pSel = pSel
	loaded := playlistIndex(a.offline, saved.LoadedPlaylist, 0)
	if loaded >= len(a.offline) {
		return
	}
	p := a.offline[loaded]
	a.loadedPlaylist, a.loadedPlaylistIndex = p, loaded
	a.tTotal = len(p.Tracks)
	a.tSel = trackIndex(p.Tracks, saved.SelectedTrack, saved.TrackIndex, 0)
	a.tOffset = pageOffset(a.tSel)
	end := a.tOffset + protocol.Visible
	if end > len(p.Tracks) {
		end = len(p.Tracks)
	}
	a.tracks = p.Tracks[a.tOffset:end]
	a.status = "Previous selection restored. Press Play to continue."
}

func (a *app) restoreResult(r netResult) {
	if a.restoring == nil {
		return
	}
	saved := *a.restoring
	if r.job.kind == protocol.Refresh || r.job.kind == protocol.PlaylistPage {
		if len(r.playlists) == 0 && r.total > 0 && r.job.offset >= r.total {
			a.request(netJob{kind: protocol.Refresh, offset: pageOffset(r.total - 1)})
			return
		}
		a.pSel = playlistIndex(a.playlists, saved.Playlist, a.pOffset)
		if saved.LoadedPlaylist.ID == "" {
			a.restoring = nil
			return
		}
		a.loadedPlaylist = model.Playlist{ID: saved.LoadedPlaylist.ID, Name: saved.LoadedPlaylist.Name}
		a.loadedPlaylistIndex = saved.LoadedPlaylist.Index
		a.request(netJob{kind: protocol.SelectPlaylist, id: saved.LoadedPlaylist.ID, offset: pageOffset(saved.TrackIndex)})
		return
	}
	if len(r.tracks) == 0 && r.total > 0 && r.job.offset >= r.total {
		a.request(netJob{kind: protocol.SelectPlaylist, id: r.job.id, offset: pageOffset(r.total - 1)})
		return
	}
	a.tSel = trackIndex(a.tracks, saved.SelectedTrack, saved.TrackIndex, a.tOffset)
	a.restoring = nil
	a.status = "Previous selection restored. Press Play to continue."
}

func (a *app) sessionState() model.SessionState {
	s := model.SessionState{Live: a.live, TrackIndex: a.tSel,
		LastPlayed: a.audio.track, PositionMS: a.audio.position,
		Volume: atomic.LoadUint32(&a.volume), Muted: atomic.LoadUint32(&a.muted) != 0,
		LoadedPlaylist: model.PlaylistSelection{ID: a.loadedPlaylist.ID, Name: a.loadedPlaylist.Name, Index: a.loadedPlaylistIndex}}
	if i := a.pSel - a.pOffset; i >= 0 && i < len(a.playlists) {
		p := a.playlists[i]
		s.Playlist = model.PlaylistSelection{ID: p.ID, Name: p.Name, Index: a.pSel}
	}
	s.SelectedTrack, _ = a.selectedTrack()
	return s
}

func (a *app) receivedAudio(event audioEvent) {
	if event.playing != a.audio.playing || event.paused != a.audio.paused {
		a.sessionDirty = true
	}
	if event.duration != 0 {
		event.track.DurationMS = event.duration
	}
	a.audio = event
	if event.message != "" {
		a.status = event.message
	}
}

func (a *app) saveSession(force bool) {
	if a.store == nil || a.restoring != nil {
		return
	}
	s := a.sessionState()
	compare := s
	// Checkpoint playback at most every five seconds. Selection, pause, stop,
	// volume changes and shutdown also save the current position.
	if s.PositionMS >= a.lastSession.PositionMS && s.PositionMS-a.lastSession.PositionMS < 5000 {
		compare.PositionMS = a.lastSession.PositionMS
	}
	if !force && !a.sessionDirty && compare == a.lastSession {
		return
	}
	if err := a.store.saveSession(s); err != nil {
		a.status = err.Error()
		return
	}
	a.lastSession = s
	a.sessionDirty = false
}

// discardPCM removes whole stereo frames while resuming a sequential decoder.
// Each caller still decodes only one buffer at a time, yielding between reads.
func discardPCM(pcm []byte, remaining *uint64) []byte {
	n := uint64(len(pcm))
	if n > *remaining {
		n = *remaining
	}
	*remaining -= n
	copy(pcm, pcm[n:])
	return pcm[:uint64(len(pcm))-n]
}

func positionBytes(position, rate uint32) uint64 {
	return uint64(position) * uint64(rate) / 1000 * 4
}
