package stream

import (
	"errors"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestPlaybackPairingPendingAndRotation(t *testing.T) {
	calls, polls, saved, waits := 0, 0, 0, 0
	p := PlaybackTokens{RefreshToken: "expired", Progress: func(s string) {
		if !strings.Contains(s, "spotify.com/pair: ABCD") {
			t.Fatal(s)
		}
	}, Save: func(access, refresh string) error {
		saved++
		if access != "playback" || refresh != "rotated" {
			t.Fatal("wrong credentials persisted")
		}
		return nil
	}}
	transport := func(method, endpoint string, h map[string]string, body []byte) (wire.Response, error) {
		calls++
		form, e := url.ParseQuery(string(body))
		if e != nil {
			t.Fatal(e)
		}
		if method != "POST" || form.Get("client_id") != playbackClientID || h["Content-Type"] != "application/x-www-form-urlencoded" {
			t.Fatal("invalid native authorization request")
		}
		if calls == 1 {
			if form.Get("refresh_token") != "expired" {
				t.Fatal("missing refresh")
			}
			return wire.Response{Status: 400, Body: []byte(`{"error":"invalid_grant"}`)}, nil
		}
		if strings.HasSuffix(endpoint, "/device/authorize") {
			if form.Get("scope") != "streaming" {
				t.Fatal("missing playback scope")
			}
			return wire.Response{Status: 200, Body: []byte(`{"device_code":"private&code","user_code":"ABCD","expires_in":300,"interval":1}`)}, nil
		}
		if form.Get("device_code") != "private&code" || form.Get("grant_type") != "urn:ietf:params:oauth:grant-type:device_code" {
			t.Fatal("invalid pairing grant")
		}
		polls++
		if polls == 1 {
			return wire.Response{Status: 400, Body: []byte(`{"error":"authorization_pending"}`)}, nil
		}
		if polls == 2 {
			return wire.Response{Status: 400, Body: []byte(`{"error":"slow_down"}`)}, nil
		}
		return wire.Response{Status: 200, Body: []byte(`{"access_token":"playback","refresh_token":"rotated","expires_in":3600}`)}, nil
	}
	wait := func(d time.Duration) error {
		waits++
		expected := time.Second
		if waits == 3 {
			expected = 6 * time.Second
		}
		if d != expected {
			t.Fatalf("poll interval %s", d)
		}
		return nil
	}
	token, e := p.Token(transport, wait)
	if e != nil || token != "playback" || calls != 5 || saved != 1 {
		t.Fatalf("pairing failed: %v calls=%d saved=%d", e, calls, saved)
	}
	if _, e = p.Token(transport, wait); e != nil || calls != 5 {
		t.Fatal("cached token caused additional authorization")
	}
}
func TestPlaybackPairingCancellationAndPersistenceFailure(t *testing.T) {
	stop := errors.New("stopped")
	transport := func(method, endpoint string, h map[string]string, body []byte) (wire.Response, error) {
		return wire.Response{Status: 200, Body: []byte(`{"device_code":"private","user_code":"ABCD","expires_in":300}`)}, nil
	}
	p := PlaybackTokens{}
	if _, e := p.Token(transport, func(time.Duration) error { return stop }); e != stop {
		t.Fatalf("cancellation lost: %v", e)
	}
	warned := false
	p = PlaybackTokens{RefreshToken: "valid", Save: func(string, string) error { return errors.New("read-only") }, Progress: func(s string) { warned = strings.Contains(s, "not saved") }}
	transport = func(method, endpoint string, h map[string]string, body []byte) (wire.Response, error) {
		return wire.Response{Status: 200, Body: []byte(`{"access_token":"valid","expires_in":3600}`)}, nil
	}
	if token, e := p.Token(transport, nil); e != nil || token != "valid" || !warned || p.RefreshToken != "valid" {
		t.Fatalf("read-only token handling failed: %v", e)
	}
}
