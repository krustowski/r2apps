package model

// PlaylistSelection excludes the playlist's tracks to keep the temporary
// configuration small. Indices locate a page; IDs identify a row within it.
type PlaylistSelection struct {
	ID    string `json:"id,omitempty"`
	Name  string `json:"name,omitempty"`
	Index int    `json:"index"`
}

type SessionState struct {
	Live           bool              `json:"live"`
	Playlist       PlaylistSelection `json:"selected_playlist"`
	LoadedPlaylist PlaylistSelection `json:"loaded_playlist"`
	SelectedTrack  Track             `json:"selected_track"`
	TrackIndex     int               `json:"track_index"`
	LastPlayed     Track             `json:"last_played_track"`
	PositionMS     uint32            `json:"position_ms"`
	Volume         uint32            `json:"volume"`
	Muted          bool              `json:"muted"`
}

// ResumePosition restarts completed songs, while retaining an interrupted
// song's position. No audio starts until the user presses Play.
func ResumePosition(saved Track, position uint32, selected Track) uint32 {
	duration := selected.DurationMS
	if duration == 0 {
		duration = saved.DurationMS
	}
	if saved.ID != selected.ID || saved.File != selected.File ||
		(saved.ID == "" && saved.File == "") ||
		duration != 0 && position >= duration {
		return 0
	}
	return position
}
