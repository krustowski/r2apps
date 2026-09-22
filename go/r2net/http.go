package r2net

import (
	"errors"
	"strconv"
	"strings"
	"time"
)

// An HTTP/1.0 client.
//
// HTTP/1.0 with an explicit Host header, rather than 1.1, and that is a
// deliberate choice: a 1.1 server may answer a 1.1 request with a chunked body
// or keep the connection open, and neither is worth the code here.  Asked in
// 1.0 it closes the connection when the response ends, which turns "read until
// the peer closes" into a complete answer and makes the framing free.
//
// TLS is not implemented and will not be: there is no entropy source, no big
// integer arithmetic worth the name and no room in a 2 MiB frame for a
// certificate chain.  An https:// URL is refused rather than silently fetched
// over plaintext.

// ErrTLS is returned for an https:// URL.
var ErrTLS = errors.New("https is not supported: r2 has no TLS")

// ErrBadURL is returned for a URL this client cannot make sense of.
var ErrBadURL = errors.New("malformed URL")

// maxBody is how much of a response body is kept.  A monitoring check wants
// the status line; an API wants its error message.  Neither wants a megabyte,
// and a megabyte is most of this machine's heap.
const maxBody = 16 * 1024

// URL is a parsed absolute URL, with only the parts this stack can act on.
type URL struct {
	Scheme string
	Host   string // name or dotted quad, without the port
	Port   uint16
	Path   string // path and query, always starting with '/'
}

// ParseURL picks apart an absolute http:// or https:// URL.
func ParseURL(raw string) (URL, error) {
	var u URL

	i := strings.Index(raw, "://")
	if i < 0 {
		return u, ErrBadURL
	}

	u.Scheme = strings.ToLower(raw[:i])

	rest := raw[i+3:]
	if rest == "" {
		return u, ErrBadURL
	}

	switch u.Scheme {
	case "http":
		u.Port = 80
	case "https":
		u.Port = 443
	default:
		return u, ErrBadURL
	}

	// Userinfo is not something this client can do anything with, and
	// leaving it in the host would send the request to a host that does not
	// exist.
	if at := strings.IndexByte(rest, '@'); at >= 0 {
		if slash := strings.IndexByte(rest, '/'); slash < 0 || at < slash {
			rest = rest[at+1:]
		}
	}

	authority := rest

	if j := strings.IndexAny(rest, "/?#"); j >= 0 {
		authority = rest[:j]

		u.Path = rest[j:]
		if u.Path[0] != '/' {
			u.Path = "/" + u.Path
		}
	}

	if u.Path == "" {
		u.Path = "/"
	}

	if k := strings.LastIndexByte(authority, ':'); k >= 0 {
		port, err := strconv.Atoi(authority[k+1:])
		if err != nil || port < 1 || port > 65535 {
			return u, ErrBadURL
		}

		u.Port = uint16(port)
		authority = authority[:k]
	}

	if authority == "" {
		return u, ErrBadURL
	}

	u.Host = authority

	return u, nil
}

// HostPort renders the authority the way a Host header wants it: the port is
// left out when it is the default for the scheme.
func (u URL) HostPort() string {
	if (u.Scheme == "http" && u.Port == 80) || (u.Scheme == "https" && u.Port == 443) {
		return u.Host
	}

	return u.Host + ":" + strconv.Itoa(int(u.Port))
}

// Request is one HTTP request.
type Request struct {
	Method string
	URL    string
	Body   []byte

	// ContentType is sent with a body, and defaults to application/json
	// when a body is given without one.
	ContentType string

	// Header holds any additional headers, which are sent as given.
	Header map[string]string

	// UserAgent overrides the default.
	UserAgent string
}

// Response is what came back.
type Response struct {
	Status int
	Reason string
	Header map[string]string // keys lower-cased
	Body   []byte
}

const defaultUserAgent = "r2net/1.0"

// Get fetches a URL.
func (s *Stack) Get(url string, header map[string]string, timeout time.Duration) (*Response, error) {
	return s.Do(Request{Method: "GET", URL: url, Header: header}, timeout)
}

// Do sends a request and reads the response.
//
// The timeout covers the whole exchange --- resolution, connection, request
// and response --- rather than any one part of it, which is what a checker
// with a deadline actually wants to bound.
func (s *Stack) Do(req Request, timeout time.Duration) (*Response, error) {
	deadline := deadlineFor(timeout)

	u, err := ParseURL(req.URL)
	if err != nil {
		return nil, err
	}

	if u.Scheme == "https" {
		return nil, ErrTLS
	}

	ip, err := s.Resolve(u.Host, remaining(deadline))
	if err != nil {
		return nil, err
	}

	conn, err := s.Dial(ip, u.Port, remaining(deadline))
	if err != nil {
		return nil, err
	}

	defer conn.Close()

	conn.deadline = deadline

	if _, err := conn.Write(buildRequest(req, u)); err != nil {
		return nil, err
	}

	return readResponse(conn)
}

// remaining is how much of a deadline is left, as a duration.
func remaining(deadline uint64) time.Duration {
	now := ticksNow()
	if deadline <= now {
		return time.Millisecond
	}

	return time.Duration(deadline-now) * time.Millisecond
}

