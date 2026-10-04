//go:build r2

// Package r2tls runs Memento's portable BearSSL engine over the Go r2net
// stack. No host proxy, OS sockets, insecure TLS mode or Go net package.
package r2tls

/*
#cgo LDFLAGS: -L./build -lbearssl
#include "bridge.h"
*/
import "C"

import (
	"crypto/sha256"
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"io"
	"strconv"
	"strings"
	"time"
	"unsafe"
)

var anchors uintptr
var anchorError error

func initAnchors() error {
	if anchorError != nil {
		return anchorError
	}
	if anchors != 0 {
		return nil
	}
	const cap = 256 * 1024
	addr := libgor2.KMalloc(cap)
	if addr == 0 {
		return errors.New("no memory for TLS trust roots")
	}
	buf := libgor2.KBytes(addr, cap)
	var n int
	for _, path := range []string{"/mnt/tar/opt/memento/cacerts.bin", "/mnt/iso/opt/memento/cacerts.bin"} {
		count, err := libgor2.ReadFileAt(path, buf, 0)
		if err == nil && count > 0 {
			n = count
			break
		}
	}
	if n == 0 {
		libgor2.KFree(addr)
		return errors.New("install Memento cacerts.bin before connecting")
	}
	C.r2tls_anchors((*C.uchar)(unsafe.Pointer(addr)), C.ulong(n))
	if C.web_tls_anchor_count() <= 0 {
		// BearSSL may retain pointers into this file after parsing it.
		anchors = addr
		anchorError = errors.New("invalid TLS trust-root file")
		return anchorError
	}
	anchors = addr
	return nil
}

// Random fills protocol nonces/private keys using the same seed policy as TLS.
// The Memento timing fallback retains its unverified entropy quality.
func Random(out []byte) error {
	if len(out) == 0 {
		return nil
	}
	if C.r2tls_random((*C.uchar)(unsafe.Pointer(&out[0])), C.ulong(len(out))) > 0 {
		return nil
	}
	var raw [512]byte
	C.r2tls_timing_seed((*C.uchar)(unsafe.Pointer(&raw[0])), 512)
	seed := sha256.Sum256(raw[:])
	for at := 0; at < len(out); {
		seed = sha256.Sum256(seed[:])
		at += copy(out[at:], seed[:])
	}
	return nil
}

type Client struct {
	Stack     *r2net.Stack
	Check     func() error
	audio     *tlsConn
	audioHost string
}

func (c *Client) Close() {
	if c.audio != nil {
		c.audio.Close()
		c.audio = nil
	}
	c.audioHost = ""
}

type tlsConn struct {
	tcp      *r2net.Conn
	engine   *C.web_tls
	deadline uint64
	idleMS   uint64
	check    func() error
}

