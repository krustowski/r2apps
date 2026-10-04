package stream

import (
	"bytes"
	"testing"
)

func TestPlaybackKeyAcrossSessions(t *testing.T) {
	c := Credentials{RefreshToken: "renewable-token", Device: "0123456789012345678901234567890123456789"}
	record, e := c.Encode()
	if e != nil || len(record) != CredentialFileSize {
		t.Fatal(e)
	}
	loaded, e := DecodeCredentials(record)
	if e != nil || loaded != c {
		t.Fatalf("reopen: %v", e)
	}
	short := Credentials{RefreshToken: "short"}
	next, e := short.Encode()
	if e != nil {
		t.Fatal(e)
	}
	copy(record, next)
	loaded, e = DecodeCredentials(record)
	if e != nil || loaded != short {
		t.Fatal("short rotation left stale bytes")
	}
	for _, bad := range [][]byte{record[:5], bytes.Repeat([]byte{'x'}, CredentialFileSize+1), []byte(`{"refresh_token":"valid","device_id":"bad"}`)} {
		if _, e = DecodeCredentials(bad); e == nil {
			t.Fatal("corrupt key accepted")
		}
	}
}
