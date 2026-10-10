package main

import (
	"errors"
	"io"
	"strconv"
	"strings"
	"testing"
	"time"
)

// fakeConn answers each request written to it with the next scripted
// response; an empty script entry means the connection dies at that point.
type fakeConn struct {
	script []string
	asked  int
	buf    []byte
	closed bool
	stop   bool // a request on it is stopped half way
}

func (c *fakeConn) Write(b []byte) (int, error) {
	if c.closed {
		return 0, errors.New("closed")
	}
	if strings.HasPrefix(string(b), "GET ") || strings.HasPrefix(string(b), "POST ") {
		if c.asked < len(c.script) {
			c.buf = append(c.buf, c.script[c.asked]...)
		}
		c.asked++
	}
	return len(b), nil
}

func (c *fakeConn) Read(b []byte) (int, error) {
	if c.stop {
		return 0, errStopped
	}
	if len(c.buf) == 0 {
		return 0, io.EOF
	}
	n := copy(b, c.buf)
	c.buf = c.buf[n:]
	return n, nil
}

func (c *fakeConn) Close() error             { c.closed = true; return nil }
func (c *fakeConn) SetTimeout(time.Duration) {}

func reply(body string, keep bool) string {
	h := "HTTP/1.1 200 OK\r\nContent-Length: " + itoa(len(body)) + "\r\n"
	if !keep {
		h += "Connection: close\r\n"
	}
	return h + "\r\n" + body
}

func itoa(n int) string { return strconv.Itoa(n) }

type dials struct {
	conns []*fakeConn
	next  [][]string
}

func (d *dials) dial(bool, string, uint16, time.Duration) (conn, error) {
	if len(d.next) == 0 {
		return nil, errors.New("TCP api.telegram.org: timeout")
	}
	c := &fakeConn{script: d.next[0]}
	d.next = d.next[1:]
	d.conns = append(d.conns, c)
	return c, nil
}

func newClient(d *dials, clock *uint64) *client {
	return &client{dial: d.dial, now: func() uint64 { return *clock }}
}

var getMe = job{kind: rqGetMe, api: "https://api.telegram.org", path: "/botT/getMe"}
var send = job{kind: rqSend, api: "https://api.telegram.org", path: "/botT/sendMessage", body: form("text=hi")}

func TestKeepAlive(t *testing.T) {
	clock := uint64(1000)
	d := &dials{next: [][]string{{reply("{}", true), reply("{1}", true), reply("{2}", false)}, {reply("{3}", true)}}}
	c := newClient(d, &clock)
	for i, want := range []string{"{}", "{1}", "{2}", "{3}"} {
		r := c.do(getMe)
		if r.err != nil || string(r.body) != want || r.fresh != (i == 0 || i == 3) {
			t.Fatalf("request %d: %q fresh %v err %v", i, r.body, r.fresh, r.err)
		}
	}
	if len(d.conns) != 2 || !d.conns[0].closed {
		t.Fatalf("%d connections; the first closed: %v", len(d.conns), d.conns[0].closed)
	}
	// Kept too long: not trusted.
	clock += keepIdleMS + 1
	d.next = [][]string{{reply("{4}", true)}}
	if r := c.do(getMe); !r.fresh || string(r.body) != "{4}" {
		t.Fatal("an old kept connection was used")
	}
}

func TestKeptConnectionClosedByServer(t *testing.T) {
	clock := uint64(0)
	// The second request finds the kept connection gone (nothing comes
	// back): a GET goes again on a new one, and so does a POST.
	d := &dials{next: [][]string{{reply("{}", true)}, {reply("{a}", true)}, {reply("{b}", true)}}}
	c := newClient(d, &clock)
	c.do(getMe)
	if r := c.do(getMe); r.err != nil || string(r.body) != "{a}" || !r.fresh {
		t.Fatalf("GET not retried: %q %v", r.body, r.err)
	}
	c.conn.(*fakeConn).script = nil // dies again
	if r := c.do(send); r.err != nil || string(r.body) != "{b}" {
		t.Fatalf("POST not retried: %q %v", r.body, r.err)
	}
	// Half an answer, then the connection dies: a POST may have been taken,
	// so it is not sent twice.
	c.conn.(*fakeConn).script = []string{"", "HTTP/1.1 200 OK\r\nContent-Le"}
	c.conn.(*fakeConn).asked = 1
	d.next = [][]string{{reply("{c}", true)}}
	if r := c.do(send); r.err == nil || len(d.next) != 1 {
		t.Fatalf("a half-answered POST went again: %v", r.err)
	}
	// And a connection that cannot be made at all is an error.
	d.next = nil
	if r := c.do(getMe); r.err == nil || r.status != 0 {
		t.Fatal("no connection, no error")
	}
}

func TestStoppedDropsTheConnection(t *testing.T) {
	clock := uint64(0)
	d := &dials{next: [][]string{{reply("{}", true)}, {reply("{x}", true)}}}
	c := newClient(d, &clock)
	c.do(getMe)
	c.conn.(*fakeConn).stop = true
	if r := c.do(getMe); r.err != errStopped || c.conn != nil || len(d.next) != 1 {
		t.Fatalf("stopped: %v, kept %v", r.err, c.conn != nil)
	}
	if r := c.do(getMe); !r.fresh || string(r.body) != "{x}" {
		t.Fatal("no new connection after a stop")
	}
}

func TestBigBodiesOutsideTheHeap(t *testing.T) {
	clock := uint64(0)
	big := strings.Repeat("x", bigBody)
	d := &dials{next: [][]string{{"HTTP/1.1 200 OK\r\nContent-Length: 65536\r\n\r\n" + big}}}
	c := newClient(d, &clock)
	var given, freed int
	c.alloc = func(n int) []byte { given += n; return make([]byte, n) }
	c.free = func([]byte) { freed++ }
	r := c.do(job{kind: rqFile, api: "https://api.telegram.org", path: "/file/botT/x", limit: 1 << 20})
	if r.err != nil || len(r.body) != bigBody || given != bigBody {
		t.Fatalf("%d bytes, %d given: %v", len(r.body), given, r.err)
	}
	r.free()
	r.free()
	if freed != 1 || r.body != nil {
		t.Fatalf("freed %d times", freed)
	}
}

func TestRequestHead(t *testing.T) {
	e, _ := parseAPI("http://10.0.2.2:8081/x/")
	head := string(e.request(job{path: "/botT/sendPhoto", body: [][]byte{[]byte("ab"), []byte("cde")}, contentType: "multipart/form-data; boundary=b"}))
	if !strings.HasPrefix(head, "POST /x/botT/sendPhoto HTTP/1.1\r\nHost: 10.0.2.2:8081\r\n") ||
		!strings.Contains(head, "Content-Length: 5\r\n") || !strings.Contains(head, "Connection: keep-alive") {
		t.Fatalf("head %q", head)
	}
}