func (c *Client) connect(host string, port uint16, timeout time.Duration) (*tlsConn, error) {
	if c.Check != nil {
		if e := c.Check(); e != nil {
			return nil, e
		}
	}
	if err := initAnchors(); err != nil {
		return nil, err
	}
	var seed [512]byte
	seedLen := 32
	if C.r2tls_random((*C.uchar)(unsafe.Pointer(&seed[0])), C.ulong(seedLen)) <= 0 {
		// Same compatibility fallback and sample count as Memento's browser.
		// Certificate validation stays enabled; seed quality is unverified.
		seedLen = len(seed)
		C.r2tls_timing_seed((*C.uchar)(unsafe.Pointer(&seed[0])), C.ulong(seedLen))
	}
	var rtc libgor2.RTC
	if libgor2.ReadRTC(&rtc) != nil || rtc.Year() < 2024 || rtc.Month < 1 || rtc.Month > 12 || rtc.Day < 1 || rtc.Day > 31 || rtc.Hours > 23 || rtc.Minutes > 59 || rtc.Seconds > 59 {
		return nil, errors.New("HTTPS needs a valid RTC date")
	}
	date := time.Date(int(rtc.Year()), time.Month(rtc.Month), int(rtc.Day), int(rtc.Hours), int(rtc.Minutes), int(rtc.Seconds), 0, time.UTC)
	seconds := date.Unix()
	days := seconds/86400 + 719528
	name := append([]byte(host), 0)
	eng := C.web_tls_new((*C.char)(unsafe.Pointer(&name[0])), 0, (*C.uchar)(unsafe.Pointer(&seed[0])), C.ulong(seedLen), C.ulong(days), C.ulong(seconds%86400))
	if eng == nil {
		return nil, errors.New("TLS context allocation failed")
	}
	ip, err := c.Stack.ResolveTCP(host, 15*time.Second)
	if err != nil {
		C.web_tls_free(eng)
		return nil, fmt.Errorf("DNS %s: %w", host, err)
	}
	tcp, err := c.Stack.Dial(ip, port, 15*time.Second)
	if err != nil {
		C.web_tls_free(eng)
		return nil, fmt.Errorf("TCP %s: %w", host, err)
	}
	// DNS and TCP each have their own deadline. Older CPUs also need time for
	// the initial TLS handshake; do not spend its budget before it starts.
	handshakeTimeout := timeout
	if handshakeTimeout < 60*time.Second {
		handshakeTimeout = 60 * time.Second
	}
	conn := &tlsConn{tcp: tcp, engine: eng, check: c.Check}
	conn.setTimeout(handshakeTimeout)
	for C.web_tls_established(eng) == 0 {
		if err := conn.step(); err != nil {
			conn.Close()
			return nil, fmt.Errorf("TLS handshake %s: %w", host, err)
		}
	}
	return conn, nil
}

func (c *tlsConn) setTimeout(timeout time.Duration) {
	c.idleMS = uint64(timeout.Milliseconds())
	c.deadline = libgor2.Ticks() + c.idleMS
}
func (c *tlsConn) progress() { c.deadline = libgor2.Ticks() + c.idleMS }
func (c *tlsConn) step() error {
	if c.check != nil {
		if e := c.check(); e != nil {
			return e
		}
	}
	now := libgor2.Ticks()
	if now >= c.deadline {
		received, gaps := c.tcp.ReceiveStats()
		return fmt.Errorf("%w (rx=%d gap=%d)", r2net.ErrTimeout, received, gaps)
	}
	c.tcp.SetDeadline(time.Duration(c.deadline-now) * time.Millisecond)
	state := uint(C.web_tls_state(c.engine))
	if state&1 != 0 {
		code := C.web_tls_error(c.engine)
		if code == 0 {
			return io.EOF
		}
		return errors.New("TLS: " + C.GoString(C.web_tls_error_text(code)))
	}
	var n C.ulong
	if state&2 != 0 {
		p := C.web_tls_sendrec_buf(c.engine, &n)
		sent, err := c.tcp.Write(unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)))
		if sent > 0 {
			C.web_tls_sendrec_ack(c.engine, C.ulong(sent))
			c.progress()
		}
		return err
	}
	if state&4 != 0 {
		p := C.web_tls_recvrec_buf(c.engine, &n)
		got, err := c.tcp.Read(unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)))
		if got > 0 {
			C.web_tls_recvrec_ack(c.engine, C.ulong(got))
			c.progress()
			return nil
		}
		if err == r2net.ErrClosed {
			return io.ErrUnexpectedEOF
		}
		if err == r2net.ErrTimeout {
			received, gaps := c.tcp.ReceiveStats()
			return fmt.Errorf("%w (rx=%d gap=%d)", err, received, gaps)
		}
		return err
	}
	return errors.New("TLS engine made no progress")
}

