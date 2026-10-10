package main

import (
	"errors"
	"io"
	"strconv"
	"strings"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
)

// The traffic is one request at a time, driven by the network worker:
//
//	getMe                   once, to check the token and learn the bot's name
//	getUpdates?timeout=20   a long poll: the server holds it until something
//	                        arrives or 20 s pass
//	sendMessage, sendAnimation, sendPhoto, setMessageReaction
//	                        what was typed, pasted or picked; a poll in
//	                        progress is dropped for them
//	getFile, then the file  a picture someone sent
//
// One connection is kept open from one request to the next (HTTP/1.1
// keep-alive), so the TLS handshake --- seconds on a slow machine, and the
// part most likely to fail --- comes once rather than with every request.  A
// request stopped half way (a poll dropped for a message) takes its
// connection with it.

var errStopped = errors.New("Stopped")

// job is a request for the worker.
type job struct {
	kind        request
	api         string   // "https://api.telegram.org"
	path        string   // "/bot<token>/getMe"
	body        [][]byte // POSTed, one part after another, when not nil
	contentType string   // of the body; form encoded when ""
	limit       int      // of the answer's body; jsonLimit when 0
}

// result is what came back: an HTTP answer, or err when there was none.
type result struct {
	kind    request
	status  int
	body    []byte
	release func() // gives the body back when it lives outside the Go heap
	err     error
	took    uint64 // milliseconds
	fresh   bool   // on a connection of its own (with a handshake)
}

func (r *result) free() {
	if r.release != nil {
		r.release()
		r.release = nil
	}
	r.body = nil
}

const (
	jsonLimit = 1024 * 1024
	// The server holds a long poll for 20 s; a connection that says nothing
	// for longer than this has gone.
	idleTimeout = 30 * time.Second
	// A kept connection older than this is not trusted with a request: the
	// server may have closed it meanwhile.
	keepIdleMS = 15000
	// A body this big or bigger goes outside the Go heap.
	bigBody = 64 * 1024
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

func (j job) method() string {
	if j.body != nil {
		return "POST"
	}
	return "GET"
}

// request is the HTTP request head for j.
func (e endpoint) request(j job) []byte {
	var b strings.Builder
	b.WriteString(j.method() + " " + e.prefix + j.path + " HTTP/1.1\r\nHost: " + e.host)
	if (e.secure && e.port != 443) || (!e.secure && e.port != 80) {
		b.WriteString(":" + strconv.Itoa(int(e.port)))
	}
	b.WriteString("\r\nUser-Agent: r2telegram/1.0\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n")
	if j.body != nil {
		ct := j.contentType
		if ct == "" {
			ct = "application/x-www-form-urlencoded"
		}
		n := 0
		for _, p := range j.body {
			n += len(p)
		}
		b.WriteString("Content-Type: " + ct + "\r\nContent-Length: " + strconv.Itoa(n) + "\r\n")
	}
	b.WriteString("\r\n")
	return []byte(b.String())
}

// counter counts what is read through it: a kept connection that fails
// before its answer has begun was most likely closed by the server.
type counter struct {
	r io.Reader
	n int
}

func (c *counter) Read(b []byte) (int, error) {
	n, err := c.r.Read(b)
	c.n += n
	return n, err
}

// client does jobs over one kept connection.
type client struct {
	dial   dialer
	now    func() uint64
	alloc  func(n int) []byte // room outside the Go heap, or nil
	free   func([]byte)
	conn   conn
	key    string // the API the connection is to
	usedAt uint64
}

func (c *client) drop() {
	if c.conn != nil {
		c.conn.Close()
		c.conn = nil
	}
}

// do does j.  A request stopped on the way comes back as errStopped,
// however far down it was noticed.
func (c *client) do(j job) result {
	start := c.now()
	r := c.attempt(j, false)
	// A kept connection the server had closed: once more on a new one.  A
	// POST only when nothing came back at all, since then it was not read.
	if r.err != nil && !r.fresh && !errors.Is(r.err, errStopped) && (j.body == nil || r.status == -1) {
		r.free()
		r = c.attempt(j, true)
	}
	if r.err != nil && errors.Is(r.err, errStopped) {
		r.err = errStopped
	}
	if r.status < 0 {
		r.status = 0
	}
	r.took = c.now() - start
	return r
}

// attempt does j once; status -1 with an error says nothing was read back.
func (c *client) attempt(j job, fresh bool) result {
	r := result{kind: j.kind, status: -1}
	e, err := parseAPI(j.api)
	if err != nil {
		r.err = err
		return r
	}
	if fresh || c.conn == nil || c.key != j.api || c.now()-c.usedAt > keepIdleMS {
		c.drop()
		if c.conn, err = c.dial(e.secure, e.host, e.port, idleTimeout); err != nil {
			c.conn = nil
			r.err = err
			return r
		}
		c.key, r.fresh = j.api, true
	}
	c.conn.SetTimeout(idleTimeout)
	if _, err = c.conn.Write(e.request(j)); err == nil {
		for _, p := range j.body {
			if _, err = c.conn.Write(p); err != nil {
				break
			}
		}
	}
	if err != nil {
		c.drop()
		r.err = err
		return r
	}
	limit := j.limit
	if limit == 0 {
		limit = jsonLimit
	}
	var big []byte
	alloc := func(n int) []byte {
		if n >= bigBody && c.alloc != nil {
			big = c.alloc(n)
		}
		return big
	}
	in := &counter{r: c.conn}
	resp, err := wire.ReadAlloc(in, limit, alloc)
	if big != nil {
		r.release = func() { c.free(big) }
	}
	if err != nil {
		c.drop()
		if in.n > 0 {
			r.status = 0
		}
		if err == wire.ErrTooLarge {
			err = errors.New("the answer is too big")
		}
		r.free()
		r.err = err
		return r
	}
	if resp.Reusable() {
		c.usedAt = c.now()
	} else {
		c.drop()
	}
	r.status, r.body = resp.Status, resp.Body
	return r
}
