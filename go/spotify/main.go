//go:build r2

// spotify.elf is a Go process. Memento only paints pointer-free snapshots and
// queues commands; it does not own the model, networking or audio playback.
package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
	"unsafe"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
	"github.com/krustowski/rou2exOS-apps/go/r2tls"
	"github.com/krustowski/rou2exOS-apps/go/spotify/codec"
	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
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

const configPath = "/mnt/fat/SPOTIFY.CFG"
const playbackKeyPath = "/mnt/tmp/SPOTIFY.KEY"

type netJob struct {
	track      model.Track
	generation uint32
	kind       uint32
	id         string
	offset     int
}
type netResult struct {
	job       netJob
	playlists []model.Playlist
	tracks    []model.Track
	total     int
	err       error
}
type audioJob struct {
	op    uint32
	track model.Track
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
	configFromFAT                                bool
	status                                       string
	audio                                        audioEvent
	netJobs                                      chan netJob
	netResults                                   chan netResult
	audioJobs                                    chan audioJob
	audioEvents                                  chan audioEvent
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

func main() {
	args := r2.Args()
	if len(args) != 3 || args[1] != "--host" {
		fmt.Println("Spotify Go prototype: launch from Memento's Spotify icon.")
		return
	}
	address, err := strconv.ParseUint(args[2], 0, 64)
	size := uint64(unsafe.Sizeof(protocol.Block{}))
	if err != nil || address%4 != 0 || address < 0xc00000 || address >= 0x1000000 || size > 0x1000000-address {
		fmt.Println("Invalid Spotify host block.")
		return
	}
	block := (*protocol.Block)(unsafe.Pointer(uintptr(address)))
	if block.Magic != protocol.Magic || block.Version != protocol.Version {
		fmt.Println("Unsupported Spotify host protocol.")
		return
	}
	a := &app{netProgress: make(chan string, 1), streamPCM: make(chan *streamChunk, 8), streamPool: make(chan *streamChunk, 10), volume: 100, pSel: 0, tSel: 0, status: startupChecks(),
		netJobs: make(chan netJob, 1), netResults: make(chan netResult, 1), audioJobs: make(chan audioJob, 8), audioEvents: make(chan audioEvent, 8), done: make(chan struct{}), audioDone: make(chan struct{}), networkDone: make(chan struct{})}
	for i := 0; i < cap(a.streamPool); i++ {
		a.streamPool <- new(streamChunk)
	}
	a.loadConfig()
	a.offline = append([]model.Playlist{model.Demo()}, a.config.Playlists...)
	a.offlinePlaylists(0)
	a.openPlaylist(0)
	go a.network()
	go a.playback()
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	hostBeat := atomic.LoadUint32(&block.HostBeat)
	hostSeen := r2.Ticks()
	lastFrame := uint64(0)
	for {
		if atomic.LoadUint32(&block.Quit) != 0 {
			break
		}
		beat := atomic.LoadUint32(&block.HostBeat)
		if beat != hostBeat {
			hostBeat = beat
			hostSeen = r2.Ticks()
		} else if r2.Ticks()-hostSeen > 10000 {
			break
		}
		atomic.AddUint32(&block.ClientBeat, 1)
		head, tail := atomic.LoadUint32(&block.Head), atomic.LoadUint32(&block.Tail)
		if head-tail > protocol.QueueSize {
			a.status = "Invalid host command queue."
			break
		}
		for tail != head {
			cmd := block.Commands[tail%protocol.QueueSize]
			tail++
			atomic.StoreUint32(&block.Tail, tail)
			a.command(cmd)
		}
		select {
		case progress := <-a.netProgress:
			a.status = progress
		case result := <-a.netResults:
			a.received(result)
		default:
		}
		for drained := false; !drained; {
			select {
			case event := <-a.audioEvents:
				a.audio = event
				if event.message != "" {
					a.status = event.message
				}
			default:
				drained = true
			}
		}
		if now := r2.Ticks(); now-lastFrame >= 100 {
			a.publish(block)
			lastFrame = now
		}
		<-ticker.C
	}
	close(a.done)
	// Audio worker never touches shared memory. Returning ends all goroutines;
	// close the process's stream before acknowledging that the host can free it.
	select {
	case <-a.audioDone:
	case <-time.After(time.Second):
		r2.AudioClose()
	}
	select {
	case <-a.networkDone:
	case <-time.After(2 * time.Second):
	}
	atomic.StoreUint32(&block.Exited, 1)
}

func readConfig(path string) ([]byte, error) {
	b := make([]byte, 64*1024+1)
	n, e := r2.ReadFileAt(path, b, 0)
	if e != nil {
		return nil, e
	}
	if n > 64*1024 {
		return nil, fmt.Errorf("configuration exceeds 64 KiB")
	}
	return b[:n], nil
}
func (a *app) loadConfig() {
	for _, path := range []string{configPath, "/mnt/tar/opt/spotify/config.josn", "/mnt/tar/opt/spotify/config.json", "/mnt/iso/opt/spotify/config.josn", "/mnt/iso/opt/spotify/config.json"} {
		b, err := readConfig(path)
		if err != nil {
			continue
		}
		c, err := model.ParseConfig(b)
		if err != nil {
			a.status = err.Error()
			return
		}
		a.config = c
		a.configFromFAT = path == configPath
		return
	}
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
		}
	case protocol.PlayPause:
		t, ok := a.selectedTrack()
		if !ok {
			return
		}
		if a.audio.playing && a.audio.track.ID == t.ID && a.audio.track.File == t.File {
			a.sendAudio(audioJob{op: protocol.PlayPause})
			return
		}
		a.sendAudio(audioJob{op: protocol.SelectTrack, track: t})
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
		a.request(netJob{kind: protocol.Refresh})
	case protocol.PlaylistPage:
		if a.busy {
			return
		}
		offset := a.pOffset + int(int32(c.Value))*protocol.Visible
		if offset < 0 || offset >= a.pTotal {
			return
		}
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
}
func (a *app) network() {
	defer close(a.networkDone)
	var stack *r2net.Stack
	var api *model.API
	var transport *r2tls.Client
	config := a.config
	tokens := stream.PlaybackTokens{AccessToken: config.StreamingAccessToken, RefreshToken: config.StreamingRefreshToken,
		Progress: func(s string) {
			select {
			case a.netProgress <- s:
			default:
			}
		}}
	var active *codec.Reader
	var generation uint32
	var playbackDevice string
	var duration uint32
	var requestGeneration uint32
	check := func() error {
		runtime.Gosched()
		select {
		case <-a.done:
			return fmt.Errorf("Spotify request cancelled")
		default:
		}
		if requestGeneration != 0 && requestGeneration != atomic.LoadUint32(&a.streamGeneration) {
			return fmt.Errorf("Spotify stream cancelled")
		}
		return nil
	}
	keyData := make([]byte, stream.CredentialFileSize)
	if n, e := r2.ReadFileAt(playbackKeyPath, keyData, 0); e == nil && n >= 0 && n <= len(keyData) {
		if key, e := stream.DecodeCredentials(keyData[:n]); e == nil {
			tokens.AccessToken = ""
			tokens.RefreshToken = key.RefreshToken
			playbackDevice = key.Device
		}
	}
	ready := make(chan struct{})
	close(ready)
	var pending *streamChunk
	havePending := false
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	defer func() {
		if active != nil {
			active.Close()
		}
		if transport != nil {
			transport.Close()
		}
		if stack != nil {
			stack.Close()
		}
	}()
	initialize := func() error {
		if stack != nil {
			return nil
		}
		var err error
		stack, err = r2net.Open(r2net.Options{Check: check})
		if err != nil {
			return err
		}
		transport = &r2tls.Client{Stack: stack, Check: check}
		api = &model.API{Config: &config, Do: transport.Do, Save: func() error {
			b, err := json.Marshal(config)
			if err != nil {
				return err
			}
			old, readErr := readConfig(configPath)
			if readErr == nil && len(old) > len(b) {
				start := len(b)
				b = append(b, make([]byte, len(old)-len(b))...)
				for i := start; i < len(b); i++ {
					b[i] = ' '
				}
			}
			n, err := r2.WriteFileAt(configPath, b, 0)
			if err != nil || n != len(b) {
				return fmt.Errorf("cannot persist rotated Spotify token to SPOTIFY.CFG")
			}
			return nil
		}}
		return nil
	}
	for {
		if generation != atomic.LoadUint32(&a.streamGeneration) {
			requestGeneration = 0
			if transport != nil {
				transport.Close()
			}
			if active != nil {
				active.Close()
				active = nil
			}
			if pending != nil {
				a.streamPool <- pending
				pending = nil
			}
			havePending = false
		}
		var decode <-chan struct{}
		if active != nil && !havePending {
			decode = ready
		}
		var output chan *streamChunk
		if havePending {
			output = a.streamPCM
		}
		select {
		case <-a.done:
			return
		case output <- pending:
			pending = nil
			havePending = false
		case job := <-a.netJobs:
			requestGeneration = 0
			err := initialize()
			if job.kind == streamJob {
				if active != nil {
					active.Close()
					active = nil
				}
				if pending != nil {
					a.streamPool <- pending
					pending = nil
				}
				havePending = false
				generation = job.generation
				duration = job.track.DurationMS
				requestGeneration = generation
				if generation != atomic.LoadUint32(&a.streamGeneration) {
					continue
				}
				cancelled := func() error {
					select {
					case <-a.done:
						return fmt.Errorf("stream cancelled")
					default:
					}
					if generation != atomic.LoadUint32(&a.streamGeneration) {
						return fmt.Errorf("stream cancelled")
					}
					runtime.Gosched()
					return nil
				}
				if err == nil {
					var token string
					if playbackDevice == "" {
						var id [20]byte
						err = r2tls.Random(id[:])
						playbackDevice = hex.EncodeToString(id[:])
					}
					tokens.Save = func(access, refresh string) error {
						config.StreamingAccessToken = ""
						config.StreamingRefreshToken = refresh
						record, e := (stream.Credentials{RefreshToken: refresh, Device: playbackDevice}).Encode()
						if e == nil {
							var n int
							n, e = r2.WriteFileAt(playbackKeyPath, record, 0)
							if n != len(record) && e == nil {
								e = fmt.Errorf("short playback key write")
							}
						}
						if e == nil && !a.configFromFAT {
							return nil
						}
						fatErr := api.Save()
						if e == nil || fatErr == nil {
							return nil
						}
						return fmt.Errorf("cannot save playback key to /mnt/tmp or FAT")
					}
					token, err = tokens.Token(transport.Do, func(duration time.Duration) error {
						for duration > 0 {
							if e := cancelled(); e != nil {
								return e
							}
							part := 200 * time.Millisecond
							if duration < part {
								part = duration
							}
							time.Sleep(part)
							duration -= part
						}
						return nil
					})
					if err == nil {
						session := stream.Session{Progress: tokens.Progress, Device: playbackDevice, HTTP: transport.Do, Random: r2tls.Random, Yield: cancelled, Dial: func(host string, port uint16) (stream.Socket, error) {
							ip, e := stack.ResolveTCP(host, 15*time.Second)
							if e != nil {
								return nil, e
							}
							conn, e := stack.Dial(ip, port, 15*time.Second)
							if e == nil {
								conn.SetDeadline(30 * time.Second)
							}
							return conn, e
						}}
						err = session.Start(token)
						if err != nil {
							err = fmt.Errorf("playback login: %w", err)
						}
						playbackDevice = session.Device
						if err == nil {
							var source *stream.CDNReader
							var key []byte
							source, key, err = session.Audio(job.track.ID)
							if err == nil {
								source.Do = transport.DoAudio
								if duration == 0 {
									duration = source.DurationMS
								}
							}
							if err == nil {
								var decrypted *stream.Decryptor
								decrypted, err = stream.NewDecryptor(source, key)
								if err == nil {
									if tokens.Progress != nil {
										tokens.Progress("Downloading Spotify Vorbis headers...")
									}
									active, err = codec.Open(decrypted)
									if err != nil {
										err = fmt.Errorf("audio headers: %w", err)
									}
								}
							}
						}
						session.Close()
					}
				}
				if err != nil {
					pending = <-a.streamPool
					pending.n, pending.rate, pending.generation, pending.end, pending.err = 0, 0, generation, true, err
					pending.duration = duration
					havePending = true
				}
				continue
			}
			requestGeneration = 0
			result := netResult{job: job, err: err}
			if err == nil {
				if job.kind == protocol.Refresh || job.kind == protocol.PlaylistPage {
					result.playlists, result.total, result.err = api.Playlists(job.offset)
				} else {
					result.tracks, result.total, result.err = api.Tracks(job.id, job.offset)
				}
			}
			select {
			case a.netResults <- result:
			case <-a.done:
				return
			}
		case <-ticker.C:
			continue
		case <-decode:
			pending = <-a.streamPool
			pending.n, pending.rate, pending.generation, pending.end, pending.err = 0, active.Rate, generation, false, nil
			pending.duration = duration
			requestGeneration = generation
			pending.n, pending.err = active.Read(pending.data[:])
			requestGeneration = 0
			runtime.Gosched()
			if pending.err != nil {
				pending.end = true
				if pending.err == io.EOF {
					pending.err = nil
				}
				active.Close()
				active = nil
			}
			havePending = true
		}
	}
}

