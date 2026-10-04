package stream

import (
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"io"
	"strconv"
	"strings"
)

const ChunkSize = 16384
const headerRangeSize = 4096

type CDNReader struct {
	Do          HTTP
	URL         string
	URLs        []string
	Check       func() error
	Progress    func(string)
	urlIndex    int
	Total       int64
	DurationMS  uint32
	cache       []byte
	cacheOffset int64
}

func validCDNURL(url string) bool {
	if !strings.HasPrefix(url, "https://") {
		return false
	}
	host := strings.SplitN(strings.TrimPrefix(url, "https://"), "/", 2)[0]
	return !strings.ContainsAny(host, "@:\r\n") && (strings.HasSuffix(host, ".scdn.co") || host == "audio-ak-spotify-com.akamaized.net")
}
func (r *CDNReader) ReadAt(p []byte, offset int64) (int, error) {
	if offset < 0 {
		return 0, errors.New("negative CDN offset")
	}
	copied := 0
	for copied < len(p) {
		pos := offset + int64(copied)
		if r.Total >= 0 && pos >= r.Total {
			return copied, io.EOF
		}
		if pos < r.cacheOffset || pos >= r.cacheOffset+int64(len(r.cache)) {
			size := int64(ChunkSize)
			if pos < ChunkSize {
				size = headerRangeSize
			}
			start := pos / size * size
			end := start + size - 1
			response, total, e := r.rangeRequest(start, end)
			if e != nil {
				return copied, fmt.Errorf("audio range %d: %w", start, e)
			}
			if r.Total >= 0 && r.Total != total {
				return copied, errors.New("audio CDN file size changed")
			}
			r.Total = total
			r.cacheOffset = start
			r.cache = response.Body
		}
		n := copy(p[copied:], r.cache[pos-r.cacheOffset:])
		copied += n
	}
	return copied, nil
}

// A failed request never replaces a validated cache entry. Retry at most
// three times, rotating only among URLs supplied for this same audio file.
func (r *CDNReader) rangeRequest(start, end int64) (wire.Response, int64, error) {
	var last error
	urls := r.URLs
	if len(urls) == 0 {
		urls = []string{r.URL}
	}
	for attempt := 0; attempt < 3; attempt++ {
		if r.Check != nil {
			if e := r.Check(); e != nil {
				return wire.Response{}, 0, e
			}
		}
		url := urls[r.urlIndex%len(urls)]
		if !validCDNURL(url) {
			return wire.Response{}, 0, errors.New("invalid audio CDN URL")
		}
		response, e := r.Do("GET", url, map[string]string{"Range": fmt.Sprintf("bytes=%d-%d", start, end), "Accept": "application/octet-stream"}, nil)
		if e == nil && response.Status == 206 {
			a, b, total, err := contentRange(response.Header["content-range"])
			if err != nil || a != start || b > end || int64(len(response.Body)) != b-a+1 || len(response.Body) == 0 {
				return wire.Response{}, 0, errors.New("invalid audio CDN content range")
			}
			return response, total, nil
		}
		if e == nil {
			e = fmt.Errorf("audio CDN HTTP %d", response.Status)
			if response.Status != 408 && response.Status != 429 && response.Status < 500 && response.Status != 403 && response.Status != 404 && response.Status != 410 {
				return wire.Response{}, 0, e
			}
		}
		last = e
		if r.Check != nil {
			if e := r.Check(); e != nil {
				return wire.Response{}, 0, e
			}
		}
		if attempt < 2 {
			r.urlIndex = (r.urlIndex + 1) % len(urls)
			if r.Progress != nil {
				r.Progress(fmt.Sprintf("Retrying Spotify audio download (%d/3)...", attempt+2))
			}
		}
	}
	return wire.Response{}, 0, last
}
func contentRange(s string) (int64, int64, int64, error) {
	fail := errors.New("invalid content range")
	if !strings.HasPrefix(s, "bytes ") {
		return 0, 0, 0, fail
	}
	parts := strings.Split(strings.TrimPrefix(s, "bytes "), "/")
	if len(parts) != 2 {
		return 0, 0, 0, fail
	}
	span := strings.Split(parts[0], "-")
	if len(span) != 2 {
		return 0, 0, 0, fail
	}
	a, e := strconv.ParseInt(span[0], 10, 64)
	if e != nil {
		return 0, 0, 0, fail
	}
	b, e := strconv.ParseInt(span[1], 10, 64)
	if e != nil {
		return 0, 0, 0, fail
	}
	total, e := strconv.ParseInt(parts[1], 10, 64)
	if e != nil || a < 0 || b < a || b >= total || total > 512*1024*1024 {
		return 0, 0, 0, fail
	}
	return a, b, total, nil
}