func (c *tlsConn) Write(b []byte) (int, error) {
	written := 0
	for len(b) > 0 {
		if uint(C.web_tls_state(c.engine))&8 != 0 {
			var n C.ulong
			p := C.web_tls_sendapp_buf(c.engine, &n)
			used := copy(unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)), b)
			C.web_tls_sendapp_ack(c.engine, C.ulong(used))
			C.web_tls_flush(c.engine)
			written += used
			b = b[used:]
			for uint(C.web_tls_state(c.engine))&2 != 0 {
				if err := c.step(); err != nil {
					return written, err
				}
			}
		} else if err := c.step(); err != nil {
			return written, err
		}
	}
	return written, nil
}
func (c *tlsConn) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	for {
		if uint(C.web_tls_state(c.engine))&16 != 0 {
			var n C.ulong
			p := C.web_tls_recvapp_buf(c.engine, &n)
			got := copy(b, unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)))
			C.web_tls_recvapp_ack(c.engine, C.ulong(got))
			return got, nil
		}
		if err := c.step(); err != nil {
			return 0, err
		}
	}
}
func (c *tlsConn) Close() { c.tcp.Close(); C.web_tls_free(c.engine) }

func (c *Client) Do(method, url string, headers map[string]string, body []byte) (wire.Response, error) {
	return c.request(method, url, headers, body, false, 60*time.Second)
}

// DoAudio keeps one framed HTTP connection alive between CDN ranges. This
// avoids DNS/TLS handshakes on every few seconds of audio. Stack ownership
// remains with the application's network worker.
func (c *Client) DoAudio(method, url string, headers map[string]string, body []byte) (wire.Response, error) {
	return c.request(method, url, headers, body, true, 30*time.Second)
}
func (c *Client) request(method, url string, headers map[string]string, body []byte, reuse bool, timeout time.Duration) (wire.Response, error) {
	var zero wire.Response
	u, err := r2net.ParseURL(url)
	if err != nil {
		return zero, err
	}
	if u.Scheme != "https" || !allowedHost(u.Host) || u.Port != 443 {
		return zero, errors.New("Spotify requests must use its HTTPS endpoints")
	}
	if method != "GET" && method != "POST" {
		return zero, errors.New("unsupported HTTP method")
	}
	if strings.ContainsAny(u.Path, "\r\n") {
		return zero, errors.New("invalid HTTP request path")
	}
	var req strings.Builder
	connection := "close"
	if reuse {
		connection = "keep-alive"
	}
	req.WriteString(method + " " + u.Path + " HTTP/1.1\r\nHost: " + u.Host + "\r\nAccept-Encoding: identity\r\nConnection: " + connection + "\r\nUser-Agent: r2spotify/0.2\r\n")
	if headers["Accept"] == "" {
		req.WriteString("Accept: application/json\r\n")
	}
	for k, v := range headers {
		if strings.ContainsAny(k, ":\r\n") || strings.ContainsAny(v, "\r\n") {
			return zero, errors.New("invalid HTTP header")
		}
		req.WriteString(k + ": " + v + "\r\n")
	}
	if method == "POST" {
		req.WriteString("Content-Length: " + strconv.Itoa(len(body)) + "\r\n")
	}
	req.WriteString("\r\n")
	if reuse && c.audio != nil && c.audioHost != u.Host {
		c.Close()
	}
	for attempt := 0; attempt < 2; attempt++ {
		conn := c.audio
		reused := reuse && conn != nil
		if !reused {
			conn, err = c.connect(u.Host, u.Port, timeout)
			if err != nil {
				return zero, err
			}
		}
		conn.setTimeout(timeout)
		_, err = conn.Write([]byte(req.String()))
		if err == nil && len(body) > 0 {
			_, err = conn.Write(body)
		}
		var response wire.Response
		if err == nil {
			response, err = wire.Read(conn)
		}
		if err == nil && reuse && response.Reusable() {
			c.audio = conn
			c.audioHost = u.Host
			return response, nil
		}
		conn.Close()
		if reuse {
			c.audio = nil
			c.audioHost = ""
		}
		if err == nil {
			return response, nil
		}
		// Retry a stale keep-alive connection once, only for idempotent GET.
		if !reused || method != "GET" || attempt != 0 {
			return zero, fmt.Errorf("HTTPS %s: %w", u.Host, err)
		}
	}
	return zero, err
}

func allowedHost(host string) bool {
	return host == "api.spotify.com" || host == "accounts.spotify.com" || strings.HasSuffix(host, ".spotify.com") || strings.HasSuffix(host, ".scdn.co") || host == "audio-ak-spotify-com.akamaized.net"
}
