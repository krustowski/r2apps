// Package wire implements bounded HTTP response framing without net/http.
package wire

import (
	"bufio"
	"errors"
	"io"
	"strconv"
	"strings"
)

const MaxBody = 64 * 1024

// ErrTooLarge is returned when a body exceeds the limit it was read with.
var ErrTooLarge = errors.New("HTTP body exceeds its limit")

type Response struct {
	Status int
	Header map[string]string
	Body   []byte
}

func Read(r io.Reader) (Response, error) { return ReadLimit(r, MaxBody) }

// ReadLimit is Read with a body of at most limit bytes rather than MaxBody.
func ReadLimit(r io.Reader, limit int) (Response, error) {
	var out Response
	b := bufio.NewReaderSize(r, 2048)
	line, err := b.ReadSlice('\n')
	if err != nil {
		return out, err
	}
	parts := strings.SplitN(strings.TrimSpace(string(line)), " ", 3)
	if len(parts) < 2 || (parts[0] != "HTTP/1.0" && parts[0] != "HTTP/1.1") {
		return out, errors.New("invalid HTTP status")
	}
	out.Status, err = strconv.Atoi(parts[1])
	if err != nil || out.Status < 100 || out.Status > 599 {
		return out, errors.New("invalid HTTP status")
	}
	out.Header = make(map[string]string)
	for bytes := len(line); ; {
		line, err = b.ReadSlice('\n')
		if err != nil {
			return out, err
		}
		bytes += len(line)
		if bytes > 8192 {
			return out, errors.New("HTTP headers exceed 8 KiB")
		}
		s := strings.TrimSpace(string(line))
		if s == "" {
			break
		}
		i := strings.IndexByte(s, ':')
		if i < 1 {
			return out, errors.New("invalid HTTP header")
		}
		k, v := strings.ToLower(s[:i]), strings.TrimSpace(s[i+1:])
		if old, ok := out.Header[k]; ok {
			if k == "content-length" && old != v {
				return out, errors.New("conflicting content lengths")
			}
			v = old + "," + v
		}
		out.Header[k] = v
	}
	if out.Status < 200 || out.Status == 204 || out.Status == 304 {
		return out, nil
	}
	if encoding := out.Header["content-encoding"]; encoding != "" && encoding != "identity" {
		return out, errors.New("compressed HTTP response unsupported")
	}
	if encoding := out.Header["transfer-encoding"]; encoding != "" {
		if strings.ToLower(encoding) != "chunked" {
			return out, errors.New("unsupported HTTP transfer encoding")
		}
		for {
			line, err = b.ReadSlice('\n')
			if err != nil {
				return out, err
			}
			s := strings.TrimSpace(string(line))
			if i := strings.IndexByte(s, ';'); i >= 0 {
				s = s[:i]
			}
			n, err := strconv.ParseUint(s, 16, 32)
			if err != nil {
				return out, errors.New("invalid HTTP chunk")
			}
			if n == 0 {
				// Consume the terminating CRLF and bounded trailers before
				// reusing this connection for another response.
				for total := 0; ; {
					trailer, e := b.ReadSlice('\n')
					if e != nil {
						return out, e
					}
					total += len(trailer)
					if total > 8192 {
						return out, errors.New("HTTP trailers exceed 8 KiB")
					}
					if string(trailer) == "\r\n" {
						return out, nil
					}
					if !strings.Contains(string(trailer), ":") {
						return out, errors.New("invalid HTTP trailer")
					}
				}
			}
			if uint64(len(out.Body))+n > uint64(limit) {
				return out, ErrTooLarge
			}
			at := len(out.Body)
			out.Body = append(out.Body, make([]byte, int(n))...)
			if _, err := io.ReadFull(b, out.Body[at:]); err != nil {
				return out, err
			}
			var crlf [2]byte
			if _, err := io.ReadFull(b, crlf[:]); err != nil {
				return out, err
			}
			if string(crlf[:]) != "\r\n" {
				return out, errors.New("invalid HTTP chunk terminator")
			}
		}
	}
	if s, ok := out.Header["content-length"]; ok {
		n, err := strconv.ParseUint(s, 10, 32)
		if err != nil {
			return out, errors.New("invalid HTTP content length")
		}
		if n > uint64(limit) {
			return out, ErrTooLarge
		}
		out.Body = make([]byte, int(n))
		_, err = io.ReadFull(b, out.Body)
		return out, err
	}
	out.Body, err = io.ReadAll(io.LimitReader(b, int64(limit)+1))
	if len(out.Body) > limit {
		return out, ErrTooLarge
	}
	return out, err
}

// Reusable reports whether a complete response has an explicit body boundary.
func (r Response) Reusable() bool {
	for _, token := range strings.Split(r.Header["connection"], ",") {
		if strings.EqualFold(strings.TrimSpace(token), "close") {
			return false
		}
	}
	return r.Status >= 200 && (r.Header["content-length"] != "" || strings.EqualFold(r.Header["transfer-encoding"], "chunked"))
}
