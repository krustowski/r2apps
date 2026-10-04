package stream

import (
	"bytes"
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"strings"
	"testing"
)

func TestCDNFailoverAndSmallStartupRange(t *testing.T) {
	urls := []string{"https://audio4-fa.scdn.co/file", "https://audio-ak-spotify-com.akamaized.net/file"}
	calls := 0
	r := CDNReader{Total: -1, URL: urls[0], URLs: urls, Do: func(_ string, url string, h map[string]string, _ []byte) (wire.Response, error) {
		calls++
		if h["Range"] != "bytes=0-4095" {
			t.Fatal("startup fetched an entire playback range")
		}
		if calls == 1 {
			return wire.Response{}, errors.New("timed out")
		}
		if url != urls[1] {
			t.Fatal("did not rotate CDN URL")
		}
		return wire.Response{Status: 206, Header: map[string]string{"content-range": "bytes 0-4095/50000"}, Body: bytes.Repeat([]byte{7}, 4096)}, nil
	}}
	var out [27]byte
	if n, e := r.ReadAt(out[:], 0); e != nil || n != len(out) || out[0] != 7 || calls != 2 {
		t.Fatal(n, e, calls)
	}
	if _, e := r.ReadAt(out[:], 27); e != nil || calls != 2 {
		t.Fatal("successful cache was lost", e)
	}
}
func TestCDNRetryBoundsAndCancellation(t *testing.T) {
	timeout := errors.New("timed out")
	cancelled := errors.New("cancelled")
	for _, cancel := range []bool{false, true} {
		calls := 0
		r := CDNReader{Total: -1, URL: "https://audio4-fa.scdn.co/file", Check: func() error {
			if cancel && calls > 0 {
				return cancelled
			}
			return nil
		}, Do: func(string, string, map[string]string, []byte) (wire.Response, error) {
			calls++
			return wire.Response{}, timeout
		}}
		_, e := r.ReadAt(make([]byte, 27), 0)
		if cancel {
			if !errors.Is(e, cancelled) || calls != 1 {
				t.Fatal("cancelled download retried", calls, e)
			}
		} else if !errors.Is(e, timeout) || calls != 3 {
			t.Fatal("unbounded retry", calls, e)
		}
	}
}
func TestCDNRejectsInvalidBodyWithoutRetry(t *testing.T) {
	calls := 0
	r := CDNReader{Total: -1, URL: "https://audio4-fa.scdn.co/file", Do: func(string, string, map[string]string, []byte) (wire.Response, error) {
		calls++
		return wire.Response{Status: 206, Header: map[string]string{"content-range": "bytes 1-27/100"}, Body: make([]byte, 27)}, nil
	}}
	if _, e := r.ReadAt(make([]byte, 27), 0); e == nil || calls != 1 {
		t.Fatal("invalid range retried or accepted")
	}
}
func TestCDNRangesAcrossStartupBoundary(t *testing.T) {
	data := make([]byte, ChunkSize*2+123)
	for i := range data {
		data[i] = byte(i * 13)
	}
	r := CDNReader{Total: -1, URL: "https://audio4-fa.scdn.co/file", Do: func(_, _ string, h map[string]string, _ []byte) (wire.Response, error) {
		var start, end int
		fmt.Sscanf(h["Range"], "bytes=%d-%d", &start, &end)
		if start < ChunkSize && end-start+1 != 4096 {
			t.Fatal("large startup range")
		}
		if end >= len(data) {
			end = len(data) - 1
		}
		return wire.Response{Status: 206, Header: map[string]string{"content-range": fmt.Sprintf("bytes %d-%d/%d", start, end, len(data))}, Body: append([]byte(nil), data[start:end+1]...)}, nil
	}}
	out := make([]byte, ChunkSize+9000)
	if n, e := r.ReadAt(out, 123); e != nil || n != len(out) || !bytes.Equal(out, data[123:123+len(out)]) {
		t.Fatal("range boundary lost bytes", n, e)
	}
	r.Do = func(string, string, map[string]string, []byte) (wire.Response, error) {
		return wire.Response{Status: 403}, nil
	}
	if _, e := r.ReadAt(make([]byte, 1), int64(len(data)-1)); e == nil || !strings.Contains(e.Error(), "403") {
		t.Fatal(e)
	}
}