// buildRequest renders the request head and body into one buffer, so that a
// small request goes out as a single segment.
func buildRequest(req Request, u URL) []byte {
	method := req.Method
	if method == "" {
		method = "GET"
	}

	agent := req.UserAgent
	if agent == "" {
		agent = defaultUserAgent
	}

	var b strings.Builder

	b.WriteString(method)
	b.WriteString(" ")
	b.WriteString(u.Path)
	b.WriteString(" HTTP/1.0\r\nHost: ")
	b.WriteString(u.HostPort())
	b.WriteString("\r\nUser-Agent: ")
	b.WriteString(agent)
	b.WriteString("\r\nAccept: */*\r\nConnection: close\r\n")

	if len(req.Body) > 0 {
		contentType := req.ContentType
		if contentType == "" {
			contentType = "application/json"
		}

		b.WriteString("Content-Type: ")
		b.WriteString(contentType)
		b.WriteString("\r\nContent-Length: ")
		b.WriteString(strconv.Itoa(len(req.Body)))
		b.WriteString("\r\n")
	}

	for k, v := range req.Header {
		b.WriteString(k)
		b.WriteString(": ")
		b.WriteString(v)
		b.WriteString("\r\n")
	}

	b.WriteString("\r\n")

	out := make([]byte, 0, b.Len()+len(req.Body))
	out = append(out, b.String()...)
	out = append(out, req.Body...)

	return out
}

// readResponse reads until the head is complete and the body has either
// reached its declared length or the peer has closed.
func readResponse(conn *Conn) (*Response, error) {
	var (
		raw  []byte
		buf  [2048]byte
		head int = -1
	)

	for {
		n, err := conn.Read(buf[:])
		if n > 0 {
			raw = append(raw, buf[:n]...)

			if head < 0 {
				head = headerEnd(raw)
			}

			if head >= 0 {
				res, done, err := parseResponse(raw, head)
				if err != nil {
					return nil, err
				}

				if done {
					return res, nil
				}
			}

			if len(raw) >= maxBody+8192 {
				// Long past anything worth keeping.  Whatever
				// this is, the answer to it is already known.
				break
			}

			continue
		}

		if err != nil {
			// The peer closing is how an HTTP/1.0 response ends, so
			// it is a result and not a failure --- provided a
			// response actually arrived.
			if errors.Is(err, ErrClosed) {
				break
			}

			return nil, err
		}
	}

	if head < 0 {
		head = headerEnd(raw)
	}

	if head < 0 {
		return nil, errors.New("truncated response head")
	}

	res, _, err := parseResponse(raw, head)

	return res, err
}

// headerEnd finds the blank line that ends the response head, tolerating a
// server that uses bare newlines.
func headerEnd(b []byte) int {
	for i := 0; i+1 < len(b); i++ {
		if b[i] == '\n' && b[i+1] == '\n' {
			return i + 2
		}

		if i+3 < len(b) && b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n' {
			return i + 4
		}
	}

	return -1
}

// parseResponse turns what has arrived so far into a Response, and says
// whether the body is complete.
func parseResponse(raw []byte, head int) (*Response, bool, error) {
	lines := strings.Split(strings.TrimRight(string(raw[:head]), "\r\n"), "\n")
	if len(lines) == 0 {
		return nil, false, errors.New("empty response")
	}

	status, reason, err := parseStatusLine(strings.TrimRight(lines[0], "\r"))
	if err != nil {
		return nil, false, err
	}

	res := &Response{
		Status: status,
		Reason: reason,
		Header: make(map[string]string, len(lines)),
	}

	for _, line := range lines[1:] {
		line = strings.TrimRight(line, "\r")

		colon := strings.IndexByte(line, ':')
		if colon <= 0 {
			continue
		}

		key := strings.ToLower(strings.TrimSpace(line[:colon]))
		res.Header[key] = strings.TrimSpace(line[colon+1:])
	}

	body := raw[head:]
	if len(body) > maxBody {
		body = body[:maxBody]
	}

	res.Body = body

	// A declared length is the only way to know a response is complete
	// without waiting for the close, and waiting for the close costs a
	// round trip on every check.
	if v, ok := res.Header["content-length"]; ok {
		length, err := strconv.Atoi(v)
		if err == nil && (len(raw)-head >= length || length > maxBody) {
			return res, true, nil
		}
	}

	return res, false, nil
}

// parseStatusLine reads "HTTP/1.x CODE REASON".
func parseStatusLine(line string) (int, string, error) {
	if !strings.HasPrefix(line, "HTTP/") {
		return 0, "", errors.New("not an HTTP response: " + shorten(line))
	}

	sp := strings.IndexByte(line, ' ')
	if sp < 0 {
		return 0, "", errors.New("malformed status line")
	}

	rest := strings.TrimLeft(line[sp+1:], " ")

	code := rest
	reason := ""

	if sp2 := strings.IndexByte(rest, ' '); sp2 >= 0 {
		code = rest[:sp2]
		reason = strings.TrimSpace(rest[sp2+1:])
	}

	status, err := strconv.Atoi(code)
	if err != nil || status < 100 || status > 599 {
		return 0, "", errors.New("malformed status code")
	}

	return status, reason, nil
}

func shorten(s string) string {
	if len(s) > 40 {
		return s[:40] + "..."
	}

	return s
}
