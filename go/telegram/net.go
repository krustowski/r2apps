package main

import (
	"errors"
	"io"
	"strconv"
	"strings"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
)

// The traffic is one request at a time, each on a connection of its own
// (Connection: close), driven by the network worker:
//
//	getMe                   once, to check the token and learn the bot's name
//	getUpdates?timeout=20   a long poll: the server holds it until something
//	                        arrives or 20 s pass
//	sendMessage, sendAnimation, sendPhoto, setMessageReaction
//	                        what was typed, pasted or picked; a poll in
//	                        progress is dropped for them
//	getFile, then the file  a picture someone sent

var errStopped = errors.New("Stopped")

// job is a request for the worker.
type job struct {
	kind        request
	api         string // "https://api.telegram.org"
	path        string // "/bot<token>/getMe"
	body        []byte // POSTed when not nil
	contentType string // of the body; form encoded when ""
	limit       int    // of the answer's body; jsonLimit when 0
}

// result is what came back: an HTTP answer, or err when there was none.
type result struct {
	kind   request
	status int
	body   []byte
	err    error
}

const (
	jsonLimit = 1024 * 1024
	// The server holds a long poll for 20 s; a connection that says nothing
	// for longer than this has gone.
	idleTimeout = 30 * time.Second
)

// conn is a connection to the API: TLS, or plain TCP for a test server.
type conn interface {
	io.ReadWriteCloser
	SetTimeout(time.Duration)
}

type dialer func(secure bool, host string, port uint16, timeout time.Duration) (conn, error)

type endpoint struct {
	secure bool
	host   string
	port   uint16
	prefix string
}

// parseAPI reads "https://api.telegram.org" or "http://10.0.2.2:8081/x".
func parseAPI(api string) (endpoint, error) {
	var e endpoint
	rest, ok := strings.CutPrefix(api, "https://")
	if ok {
		e.secure, e.port = true, 443
	} else if rest, ok = strings.CutPrefix(api, "http://"); ok {
		e.port = 80
	} else {
		return e, errors.New("the API address must start with https:// or http://")
	}
	if i := strings.IndexByte(rest, '/'); i >= 0 {
		rest, e.prefix = rest[:i], strings.TrimRight(rest[i:], "/")
	}
	e.host = rest
	if i := strings.LastIndexByte(rest, ':'); i >= 0 {
		p, err := strconv.ParseUint(rest[i+1:], 10, 16)
		if err != nil || p == 0 {
			return e, errors.New("bad port in the API address")
		}
		e.host, e.port = rest[:i], uint16(p)
	}
	if e.host == "" || strings.ContainsAny(e.host, " \r\n") {
		return e, errors.New("bad host in the API address")
	}
	return e, nil
}

// request is the HTTP request for j.
func (e endpoint) request(j job) []byte {
	method := "GET"
	if j.body != nil {
		method = "POST"
	}
	var b strings.Builder
	b.WriteString(method + " " + e.prefix + j.path + " HTTP/1.1\r\nHost: " + e.host)
	if (e.secure && e.port != 443) || (!e.secure && e.port != 80) {
		b.WriteString(":" + strconv.Itoa(int(e.port)))
	}
	b.WriteString("\r\nUser-Agent: r2telegram/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n")
	if j.body != nil {
		ct := j.contentType
		if ct == "" {
			ct = "application/x-www-form-urlencoded"
		}
		b.WriteString("Content-Type: " + ct + "\r\nContent-Length: " + strconv.Itoa(len(j.body)) + "\r\n")
	}
	b.WriteString("\r\n")
	return []byte(b.String())
}

// exchange does j over a connection of its own.  A request stopped on the
// way comes back as errStopped, however far down it was noticed.
func exchange(dial dialer, j job) result {
	r := do(dial, j)
	if r.err != nil && errors.Is(r.err, errStopped) {
		r.err = errStopped
	}
	return r
}

func do(dial dialer, j job) result {
	r := result{kind: j.kind}
	e, err := parseAPI(j.api)
	if err != nil {
		r.err = err
		return r
	}
	c, err := dial(e.secure, e.host, e.port, idleTimeout)
	if err != nil {
		r.err = err
		return r
	}
	defer c.Close()
	c.SetTimeout(idleTimeout)
	if _, err = c.Write(e.request(j)); err == nil && len(j.body) > 0 {
		_, err = c.Write(j.body)
	}
	if err != nil {
		r.err = err
		return r
	}
	limit := j.limit
	if limit == 0 {
		limit = jsonLimit
	}
	resp, err := wire.ReadLimit(c, limit)
	if err != nil {
		if err == wire.ErrTooLarge {
			err = errors.New("the answer is too big")
		}
		r.err = err
		return r
	}
	r.status, r.body = resp.Status, resp.Body
	return r
}