func (a *app) publish(b *protocol.Block) {
	front := atomic.LoadUint32(&b.Front)
	if front > 1 {
		front = 0
	}
	back := 1 - front
	if atomic.LoadUint32(&b.Reading) == back {
		return
	}
	s := protocol.Snapshot{Volume: atomic.LoadUint32(&a.volume), Muted: atomic.LoadUint32(&a.muted), PlaylistCount: uint32(a.pTotal), TrackCount: uint32(a.tTotal), PlaylistSelected: uint32(a.pSel), TrackSelected: uint32(a.tSel), PlaylistOffset: uint32(a.pOffset), TrackOffset: uint32(a.tOffset), PositionMS: a.audio.position, DurationMS: a.audio.duration}
	if a.audio.playing {
		s.Playing = 1
	}
	if a.audio.paused {
		s.Paused = 1
	}
	runtime.ReadMemStats(&a.memory)
	s.HeapBytes = uint32(a.memory.HeapAlloc)
	stats := r2.ReadStackStats()
	used, _ := r2.StackUsed()
	if used > stats.Peak {
		stats.Peak = used
	}
	s.StackBytes = uint32(stats.Peak)
	// TinyGo NumGoroutine is a constant stub; r2 stack counters count live tasks.
	s.Goroutines = uint32(stats.Allocated + stats.Reused - stats.Exited)
	protocol.Text(s.Status[:], a.status)
	protocol.Text(s.NowPlaying[:], a.audio.track.Name)
	for i := range s.Playlists {
		s.Playlists[i].Index = ^uint32(0)
		s.Tracks[i].Index = ^uint32(0)
	}
	for i, p := range a.playlists {
		if i == protocol.Visible {
			break
		}
		s.Playlists[i].Index = uint32(a.pOffset + i)
		protocol.Text(s.Playlists[i].Text[:], p.Name)
	}
	for i, t := range a.tracks {
		if i == protocol.Visible {
			break
		}
		s.Tracks[i].Index = uint32(a.tOffset + i)
		protocol.Text(s.Tracks[i].Text[:], t.Name+" - "+t.Artist)
	}
	b.Snapshots[back] = s
	atomic.StoreUint32(&b.Front, back)
	atomic.AddUint32(&b.Frame, 1)
}

