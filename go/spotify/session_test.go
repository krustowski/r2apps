package main

import (
	"bytes"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
)

// Mimic WriteFileAt, including its lack of truncation.
type memoryFiles map[string][]byte

func (files memoryFiles) read(path string, limit int) ([]byte, error) {
	b, ok := files[path]
	if !ok || len(b) > limit {
		return nil, errors.New("missing or oversized file")
	}
	return b, nil
}

func (files memoryFiles) write(path string, b []byte) (int, error) {
	if len(files[path]) < len(b) {
		files[path] = append(files[path], make([]byte, len(b)-len(files[path]))...)
	}
	copy(files[path], b)
	return len(b), nil
}

func readSaved(t *testing.T, files memoryFiles, path string) model.Config {
	t.Helper()
	c, err := model.ParseConfig(files[path])
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func sessionApp(c model.Config) *app {
	a := &app{config: c, volume: 100, netJobs: make(chan netJob, 1), audioJobs: make(chan audioJob, 8)}
	a.offline = append([]model.Playlist{model.Demo()}, c.Playlists...)
	a.offlinePlaylists(0)
	a.openPlaylist(0)
	return a
}

func TestTemporaryConfigAndLegacyKeyPrecedence(t *testing.T) {
	key, err := (stream.Credentials{RefreshToken: "legacy-token", Device: strings.Repeat("a", 40)}).Encode()
	if err != nil {
		t.Fatal(err)
	}
	files := memoryFiles{
		configPath:      []byte(`{"client_id":"fat-app","refresh_token":"fat-web","streaming_access_token":"old-access","streaming_refresh_token":"old-playback"}`),
		playbackKeyPath: key,
	}
	s := &configStore{write: files.write}
	if status := s.load(files.read); status != "" || s.config.ClientID != "fat-app" || s.config.StreamingRefreshToken != "legacy-token" || s.config.StreamingDeviceID != strings.Repeat("a", 40) || s.config.StreamingAccessToken != "" || !s.fromFAT {
		t.Fatal("legacy playback key was not imported into boot config")
	}
	if err := s.saveSession(model.SessionState{PositionMS: 1000}); err != nil {
		t.Fatal(err)
	}
	c := s.config
	c.RefreshToken, c.StreamingRefreshToken, c.StreamingDeviceID = "new-web", "new-playback", strings.Repeat("b", 40)
	if err := s.saveCredentials(c); err != nil {
		t.Fatal(err)
	}
	loaded := &configStore{write: files.write}
	if status := loaded.load(files.read); status != "" || loaded.config.RefreshToken != "new-web" || loaded.config.StreamingRefreshToken != "new-playback" || loaded.config.StreamingDeviceID != strings.Repeat("b", 40) || loaded.config.Session.PositionMS != 1000 || !loaded.fromFAT {
		t.Fatal("temporary config did not take precedence over the stale key")
	}
	if len(files[configPath]) >= 4096 || len(files[temporaryConfigPath]) >= 4096 {
		t.Fatal("small configs grew unnecessarily")
	}
}

func TestBootConfigFallbackAndCorruptTemporaryConfig(t *testing.T) {
	files := memoryFiles{
		configPath:                         []byte(`invalid boot JSON`),
		"/mnt/tar/opt/spotify/config.josn": []byte(`{"client_id":"tar-app","refresh_token":"tar-refresh","streaming_refresh_token":"tar-playback"}`),
		"/mnt/tar/opt/spotify/config.json": []byte(`{"client_id":"other-app"}`),
		temporaryConfigPath:                bytes.Repeat([]byte{'x'}, 2000),
		playbackKeyPath:                    []byte(`{"refresh_token":"invalid-device-token","device_id":"bad"}`),
	}
	s := &configStore{write: files.write}
	if status := s.load(files.read); status == "" || s.config.ClientID != "tar-app" || s.config.RefreshToken != "tar-refresh" || s.config.StreamingRefreshToken != "tar-playback" || s.fromFAT {
		t.Fatal("invalid temporary config or key prevented boot-config fallback")
	}
	if err := s.saveSession(model.SessionState{}); err != nil {
		t.Fatal(err)
	}
	if c := readSaved(t, files, temporaryConfigPath); c.ClientID != "tar-app" || len(files[temporaryConfigPath]) != 2000 {
		t.Fatal("a smaller valid replacement left corrupt trailing bytes")
	}
	files[temporaryConfigPath] = []byte(`{"client_id":"temporary-app","refresh_token":"temporary-refresh","session":{"position_ms":12345}}`)
	s = &configStore{write: files.write}
	s.load(files.read)
	if s.config.ClientID != "temporary-app" || s.config.Session.PositionMS != 12345 {
		t.Fatal("valid temporary config did not take precedence over boot config")
	}
}

func TestTemporaryConfigurationAcrossReopen(t *testing.T) {
	c := model.Config{ClientID: "developer-app", RefreshToken: "web-token", StreamingRefreshToken: "playback-token", StreamingDeviceID: strings.Repeat("a", 40)}
	for i := 1; i < 20; i++ {
		p := model.Playlist{ID: fmt.Sprint(i), Name: fmt.Sprint("Playlist ", i)}
		for j := 0; j < 25; j++ {
			p.Tracks = append(p.Tracks, model.Track{ID: fmt.Sprintf("%d-%d", i, j), Name: fmt.Sprint("Song ", j), File: fmt.Sprintf("/mnt/tar/%d-%d.wav", i, j), DurationMS: 200000})
		}
		c.Playlists = append(c.Playlists, p)
	}
	files := memoryFiles{}
	a := sessionApp(c)
	a.store = &configStore{config: c, write: files.write}
	a.offlinePlaylists(10)
	a.openPlaylist(13)
	a.selectPlaylist(16) // Highlighting differs from the playlist whose songs are loaded.
	a.command(protocol.Command{Op: protocol.TrackPage, Value: 1})
	a.command(protocol.Command{Op: protocol.SelectTrack, Value: 12})
	a.audio = audioEvent{track: a.loadedPlaylist.Tracks[13], position: 45678, duration: 200000}
	a.volume, a.muted = 40, 1
	a.saveSession(true)
	c.RefreshToken, c.StreamingRefreshToken = "rotated-web", "rotated-playback"
	if err := a.store.saveCredentials(c); err != nil {
		t.Fatal(err)
	}
	a.saveSession(true) // The UI's original config still has the old credentials.
	loaded := readSaved(t, files, temporaryConfigPath)
	if loaded.RefreshToken != "rotated-web" || loaded.StreamingRefreshToken != "rotated-playback" || loaded.StreamingDeviceID != c.StreamingDeviceID {
		t.Fatal("selection save overwrote rotated credentials")
	}
	reopened := sessionApp(loaded)
	reopened.restoreSession()
	selected, ok := reopened.selectedTrack()
	if !ok || selected.ID != "13-12" || reopened.pSel != 16 || reopened.pOffset != 10 || reopened.loadedPlaylist.ID != "13" || reopened.tOffset != 10 || reopened.tSel != 12 {
		t.Fatal("paged selections did not survive reopening")
	}
	if reopened.audio.track.ID != "13-13" || reopened.audio.position != 45678 || reopened.audio.playing || reopened.volume != 40 || reopened.muted != 1 || len(reopened.audioJobs) != 0 {
		t.Fatal("last played song, mark or volume did not survive reopening")
	}
	reopened.command(protocol.Command{Op: protocol.PlayPause})
	if job := <-reopened.audioJobs; job.position != 0 || job.track.ID != selected.ID {
		t.Fatal("a different selected song resumed the last played song's position")
	}
	reopened.command(protocol.Command{Op: protocol.SelectTrack, Value: 13})
	reopened.command(protocol.Command{Op: protocol.PlayPause})
	if job := <-reopened.audioJobs; job.position != 45678 || job.track.ID != "13-13" {
		t.Fatal("Play did not resume the saved song")
	}
	if _, exists := files[configPath]; exists {
		t.Fatal("ISO-only configuration wrote to FAT")
	}
}

func TestLiveSelectionRestoration(t *testing.T) {
	saved := model.SessionState{Live: true, Volume: 100,
		Playlist:       model.PlaylistSelection{ID: "selected-playlist", Index: 23},
		LoadedPlaylist: model.PlaylistSelection{ID: "1234567890123456789012", Name: "Loaded", Index: 12},
		SelectedTrack:  model.Track{ID: "selected-song"}, TrackIndex: 47,
		LastPlayed: model.Track{ID: "last-played", DurationMS: 200000}, PositionMS: 90000}
	a := sessionApp(model.Config{RefreshToken: "web-token", Session: &saved})
	files := memoryFiles{}
	a.store = &configStore{config: a.config, write: files.write}
	a.restoreSession()
	job := <-a.netJobs
	if job.kind != protocol.Refresh || job.offset != 20 || !a.busy {
		t.Fatal("did not request the saved playlist page")
	}
	a.saveSession(true)
	if len(files) != 0 {
		t.Fatal("incomplete restoration overwrote the saved state")
	}
	a.received(netResult{job: job, total: 30, playlists: []model.Playlist{{ID: "other"}, {ID: "selected-playlist"}}})
	job = <-a.netJobs
	if a.pSel != 21 || job.kind != protocol.SelectPlaylist || job.id != saved.LoadedPlaylist.ID || job.offset != 40 {
		t.Fatal("restoration ignored IDs or the independently loaded playlist")
	}
	a.received(netResult{job: job, total: 50, tracks: []model.Track{{ID: "other"}, {ID: "selected-song"}}})
	if a.tSel != 41 || a.restoring != nil || !a.live || a.busy || a.audio.position != saved.PositionMS || len(a.audioJobs) != 0 {
		t.Fatal("live selection was not restored without starting audio")
	}
	a.saveSession(true)
	if got := readSaved(t, files, temporaryConfigPath).Session; got.Playlist.Index != 21 || got.TrackIndex != 41 || got.LastPlayed.ID != "last-played" {
		t.Fatal("restored positions were not saved")
	}
}

func TestRestorationHandlesShorterPagesAndNetworkFailure(t *testing.T) {
	saved := model.SessionState{Live: true, Volume: 100, Playlist: model.PlaylistSelection{Index: 25}, LoadedPlaylist: model.PlaylistSelection{ID: "1234567890123456789012"}, TrackIndex: 47}
	a := sessionApp(model.Config{RefreshToken: "token", Session: &saved})
	files := memoryFiles{}
	a.store = &configStore{config: a.config, write: files.write}
	a.restoreSession()
	job := <-a.netJobs
	a.received(netResult{job: job, err: errors.New("network unavailable")})
	a.saveSession(true)
	if len(files) != 0 || a.restoring == nil {
		t.Fatal("a temporary network failure discarded the saved live selection")
	}
	a.command(protocol.Command{Op: protocol.Refresh})
	job = <-a.netJobs
	if job.offset != 20 || a.restoring == nil {
		t.Fatal("Refresh did not retry the interrupted restoration")
	}
	a.received(netResult{job: job, total: 3})
	job = <-a.netJobs
	if job.offset != 0 {
		t.Fatal("did not recover from a playlist page past the new total")
	}
	a.received(netResult{job: job, total: 3, playlists: []model.Playlist{{ID: "one"}, {ID: saved.LoadedPlaylist.ID}, {ID: "three"}}})
	job = <-a.netJobs
	a.received(netResult{job: job, total: 12})
	job = <-a.netJobs
	if job.offset != 10 {
		t.Fatal("did not recover from a track page past the new total")
	}
	a.received(netResult{job: job, total: 12, tracks: []model.Track{{ID: "ten"}, {ID: "eleven"}}})
	if a.tSel != 10 || a.restoring != nil {
		t.Fatal("did not fall back to an available song")
	}
}

func TestPlaybackCheckpointsPauseStopAndClose(t *testing.T) {
	files, writes := memoryFiles{}, 0
	a := sessionApp(model.Config{})
	a.store = &configStore{write: func(path string, b []byte) (int, error) { writes++; return files.write(path, b) }}
	event := audioEvent{playing: true, track: model.Demo().Tracks[0], duration: 10000, position: 1000}
	a.receivedAudio(event)
	a.saveSession(true)
	event.position = 4999
	a.receivedAudio(event)
	a.saveSession(false)
	if writes != 1 {
		t.Fatal("playback progress wrote the config too frequently")
	}
	event.position = 6000
	a.receivedAudio(event)
	a.saveSession(false)
	if writes != 2 {
		t.Fatal("playback checkpoint was not saved")
	}
	event.position, event.paused = 6100, true
	a.receivedAudio(event)
	a.saveSession(false)
	if got := readSaved(t, files, temporaryConfigPath).Session; writes != 3 || got.PositionMS != 6100 {
		t.Fatal("pause did not save the current mark")
	}
	event.position, event.playing, event.paused = 6200, false, false
	a.receivedAudio(event)
	a.saveSession(false)
	if got := readSaved(t, files, temporaryConfigPath).Session; writes != 4 || got.PositionMS != 6200 {
		t.Fatal("stop did not retain the current mark")
	}
	event.position = 6300
	a.receivedAudio(event)
	a.saveSession(true)
	if got := readSaved(t, files, temporaryConfigPath).Session; writes != 5 || got.PositionMS != 6300 {
		t.Fatal("close did not save the final mark")
	}
}

func TestConcurrentCredentialAndSessionSaves(t *testing.T) {
	files := memoryFiles{}
	c := model.Config{ClientID: "app", RefreshToken: strings.Repeat("old-secret", 100), StreamingRefreshToken: "playback", StreamingDeviceID: strings.Repeat("a", 40)}
	s := &configStore{config: c, fromFAT: true, write: files.write}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		for i := 1; i <= 20; i++ {
			if err := s.saveSession(model.SessionState{PositionMS: uint32(i)}); err != nil {
				t.Error(err)
			}
		}
	}()
	go func() {
		defer wg.Done()
		for i := 1; i <= 20; i++ {
			c.RefreshToken = fmt.Sprint("new-", i)
			if err := s.saveCredentials(c); err != nil {
				t.Error(err)
			}
		}
	}()
	wg.Wait()
	loaded := readSaved(t, files, temporaryConfigPath)
	if loaded.RefreshToken != "new-20" || loaded.Session.PositionMS != 20 || loaded.StreamingRefreshToken != "playback" || bytes.Contains(files[temporaryConfigPath], []byte("old-secret")) {
		t.Fatal("concurrent saves lost state or retained old credentials")
	}
	fat := readSaved(t, files, configPath)
	if fat.Session != nil || fat.RefreshToken != "new-20" || fat.StreamingDeviceID != c.StreamingDeviceID {
		t.Fatal("FAT did not retain credentials independently of temporary state")
	}
}

