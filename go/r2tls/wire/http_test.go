package wire

import (
	"strings"
	"testing"
)

func TestResponseFraming(t *testing.T) {
	for _, raw := range []string{"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n1;extension=yes\r\nc\r\n0\r\n\r\n", "HTTP/1.0 200 OK\r\n\r\nabc"} {
		r, e := Read(strings.NewReader(raw))
		if e != nil || string(r.Body) != "abc" {
			t.Fatalf("%#v %v", r, e)
		}
	}
}
func TestRejectsTruncationAndOversize(t *testing.T) {
	for _, raw := range []string{"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na", "HTTP/1.1 200 OK\r\nContent-Length: 65537\r\n\r\n", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10001\r\n", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naZZ", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nabc"} {
		if _, e := Read(strings.NewReader(raw)); e == nil {
			t.Fatal("malformed response accepted")
		}
	}
}

func TestKeepAliveFraming(t *testing.T) {
	for _, closeHeader := range []string{"", "Connection: keep-alive\r\n", "Connection: Keep-Alive, Close\r\n"} {
		r, e := Read(strings.NewReader("HTTP/1.1 206 Partial Content\r\nContent-Length: 3\r\n" + closeHeader + "\r\nabc"))
		if e != nil || r.Reusable() == strings.Contains(closeHeader, "Close") {
			t.Fatalf("reuse %q: %v", closeHeader, e)
		}
	}
	raw := "HTTP/1.1 206 Partial Content\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\nX-Trailer: checked\r\n\r\n"
	r, e := Read(strings.NewReader(raw))
	if e != nil || !r.Reusable() || string(r.Body) != "abc" {
		t.Fatalf("trailers: %v", e)
	}
	if _, e = Read(strings.NewReader(strings.TrimSuffix(raw, "\r\n"))); e == nil {
		t.Fatal("truncated trailers accepted")
	}
}
