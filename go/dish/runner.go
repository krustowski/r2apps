package main

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

// The checks themselves.
//
// Upstream starts a goroutine per socket and fans the results back in over a
// channel, which is the right way round on a machine with threads and a kernel
// socket layer.  Here the whole TCP/IP stack is one cooperatively scheduled
// object in this process: a goroutine costs 32 KiB of stack committed before it
// runs once, the kernel hands over one frame at a time through a single shared
// buffer, and nothing in r2net is re-entrant.  So the checks run one after
// another.  The cost is wall-clock time on a list where several hosts are down
// --- each one waits out its own timeout --- and the gain is that the result of
// a run is the same every time it runs.

const agentVersion = "1.12"

// testResults holds the overall results of all socket checks combined.
type testResults struct {
	messengerText string
	results       map[string]bool
	failedCount   int
}

// runTests orchestrates the checking of a list of sockets: it fetches the
// list, runs the checks, collects the results and returns them.
func runTests(cfg *Config, stack *r2net.Stack, log *logger) (*testResults, error) {
	list, err := fetchSocketList(cfg, stack, log)
	if err != nil {
		return nil, fmt.Errorf("error loading socket list: %w", err)
	}

	if cfg.Verbose {
		printSockets(list, log)
	}

	out := &testResults{results: make(map[string]bool, len(list.Sockets))}

	timeout := time.Duration(cfg.TimeoutSeconds) * time.Second

	for _, sock := range list.Sockets {
		result := runSocketTest(sock, stack, log, timeout)

		if !result.Passed || result.Error != nil {
			out.failedCount++
		}

		if !result.Passed || cfg.TextNotifySuccess {
			out.messengerText += formatMessengerText(result)
		}

		out.results[result.Socket.ID] = result.Passed
	}

	return out, nil
}

// runSocketTest picks a runner for the socket and runs it.
//
// The rules are upstream's, and the first matching one applies:
//   - a Host starting with http:// or https:// is checked over HTTP;
//   - a Port between 1 and 65535 is checked by opening a TCP connection;
//   - a non-empty Host is checked with an ICMP echo request;
//   - anything else is a socket that cannot be checked at all.
func runSocketTest(sock Socket, stack *r2net.Stack, log *logger, timeout time.Duration) Result {
	switch {
	case hasHTTPScheme(sock.Host):
		return httpTest(sock, stack, log, timeout)

	case sock.Port >= 1 && sock.Port <= 65535:
		return tcpTest(sock, stack, log, timeout)

	case sock.Host != "":
		return icmpTest(sock, stack, log, timeout)

	default:
		err := fmt.Errorf("no protocol could be determined from the socket %s", sock.ID)

		log.Errorf("failed to test socket: %v", err)

		return Result{Socket: sock, Error: err}
	}
}

func hasHTTPScheme(host string) bool {
	lower := strings.ToLower(host)

	return strings.HasPrefix(lower, "http://") || strings.HasPrefix(lower, "https://")
}

// httpTest sends a GET request and checks the status against the expected
// codes.
func httpTest(sock Socket, stack *r2net.Stack, log *logger, timeout time.Duration) Result {
	url := socketURL(sock)

	log.Debug("HTTP runner: connect: ", url)

	res, err := stack.Do(r2net.Request{
		Method:    "GET",
		URL:       url,
		UserAgent: "dish/" + agentVersion,
	}, timeout)
	if err != nil {
		return Result{Socket: sock, Error: err}
	}

	passed := containsInt(sock.ExpectedHTTPCodes, res.Status)
	if !passed {
		err = fmt.Errorf("expected codes: %v, got %d", sock.ExpectedHTTPCodes, res.Status)
	}

	return Result{
		Socket:       sock,
		Passed:       passed,
		ResponseCode: res.Status,
		Error:        err,
	}
}

// socketURL assembles the URL to request, the way upstream does: the host
// already carries the scheme, the port is appended to it, and the path follows.
//
// A port of zero is left off rather than sent as ":0", which no URL parser
// accepts --- upstream never meets the case because a socket with an http
// scheme and no port is unusual, but a config written for the ICMP check and
// then given a scheme produces exactly that.
func socketURL(sock Socket) string {
	url := sock.Host

	if sock.Port > 0 {
		url += ":" + strconv.Itoa(sock.Port)
	}

	return url + sock.PathHTTP
}

// tcpTest opens a TCP connection.  The check passes if the connection is
// established, and nothing is sent on it.
func tcpTest(sock Socket, stack *r2net.Stack, log *logger, timeout time.Duration) Result {
	endpoint := sock.Host + ":" + strconv.Itoa(sock.Port)

	log.Debug("TCP runner: connect: " + endpoint)

	ip, err := stack.Resolve(sock.Host, timeout)
	if err != nil {
		return Result{Socket: sock, Error: resolveError(sock.Host, err)}
	}

	conn, err := stack.Dial(ip, uint16(sock.Port), timeout)
	if err != nil {
		return Result{Socket: sock, Error: err}
	}

	if err := conn.Close(); err != nil {
		log.Errorf("failed to close TCP connection to %s: %v", endpoint, err)
	}

	return Result{Socket: sock, Passed: true}
}

// icmpTest sends one echo request and waits for the reply.
func icmpTest(sock Socket, stack *r2net.Stack, log *logger, timeout time.Duration) Result {
	ip, err := stack.Resolve(sock.Host, timeout)
	if err != nil {
		return Result{Socket: sock, Error: resolveError(sock.Host, err)}
	}

	log.Debug("ICMP runner: send to " + ip.String())

	rtt, err := stack.Ping(ip, timeout)
	if err != nil {
		return Result{Socket: sock, Error: err}
	}

	log.Debugf("ICMP runner: reply from %v in %v", ip, rtt)

	return Result{Socket: sock, Passed: true}
}

// resolveError says which of the two reasons a name did not resolve for, since
// one of them is a fact about this machine rather than about the target.
func resolveError(host string, err error) error {
	if errors.Is(err, r2net.ErrNoServer) {
		return fmt.Errorf("cannot resolve %s: no DNS server configured (use -dns, or give an address)", host)
	}

	return fmt.Errorf("failed to resolve socket host: %w", err)
}

func containsInt(haystack []int, needle int) bool {
	for _, v := range haystack {
		if v == needle {
			return true
		}
	}

	return false
}