func TestConfigurationWriteFailuresAndBounds(t *testing.T) {
	for _, write := range []func(string, []byte) (int, error){
		func(_ string, b []byte) (int, error) { return len(b) - 1, nil },
		func(string, []byte) (int, error) { return 0, errors.New("disk full") },
	} {
		s := &configStore{write: write}
		if err := s.saveSession(model.SessionState{}); err == nil || !strings.Contains(err.Error(), temporaryConfigPath) {
			t.Fatal("write failure was not reported")
		}
	}
	s := &configStore{config: model.Config{AccessToken: strings.Repeat("x", configFileSize)}, write: func(string, []byte) (int, error) {
		t.Fatal("oversized config was written")
		return 0, nil
	}}
	if err := s.saveSession(model.SessionState{}); err == nil {
		t.Fatal("oversized temporary config was accepted")
	}
}

func TestPCMResumeAcrossBuffers(t *testing.T) {
	skip := uint64(12)
	if got := discardPCM([]byte{1, 2, 3, 4, 5, 6, 7, 8}, &skip); len(got) != 0 || skip != 4 {
		t.Fatal("did not discard a whole earlier buffer")
	}
	if got := discardPCM([]byte{9, 10, 11, 12, 13, 14, 15, 16}, &skip); !bytes.Equal(got, []byte{13, 14, 15, 16}) || skip != 0 {
		t.Fatal("did not preserve frames after the resume point")
	}
	if got := positionBytes(12345, 44100); got != 544414*4 || got%4 != 0 {
		t.Fatal("resume point is not aligned to a stereo frame")
	}
}
