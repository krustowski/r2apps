package model

import (
	"encoding/json"
	"errors"
)

type Track struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Artist     string `json:"artist"`
	DurationMS uint32 `json:"duration_ms"`
	File       string `json:"file,omitempty"`
}

type Playlist struct {
	ID     string  `json:"id"`
	Name   string  `json:"name"`
	Tracks []Track `json:"tracks,omitempty"`
}

type Config struct {
	StreamingAccessToken  string `json:"streaming_access_token,omitempty"`
	StreamingRefreshToken string `json:"streaming_refresh_token,omitempty"`
	ClientID              string `json:"client_id"`
	RefreshToken          string `json:"refresh_token"`
	AccessToken           string `json:"access_token,omitempty"`
	// Optional local files for exercising PCM independently of Spotify streaming.
	LocalFiles map[string]string `json:"local_files,omitempty"`
	Playlists  []Playlist        `json:"playlists,omitempty"`
}

func ParseConfig(b []byte) (Config, error) {
	var c Config
	if len(b) > 64*1024 {
		return c, errors.New("configuration exceeds 64 KiB")
	}
	if err := json.Unmarshal(b, &c); err != nil {
		return c, errors.New("invalid Spotify configuration JSON")
	}
	if len(c.Playlists) > 200 {
		return c, errors.New("too many offline playlists (maximum 200)")
	}
	for _, p := range c.Playlists {
		if len(p.Tracks) > 500 {
			return c, errors.New("too many offline tracks (maximum 500 per playlist)")
		}
	}
	return c, nil
}

func Demo() Playlist {
	return Playlist{ID: "runtime-demo", Name: "Go runtime audio test", Tracks: []Track{
		{ID: "tone440", Name: "440 Hz stereo tone", Artist: "Generated inside rou2exOS", DurationMS: 10000, File: "tone:440"},
		{ID: "tone660", Name: "660 Hz stereo tone", Artist: "Generated inside rou2exOS", DurationMS: 10000, File: "tone:660"},
	}}
}
