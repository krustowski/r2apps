package stream

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

// PlaybackTokens are separate from a user's Web API developer-app tokens.
// Spotify Connect's public client supports its native device authorization flow.
type PlaybackTokens struct {
	AccessToken, RefreshToken string
	expires                   time.Time
	Save                      func(access, refresh string) error
	Progress                  func(string)
}

func (p *PlaybackTokens) report(s string) {
	if p.Progress != nil {
		p.Progress(s)
	}
}
func (p *PlaybackTokens) Token(do HTTP, wait func(time.Duration) error) (string, error) {
	if p.AccessToken != "" && (p.expires.IsZero() || time.Now().Before(p.expires)) {
		return p.AccessToken, nil
	}
	headers := map[string]string{"Content-Type": "application/x-www-form-urlencoded"}
	if p.RefreshToken != "" {
		r, e := do("POST", "https://accounts.spotify.com/api/token", headers, []byte("grant_type=refresh_token&client_id="+playbackClientID+"&refresh_token="+formValue(p.RefreshToken)))
		if e != nil {
			return "", e
		}
		if r.Status == 200 {
			return p.accept(r.Body)
		}
		if r.Status != 400 && r.Status != 401 {
			return "", fmt.Errorf("playback token refresh HTTP %d", r.Status)
		}
	}
	response, e := do("POST", "https://accounts.spotify.com/oauth2/device/authorize", headers, []byte("client_id="+playbackClientID+"&scope=streaming"))
	if e != nil {
		return "", e
	}
	if response.Status != 200 {
		return "", fmt.Errorf("Spotify playback pairing HTTP %d", response.Status)
	}
	var authorization struct {
		DeviceCode      string `json:"device_code"`
		UserCode        string `json:"user_code"`
		VerificationURI string `json:"verification_uri"`
		ExpiresIn       int    `json:"expires_in"`
		Interval        int    `json:"interval"`
	}
	if json.Unmarshal(response.Body, &authorization) != nil || authorization.DeviceCode == "" || authorization.UserCode == "" || authorization.ExpiresIn < 1 {
		return "", errors.New("invalid Spotify playback pairing response")
	}
	if authorization.Interval < 1 {
		authorization.Interval = 5
	}
	if authorization.Interval > 60 || authorization.ExpiresIn > 3600 {
		return "", errors.New("invalid Spotify pairing timeout")
	}
	// A fixed first-party URL avoids treating server-supplied text as navigation.
	p.report("Authorize playback at spotify.com/pair: " + authorization.UserCode)
	deadline := time.Now().Add(time.Duration(authorization.ExpiresIn) * time.Second)
	for time.Now().Before(deadline) {
		if e := wait(time.Duration(authorization.Interval) * time.Second); e != nil {
			return "", e
		}
		body := []byte("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=" + playbackClientID + "&device_code=" + formValue(authorization.DeviceCode))
		r, e := do("POST", "https://accounts.spotify.com/api/token", headers, body)
		if e != nil {
			return "", e
		}
		if r.Status == 200 {
			return p.accept(r.Body)
		}
		var failure struct{ Error string }
		if json.Unmarshal(r.Body, &failure) != nil {
			return "", errors.New("invalid Spotify pairing response")
		}
		switch failure.Error {
		case "authorization_pending":
			continue
		case "slow_down":
			authorization.Interval += 5
			if authorization.Interval > 60 {
				return "", errors.New("Spotify pairing polling limit exceeded")
			}
		case "access_denied":
			return "", errors.New("Spotify playback authorization declined")
		case "expired_token":
			return "", errors.New("Spotify playback pairing expired; press Play again")
		default:
			return "", fmt.Errorf("Spotify playback token HTTP %d", r.Status)
		}
	}
	return "", errors.New("Spotify playback pairing expired; press Play again")
}
func (p *PlaybackTokens) accept(body []byte) (string, error) {
	var token struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int    `json:"expires_in"`
	}
	if json.Unmarshal(body, &token) != nil || token.AccessToken == "" || token.ExpiresIn < 1 {
		return "", errors.New("invalid playback OAuth token response")
	}
	p.AccessToken = token.AccessToken
	if token.RefreshToken != "" {
		p.RefreshToken = token.RefreshToken
	}
	seconds := token.ExpiresIn - 60
	if seconds < 1 {
		seconds = 1
	}
	p.expires = time.Now().Add(time.Duration(seconds) * time.Second)
	if p.Save != nil {
		if e := p.Save(p.AccessToken, p.RefreshToken); e != nil {
			p.report("Playback signed in; token not saved. Re-pair after closing.")
		}
	}
	return p.AccessToken, nil
}
func formValue(s string) string {
	var b strings.Builder
	const digits = "0123456789ABCDEF"
	for i := range s {
		c := s[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune("-._~", rune(c)) {
			b.WriteByte(c)
		} else {
			b.WriteByte('%')
			b.WriteByte(digits[c>>4])
			b.WriteByte(digits[c&15])
		}
	}
	return b.String()
}
