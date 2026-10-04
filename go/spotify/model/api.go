package model

import (
	"encoding/json"
	"errors"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"strconv"
	"strings"
)

type Transport func(method, url string, headers map[string]string, body []byte) (wire.Response, error)

type API struct {
	Config *Config
	Do     Transport
	// Called after refresh-token rotation so the application can persist it.
	Save  func() error
	token string
}

func form(s string) string {
	const hex = "0123456789ABCDEF"
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune("-._~", rune(c)) {
			b.WriteByte(c)
		} else {
			b.WriteByte('%')
			b.WriteByte(hex[c>>4])
			b.WriteByte(hex[c&15])
		}
	}
	return b.String()
}

func (a *API) refresh() error {
	if a.Config.ClientID == "" || a.Config.RefreshToken == "" {
		return errors.New("configure client_id and refresh_token, or a current access_token")
	}
	body := []byte("grant_type=refresh_token&client_id=" + form(a.Config.ClientID) + "&refresh_token=" + form(a.Config.RefreshToken))
	res, err := a.Do("POST", "https://accounts.spotify.com/api/token", map[string]string{"Content-Type": "application/x-www-form-urlencoded"}, body)
	if err != nil {
		return err
	}
	if res.Status != 200 {
		return apiError(res)
	}
	var reply struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
	}
	if json.Unmarshal(res.Body, &reply) != nil || reply.AccessToken == "" {
		return errors.New("invalid token response")
	}
	a.token = reply.AccessToken
	if reply.RefreshToken != "" && reply.RefreshToken != a.Config.RefreshToken {
		a.Config.RefreshToken = reply.RefreshToken
		if a.Save != nil {
			if err := a.Save(); err != nil {
				return err
			}
		}
	}
	return nil
}

func apiError(r wire.Response) error {
	switch r.Status {
	case 401:
		return errors.New("Spotify sign-in expired; authenticate again")
	case 403:
		return errors.New("Spotify denied access; check app users and playlist permissions")
	case 429:
		return errors.New("Spotify rate limit; retry after " + r.Header["retry-after"] + " seconds")
	}
	return errors.New("Spotify HTTP " + strconv.Itoa(r.Status))
}

func (a *API) get(path string) ([]byte, error) {
	if a.token == "" {
		a.token = a.Config.AccessToken
		if a.token == "" {
			if err := a.refresh(); err != nil {
				return nil, err
			}
		}
	}
	for attempt := 0; attempt < 2; attempt++ {
		r, err := a.Do("GET", "https://api.spotify.com/v1/"+path, map[string]string{"Authorization": "Bearer " + a.token}, nil)
		if err != nil {
			return nil, err
		}
		if r.Status == 401 && attempt == 0 && a.Config.RefreshToken != "" {
			if err := a.refresh(); err != nil {
				return nil, err
			}
			continue
		}
		if r.Status != 200 {
			return nil, apiError(r)
		}
		return r.Body, nil
	}
	return nil, errors.New("Spotify authentication failed")
}

// Paging stays bounded to ten entries, so a large library fits the r2 frame.
func (a *API) Playlists(offset int) ([]Playlist, int, error) {
	if offset < 0 {
		return nil, 0, errors.New("negative playlist offset")
	}
	b, err := a.get("me/playlists?limit=10&offset=" + strconv.Itoa(offset))
	if err != nil {
		return nil, 0, err
	}
	var p struct {
		Items []Playlist `json:"items"`
		Total int        `json:"total"`
	}
	if json.Unmarshal(b, &p) != nil || p.Total < 0 || len(p.Items) > 10 {
		return nil, 0, errors.New("invalid playlist response")
	}
	return p.Items, p.Total, nil
}

type apiTrack struct {
	ID, Name, Type string
	DurationMS     uint32 `json:"duration_ms"`
	Artists        []struct{ Name string }
}

func (a *API) Tracks(id string, offset int) ([]Track, int, error) {
	if len(id) != 22 || offset < 0 {
		return nil, 0, errors.New("invalid playlist id or offset")
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9') {
			return nil, 0, errors.New("invalid playlist id")
		}
	}
	b, err := a.get("playlists/" + id + "/items?limit=10&offset=" + strconv.Itoa(offset))
	if err != nil {
		return nil, 0, err
	}
	var p struct {
		Total int
		Items []struct {
			Item  *apiTrack
			Track *apiTrack
		}
	}
	if json.Unmarshal(b, &p) != nil || p.Total < 0 || len(p.Items) > 10 {
		return nil, 0, errors.New("invalid playlist item response")
	}
	out := make([]Track, 0, len(p.Items))
	for _, entry := range p.Items {
		t := entry.Item
		if t == nil {
			t = entry.Track
		}
		if t == nil || t.Type != "track" {
			out = append(out, Track{Name: "Unavailable track"})
			continue
		}
		artists := make([]string, 0, len(t.Artists))
		for _, x := range t.Artists {
			artists = append(artists, x.Name)
		}
		out = append(out, Track{ID: t.ID, Name: t.Name, Artist: strings.Join(artists, ", "), DurationMS: t.DurationMS, File: a.Config.LocalFiles[t.ID]})
	}
	return out, p.Total, nil
}

// AccessToken returns an authorized token for the native playback session.
// Reauthorization with the streaming scope is required for AP authentication.
func (a *API) AccessToken() (string, error) {
	if a.token == "" {
		a.token = a.Config.AccessToken
		if a.token == "" {
			if err := a.refresh(); err != nil {
				return "", err
			}
		}
	}
	return a.token, nil
}
