package model

import (
	"bytes"
	"encoding/binary"
	"errors"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"testing"
)

func TestTokenRefreshAndNewPlaylistItems(t *testing.T) {
	c := Config{ClientID: "client", RefreshToken: "old token", LocalFiles: map[string]string{"song": "/mnt/tar/music/song.wav"}}
	saved, calls := false, 0
	api := API{Config: &c, Save: func() error { saved = true; return nil }}
	api.Do = func(method, url string, h map[string]string, body []byte) (wire.Response, error) {
		calls++
		if calls == 1 {
			if method != "POST" || url != "https://accounts.spotify.com/api/token" || !bytes.Contains(body, []byte("old%20token")) {
				t.Fatalf("bad refresh request: %s %s %s", method, url, body)
			}
			return wire.Response{Status: 200, Body: []byte(`{"access_token":"fresh","refresh_token":"rotated"}`)}, nil
		}
		if h["Authorization"] != "Bearer fresh" || url != "https://api.spotify.com/v1/playlists/1234567890123456789012/items?limit=10&offset=10" {
			t.Fatal("incorrect authenticated paging request")
		}
		return wire.Response{Status: 200, Body: []byte(`{"total":12,"items":[{"item":{"id":"song","type":"track","name":"Song","duration_ms":1000,"artists":[{"name":"One"},{"name":"Two"}]}},{"item":null}]}`)}, nil
	}
	tracks, total, err := api.Tracks("1234567890123456789012", 10)
	if err != nil || total != 12 || len(tracks) != 2 || tracks[0].File != "/mnt/tar/music/song.wav" || tracks[0].Artist != "One, Two" || tracks[1].Name != "Unavailable track" || !saved || c.RefreshToken != "rotated" {
		t.Fatalf("bad result %#v total %d error %v", tracks, total, err)
	}
}

func TestUnauthorizedRetriesOnlyOnce(t *testing.T) {
	c := Config{ClientID: "client", RefreshToken: "refresh", AccessToken: "expired"}
	calls := 0
	a := API{Config: &c, Do: func(method, url string, h map[string]string, body []byte) (wire.Response, error) {
		calls++
		if method == "POST" {
			return wire.Response{Status: 200, Body: []byte(`{"access_token":"new"}`)}, nil
		}
		return wire.Response{Status: 401}, nil
	}}
	_, _, err := a.Playlists(0)
	if err == nil || calls != 3 {
		t.Fatalf("expected bounded retry, calls=%d err=%v", calls, err)
	}
}
func TestRefreshSaveFailureIsReported(t *testing.T) {
	c := Config{ClientID: "c", RefreshToken: "r"}
	expected := errors.New("disk full")
	a := API{Config: &c, Do: func(string, string, map[string]string, []byte) (wire.Response, error) {
		return wire.Response{Status: 200, Body: []byte(`{"access_token":"a","refresh_token":"rotated"}`)}, nil
	}, Save: func() error { return expected }}
	_, _, err := a.Playlists(0)
	if !errors.Is(err, expected) {
		t.Fatal(err)
	}
}

func wav(channels, bits uint16, rate uint32) []byte {
	b := make([]byte, 52)
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], 44)
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], channels)
	binary.LittleEndian.PutUint32(b[24:], rate)
	binary.LittleEndian.PutUint32(b[28:], rate*4)
	binary.LittleEndian.PutUint16(b[32:], 4)
	binary.LittleEndian.PutUint16(b[34:], bits)
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], 8)
	return b
}
func TestWAVValidation(t *testing.T) {
	w, err := ParseWAV(bytes.NewReader(wav(2, 16, 48000)))
	if err != nil || w.Rate != 48000 || w.Offset != 44 || w.Bytes != 8 {
		t.Fatalf("%+v %v", w, err)
	}
	for _, b := range [][]byte{wav(1, 16, 48000), wav(2, 24, 48000), wav(2, 16, 12345), []byte("RIFF")} {
		if _, err := ParseWAV(bytes.NewReader(b)); err == nil {
			t.Fatal("invalid WAV accepted")
		}
	}
	b := wav(2, 16, 48000)
	binary.LittleEndian.PutUint32(b[40:], 1000)
	if _, err := ParseWAV(bytes.NewReader(b)); err == nil {
		t.Fatal("RIFF overflow accepted")
	}
}
func TestWAVSkipsOddChunks(t *testing.T) {
	original := wav(2, 16, 44100)
	b := append([]byte{}, original[:36]...)
	b = append(b, []byte("JUNK\x01\x00\x00\x00x\x00")...)
	b = append(b, original[36:]...)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	w, err := ParseWAV(bytes.NewReader(b))
	if err != nil || w.Offset != 54 {
		t.Fatalf("odd chunk handling: %+v %v", w, err)
	}
}
func TestConfigBoundsAndDemo(t *testing.T) {
	if _, err := ParseConfig(make([]byte, 65537)); err == nil {
		t.Fatal("oversized config accepted")
	}
	c, err := ParseConfig([]byte(`{"playlists":[{"name":"Local","tracks":[{"name":"Song","file":"/mnt/tar/music/a.wav"}]}]}`))
	if err != nil || len(c.Playlists) != 1 {
		t.Fatal(err)
	}
	if p := Demo(); len(p.Tracks) != 2 || p.Tracks[0].File != "tone:440" {
		t.Fatal(p)
	}
}
