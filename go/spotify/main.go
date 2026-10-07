//go:build r2

// spotify.elf is a Go process. Memento only paints pointer-free snapshots and
// queues commands; it does not own the model, networking or audio playback.
package main

import (
	"encoding/hex"
	"fmt"
	"io"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
	"github.com/krustowski/rou2exOS-apps/go/r2tls"
	"github.com/krustowski/rou2exOS-apps/go/spotify/codec"
	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/protocol"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
)

func main() {
	args := r2.Args()
	if len(args) != 3 || args[1] != "--host" {
		fmt.Println("Spotify Go prototype: launch from Memento's Spotify icon.")
		return
	}
	host, err := memento.Attach[protocol.Snapshot](args, protocol.Magic, protocol.Version, r2.Ticks())
	if err != nil {
		fmt.Println("Spotify host:", err)
		return
	}
	startDiagnostics(host)
	a := &app{netProgress: make(chan string, 1), streamPCM: make(chan *streamChunk, 8), streamPool: make(chan *streamChunk, 10), volume: 100, pSel: 0, tSel: 0, status: startupChecks(),
		netJobs: make(chan netJob, 1), netResults: make(chan netResult, 1), audioJobs: make(chan audioJob, 8), audioEvents: make(chan audioEvent, 8), audioFinal: make(chan audioEvent, 1), done: make(chan struct{}), audioDone: make(chan struct{}), networkDone: make(chan struct{})}
	for i := 0; i < cap(a.streamPool); i++ {
		a.streamPool <- new(streamChunk)
	}
	a.loadConfig()
	a.offline = append([]model.Playlist{model.Demo()}, a.config.Playlists...)
	a.offlinePlaylists(0)
	a.openPlaylist(0)
	a.restoreSession()
	a.saveSession(true)
	go a.network()
	go a.playback(a.audio)
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	lastFrame := uint64(0)
	lastConfigSave := r2.Ticks()
	for {
		reason := host.Poll(r2.Ticks())
		if reason == memento.Running {
			reason = host.DrainCommands(a.command)
		}
		if reason != memento.Running {
			switch reason {
			case memento.ExitHostClosed:
				diagnosticStage("host closed window")
			case memento.ExitHostTimeout:
				diagnosticStage("Memento heartbeat timeout")
			case memento.ExitBadQueue:
				diagnosticStage("invalid host command queue")
				a.status = "Invalid host command queue."
			}
			break
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
				a.receivedAudio(event)
			default:
				drained = true
			}
		}
		if now := r2.Ticks(); now-lastFrame >= 100 {
			a.publish(host)
			lastFrame = now
		}
		if now := r2.Ticks(); now-lastConfigSave >= 1000 {
			a.saveSession(false)
			lastConfigSave = now
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
	select {
	case event := <-a.audioFinal:
		a.receivedAudio(event)
	default:
	}
	a.saveSession(true)
	host.Close()
}

func readConfig(path string, limit int) ([]byte, error) {
	b := make([]byte, limit+1)
	n, e := r2.ReadFileAt(path, b, 0)
	if e != nil {
		return nil, e
	}
	if n < 0 || n > limit {
		return nil, fmt.Errorf("Spotify file exceeds size limit")
	}
	return b[:n], nil
}
func (a *app) loadConfig() {
	a.store = &configStore{write: func(path string, b []byte) (int, error) {
		return r2.WriteFileAt(path, b, 0)
	}}
	if status := a.store.load(readConfig); status != "" {
		a.status = status
	}
	a.config = a.store.config
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
	playbackDevice := config.StreamingDeviceID
	var duration uint32
	var skipBytes uint64
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
			return a.store.saveCredentials(config)
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
				if transport != nil {
					transport.Close()
				}
				// Reclaim previous-track input and metadata before new startup.
				runtime.GC()
				generation = job.generation
				skipBytes = 0
				diagnosticStage("new song after cleanup")
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
						config.StreamingDeviceID = playbackDevice
						if _, err := (stream.Credentials{RefreshToken: refresh, Device: playbackDevice}).Encode(); err != nil {
							return err
						}
						return api.Save()
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
						diagnosticStage("playback login")
						err = session.Start(token)
						if err != nil {
							err = fmt.Errorf("playback login: %w", err)
						}
						playbackDevice = session.Device
						if err == nil {
							var source *stream.CDNReader
							var key []byte
							diagnosticStage("audio metadata and key")
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
									diagnosticStage("Vorbis decoder startup")
									active, err = codec.Open(decrypted)
									if err == nil {
										skipBytes = positionBytes(job.position, active.Rate)
									}
									diagnosticStage("Vorbis decoder startup returned")
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
			pending.n = len(discardPCM(pending.data[:pending.n], &skipBytes))
			requestGeneration = 0
			runtime.Gosched()
			if pending.err != nil {
				pending.end = true
				if pending.err == io.EOF {
					pending.err = nil
				}
				active.Close()
				active = nil
				diagnosticStage("stream closed after decode")
			}
			if pending.n == 0 && !pending.end {
				a.streamPool <- pending
				pending = nil
				continue
			}
			havePending = true
		}
	}
}

func (a *app) publish(host *memento.Client[protocol.Snapshot]) {
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
	host.Publish(&s)
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
	skip                uint64
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
		n, err := s.decoder.Read(buf)
		return len(discardPCM(buf[:n], &s.skip)), err
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
func (a *app) playback(initial audioEvent) {
	defer close(a.audioDone)
	var source pcmSource
	defer func() {
		if source.decoder != nil {
			source.decoder.Close()
		}
	}()
	streamMode, streamEnd, streamOpened := false, false, false
	var generation uint32
	event := initial
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
	updatePosition := func() {
		if source.rate == 0 || !event.playing {
			return
		}
		played := source.sent
		queued := r2.AudioQueued()
		if queued <= played {
			played -= queued
		} else {
			played = 0
		}
		event.position = uint32(played * 1000 / (uint64(source.rate) * 4))
	}
	for {
		var input <-chan *streamChunk = a.streamPCM
		if streamMode && (len(pending) > 0 || event.paused) {
			input = nil
		}
		select {
		case <-a.done:
			updatePosition()
			a.audioFinal <- event
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
				updatePosition()
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
					source.sent = positionBytes(event.position, rate)
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
				updatePosition()
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
				event.message = "Stopped."
				report()
			case protocol.PlayPause:
				if event.playing {
					updatePosition()
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
				event = audioEvent{track: job.track, position: job.position}
				pending = buf[:0]
				if job.track.File == "" {
					source = pcmSource{}
					event.duration = job.track.DurationMS
					event.playing = true
					streamMode = true
					event.message = "Connecting Spotify playback session..."
					select {
					case a.netJobs <- netJob{kind: streamJob, track: job.track, generation: generation, position: job.position}:
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
					s.sent = positionBytes(job.position, s.rate)
					if s.sent >= s.bytes {
						s.sent = 0
						event.position = 0
					}
					if s.decoder != nil {
						s.skip = s.sent
					}
				}
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
				if n == 0 && err == nil && source.decoder != nil {
					continue
				}
				if err != nil && err != io.EOF || n%4 != 0 || n == 0 {
					updatePosition()
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
					updatePosition()
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
