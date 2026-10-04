//go:build !r2

// streamcheck is a developer diagnostic, not a playback backend. It tests the
// same protocol/decoder with host sockets; r2 runs them natively in spotify.elf.
package main

import (
	"bytes"
	"crypto/rand"
	"encoding/json"
	"flag"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"github.com/krustowski/rou2exOS-apps/go/spotify/codec"
	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "Stream diagnostic:", err)
		os.Exit(1)
	}
}
func run() error {
	configPath := flag.String("config", "SPOTIFY.CFG", "private account config (tokens are never printed)")
	trackID := flag.String("track", "", "optional Spotify track ID; otherwise use first nonempty playlist")
	seconds := flag.Int("seconds", 2, "PCM duration to verify (1-60 seconds; never plays on host)")
	flag.Parse()
	if *seconds < 1 || *seconds > 60 {
		return fmt.Errorf("seconds must be between 1 and 60")
	}
	data, e := os.ReadFile(*configPath)
	if e != nil {
		return e
	}
	config, e := model.ParseConfig(data)
	if e != nil {
		return e
	}
	if config.AccessToken == "" && (config.ClientID == "" || config.RefreshToken == "") {
		return fmt.Errorf("config has no account credentials")
	}
	client := &http.Client{Timeout: 45 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	transport := func(method, url string, h map[string]string, body []byte) (wire.Response, error) {
		request, e := http.NewRequest(method, url, bytes.NewReader(body))
		if e != nil {
			return wire.Response{}, fmt.Errorf("invalid request")
		}
		for k, v := range h {
			request.Header.Set(k, v)
		}
		request.Header.Set("Accept-Encoding", "identity")
		response, e := client.Do(request)
		if e != nil {
			return wire.Response{}, fmt.Errorf("HTTPS request failed")
		}
		defer response.Body.Close()
		b, e := io.ReadAll(io.LimitReader(response.Body, wire.MaxBody+1))
		if e != nil {
			return wire.Response{}, e
		}
		if len(b) > wire.MaxBody {
			return wire.Response{}, fmt.Errorf("response exceeds limit")
		}
		headers := make(map[string]string)
		for k, values := range response.Header {
			if len(values) > 0 {
				headers[strings.ToLower(k)] = values[0]
			}
		}
		return wire.Response{Status: response.StatusCode, Header: headers, Body: b}, nil
	}
	api := &model.API{Config: &config, Do: transport, Save: func() error {
		// Preserve unknown configuration fields while persisting token rotation.
		var document map[string]interface{}
		if e := json.Unmarshal(data, &document); e != nil {
			return e
		}
		document["refresh_token"] = config.RefreshToken
		document["streaming_refresh_token"] = config.StreamingRefreshToken
		b, e := json.MarshalIndent(document, "", "  ")
		if e != nil {
			return e
		}
		tmp, e := os.CreateTemp(filepath.Dir(*configPath), ".spotify-token-*")
		if e != nil {
			return e
		}
		name := tmp.Name()
		defer os.Remove(name)
		if _, e = tmp.Write(b); e == nil {
			e = tmp.Sync()
		}
		closeErr := tmp.Close()
		if e != nil {
			return e
		}
		if closeErr != nil {
			return closeErr
		}
		if e := os.Rename(name, *configPath); e != nil {
			return e
		}
		data = b
		return nil
	}}
	playback := stream.PlaybackTokens{AccessToken: config.StreamingAccessToken, RefreshToken: config.StreamingRefreshToken,
		Progress: func(s string) { fmt.Println(s) },
		Save:     func(access, refresh string) error { config.StreamingRefreshToken = refresh; return api.Save() },
	}
	token, e := playback.Token(transport, func(d time.Duration) error { time.Sleep(d); return nil })
	if e != nil {
		return e
	}
	session := stream.Session{HTTP: transport, Random: func(p []byte) error { _, e := rand.Read(p); return e }, Dial: func(host string, port uint16) (stream.Socket, error) {
		conn, e := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 20*time.Second)
		if e == nil {
			conn.SetDeadline(time.Now().Add(45 * time.Second))
		}
		return conn, e
	}}
	defer session.Close()
	if e = session.Start(token); e != nil {
		return e
	}
	fmt.Println("AP and playback login succeeded")
	id := *trackID
	if id == "" {
		playlists, _, e := api.Playlists(0)
		if e != nil {
			return e
		}
		for _, p := range playlists {
			tracks, _, e := api.Tracks(p.ID, 0)
			if e != nil {
				continue
			}
			for _, t := range tracks {
				if t.ID != "" {
					id = t.ID
					break
				}
			}
			if id != "" {
				break
			}
		}
	}
	if id == "" {
		return fmt.Errorf("no track available for test")
	}
	source, key, e := session.Audio(id)
	if e != nil {
		return e
	}
	decrypted, e := stream.NewDecryptor(source, key)
	if e != nil {
		return e
	}
	reader, e := codec.Open(decrypted)
	if e != nil {
		return e
	}
	defer reader.Close()
	var pcm [8192]byte
	frames, peak := 0, 0
	for frames < int(reader.Rate)*(*seconds) {
		n, e := reader.Read(pcm[:])
		for i := 0; i < n; i += 2 {
			v := int(int16(uint16(pcm[i]) | uint16(pcm[i+1])<<8))
			if v < 0 {
				v = -v
			}
			if v > peak {
				peak = v
			}
		}
		frames += n / 4
		if e == io.EOF {
			break
		}
		if e != nil {
			return e
		}
	}
	fmt.Printf("Live native engine decoded %d stereo PCM frames at %d Hz; peak %d (no host playback)\n", frames, reader.Rate, peak)
	return nil
}