type fileReader string

func (f fileReader) ReadAt(b []byte, offset int64) (int, error) {
	if offset < 0 {
		return 0, fmt.Errorf("negative audio offset")
	}
	n, err := r2.ReadFileAt(string(f), b, uint64(offset))
	if err != nil {
		return n, err
	}
	if n < len(b) {
		return n, io.EOF
	}
	return n, nil
}

type pcmSource struct {
	decoder             *codec.Reader
	track               model.Track
	rate                uint32
	offset, bytes, sent uint64
	tone                int
}

func openSource(t model.Track) (pcmSource, error) {
	s := pcmSource{track: t}
	if strings.HasPrefix(t.File, "tone:") {
		freq, err := strconv.Atoi(strings.TrimPrefix(t.File, "tone:"))
		if err != nil || freq < 20 || freq > 20000 {
			return s, fmt.Errorf("invalid test tone")
		}
		s.rate = 48000
		s.tone = freq
		s.bytes = uint64(s.rate) * 4 * 10
		return s, nil
	}
	if !strings.HasPrefix(t.File, "/mnt/") {
		return s, fmt.Errorf("local WAV path must start with /mnt/")
	}
	if strings.HasSuffix(strings.ToLower(t.File), ".ogg") {
		decoder, err := codec.Open(fileReader(t.File))
		if err != nil {
			return s, err
		}
		s.decoder = decoder
		s.rate = decoder.Rate
		s.bytes = ^uint64(0)
		return s, nil
	}
	w, err := model.ParseWAV(fileReader(t.File))
	if err != nil {
		return s, err
	}
	s.rate = w.Rate
	s.offset = w.Offset
	s.bytes = w.Bytes
	return s, nil
}
func (s *pcmSource) read(buf []byte) (int, error) {
	if s.decoder != nil {
		return s.decoder.Read(buf)
	}
	want := len(buf)
	if uint64(want) > s.bytes-s.sent {
		want = int(s.bytes - s.sent)
	}
	if want == 0 {
		return 0, io.EOF
	}
	if s.tone == 0 {
		return fileReader(s.track.File).ReadAt(buf[:want], int64(s.offset+s.sent))
	}
	// Integer triangle wave: avoids relying on saved floating-point state.
	for i := 0; i < want; i += 4 {
		phase := int(((s.sent/4 + uint64(i/4)) * uint64(s.tone) * 65536 / uint64(s.rate)) & 65535)
		value := phase
		if phase > 32767 {
			value = 65535 - phase
		}
		sample := int16((value - 16384) / 4)
		buf[i] = byte(sample)
		buf[i+1] = byte(uint16(sample) >> 8)
		buf[i+2] = buf[i]
		buf[i+3] = buf[i+1]
	}
	return want, nil
}
func (a *app) playback() {
	defer close(a.audioDone)
	var source pcmSource
	defer func() {
		if source.decoder != nil {
			source.decoder.Close()
		}
	}()
	streamMode, streamEnd, streamOpened := false, false, false
	var generation uint32
	var event audioEvent
	var buf [8192]byte
	pending := buf[:0]
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	defer r2.AudioClose()
	lastReport := uint64(0)
	report := func() {
		select {
		case a.audioEvents <- event:
		default:
		}
	}
	for {
		var input <-chan *streamChunk = a.streamPCM
		if streamMode && (len(pending) > 0 || event.paused) {
			input = nil
		}
		select {
		case <-a.done:
			return
		case chunk := <-input:
			valid := streamMode && chunk.generation == generation
			n, rate, end, chunkErr, chunkDuration := chunk.n, chunk.rate, chunk.end, chunk.err, chunk.duration
			if valid && n > 0 {
				copy(buf[:], chunk.data[:n])
			}
			a.streamPool <- chunk
			if !valid {
				continue
			}
			if chunkErr != nil {
				r2.AudioClose()
				event.playing = false
				streamMode = false
				event.message = "Spotify streaming: " + chunkErr.Error()
				report()
				continue
			}
			if event.duration == 0 {
				event.duration = chunkDuration
			}
			streamEnd = end
			if streamEnd && n == 0 && !streamOpened {
				event.playing = false
				streamMode = false
				event.message = "Spotify stream contained no audio frames."
				report()
				continue
			}
			if n > 0 {
				if event.message == "Buffering Spotify audio..." {
					event.message = "Playing Spotify audio inside rou2exOS."
				}
				if !streamOpened {
					if err := r2.AudioOpen(rate); err != nil {
						atomic.AddUint32(&a.streamGeneration, 1)
						event.playing = false
						streamMode = false
						event.message = err.Error()
						report()
						continue
					}
					streamOpened = true
					source.rate = rate
					event.message = "Playing Spotify audio inside rou2exOS."
					report()
				}
				volume := atomic.LoadUint32(&a.volume)
				if atomic.LoadUint32(&a.muted) != 0 {
					volume = 0
				}
				model.ApplyVolume(buf[:n], volume)
				pending = buf[:n]
			}
		case job := <-a.audioJobs:
			switch job.op {
			case protocol.Stop:
				atomic.AddUint32(&a.streamGeneration, 1)
				streamMode = false
				if source.decoder != nil {
					source.decoder.Close()
					source.decoder = nil
				}
				r2.AudioClose()
				event.playing = false
				event.paused = false
				pending = buf[:0]
				event.position = 0
				event.message = "Stopped."
				report()
			case protocol.PlayPause:
				if event.playing {
					event.paused = !event.paused
					if event.paused {
						r2.AudioPause()
						event.message = "Audio paused."
					} else {
						r2.AudioResume()
						event.message = "Playing audio inside rou2exOS."
					}
					report()
				}
			case protocol.SelectTrack:
				generation = atomic.AddUint32(&a.streamGeneration, 1)
				streamMode, streamEnd, streamOpened = false, false, false
				if source.decoder != nil {
					source.decoder.Close()
					source.decoder = nil
				}
				r2.AudioClose()
				event = audioEvent{track: job.track}
				pending = buf[:0]
				if job.track.File == "" {
					source = pcmSource{}
					event.duration = job.track.DurationMS
					event.playing = true
					streamMode = true
					event.message = "Connecting Spotify playback session..."
					select {
					case a.netJobs <- netJob{kind: streamJob, track: job.track, generation: generation}:
					default:
						event.playing = false
						streamMode = false
						event.message = "Network command queue is busy; retry Play."
					}
					report()
					continue
				}
				s, err := openSource(job.track)
				if err == nil {
					err = r2.AudioOpen(s.rate)
				}
				if err != nil {
					if s.decoder != nil {
						s.decoder.Close()
					}
					event.message = err.Error()
					report()
					continue
				}
				source = s
				event.playing = true
				if s.decoder != nil {
					event.duration = job.track.DurationMS
				} else {
					event.duration = uint32(s.bytes * 1000 / (uint64(s.rate) * 4))
				}
				event.message = "Playing local PCM inside rou2exOS."
				report()
			}
		case <-ticker.C:
			if !event.playing || event.paused {
				continue
			}
			if !streamMode && len(pending) == 0 && source.sent < source.bytes && r2.AudioQueued() < uint64(source.rate)*4/4 {
				n, err := source.read(buf[:])
				if err == io.EOF && n == 0 && source.decoder != nil {
					source.bytes = source.sent
					source.decoder.Close()
					source.decoder = nil
					continue
				}
				if err != nil && err != io.EOF || n%4 != 0 || n == 0 {
					r2.AudioClose()
					event.playing = false
					event.message = "Local audio file is truncated or unreadable."
					report()
					continue
				}
				volume := atomic.LoadUint32(&a.volume)
				if atomic.LoadUint32(&a.muted) != 0 {
					volume = 0
				}
				model.ApplyVolume(buf[:n], volume)
				pending = buf[:n]
			}
			if len(pending) > 0 {
				n, err := r2.AudioWrite(pending)
				if err != nil {
					atomic.AddUint32(&a.streamGeneration, 1)
					streamMode = false
					r2.AudioClose()
					event.playing = false
					event.message = err.Error()
					report()
					continue
				}
				source.sent += uint64(n)
				pending = pending[n:]
			}
			if source.rate == 0 {
				continue
			}
			queued := r2.AudioQueued()
			if streamMode && !streamEnd && len(pending) == 0 && queued == 0 {
				event.message = "Buffering Spotify audio..."
			}
			played := source.sent
			if queued <= played {
				played -= queued
			} else {
				played = 0
			}
			event.position = uint32(played * 1000 / (uint64(source.rate) * 4))
			if ((!streamMode && source.sent == source.bytes) || (streamMode && streamEnd)) && len(pending) == 0 && queued == 0 {
				r2.AudioClose()
				event.playing = false
				if streamMode {
					event.message = "Spotify audio finished."
					streamMode = false
				} else {
					event.message = "Local audio finished."
				}
				if source.decoder != nil {
					source.decoder.Close()
					source.decoder = nil
				}
				if event.duration == 0 {
					event.duration = event.position
				}
				report()
			}
			if now := r2.Ticks(); now-lastReport >= 200 {
				report()
				lastReport = now
			}
		}
	}
}
