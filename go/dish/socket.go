package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

// The socket, which is dish's word for a target endpoint, and the ways of
// getting a list of them.  The JSON is upstream's, unchanged: the same file
// and the same remote API feed a dish on Linux and a dish on r2.

type Result struct {
	Socket       Socket
	Passed       bool
	ResponseCode int
	Error        error
}

type SocketList struct {
	Sockets []Socket `json:"sockets"`
}

type Socket struct {
	// ID is a unique identifier of such socket.
	ID string `json:"id"`

	// Socket name, unique identificator, snake_cased.
	Name string `json:"socket_name"`

	// Remote endpoint hostname or URL.
	Host string `json:"host_name"`

	// Remote port to assemble a socket.
	Port int `json:"port_tcp"`

	// HTTP Status Codes expected when giving the endpoint a GET request.
	ExpectedHTTPCodes []int `json:"expected_http_code_array"`

	// HTTP Path to test on Host.
	PathHTTP string `json:"path_http"`
}

// printSockets lists what will be checked.
func printSockets(list *SocketList, log *logger) {
	log.Debug("loaded sockets:")

	for _, socket := range list.Sockets {
		log.Debugf("Host: %s, Port: %d, ExpectedHTTPCodes: %v", socket.Host, socket.Port, socket.ExpectedHTTPCodes)
	}
}

// loadSocketList decodes a JSON-encoded SocketList.
//
// Upstream streams this through a json.Decoder over an io.ReadCloser, which is
// the right shape when the source is a file handle or a response body.  Here
// both sources hand over a finished byte slice --- the kernel reads a whole
// file at a time, and r2net keeps a response in memory --- so there is nothing
// to stream and nothing to close.
func loadSocketList(raw []byte) (*SocketList, error) {
	list := new(SocketList)

	if err := json.Unmarshal(raw, list); err != nil {
		return nil, fmt.Errorf("error decoding sockets JSON: %w", err)
	}

	return list, nil
}

// fetchSocketList fetches the list of sockets to be checked.  The source is
// either a path to a file on a mounted filesystem or an http:// URL.
func fetchSocketList(cfg *Config, stack *r2net.Stack, log *logger) (*SocketList, error) {
	var (
		raw []byte
		err error
	)

	if isFilePath(cfg.Source) {
		raw, err = fetchSocketsFromFile(cfg, log)
	} else {
		raw, err = fetchSocketsFromRemote(cfg, stack, log)
	}

	if err != nil {
		return nil, err
	}

	return loadSocketList(raw)
}

// isFilePath reports whether the source is a path rather than a URL.
//
// Upstream compiles a regular expression for this.  Two prefix tests do the
// same job, and the regexp package is a hundred kilobytes of a two megabyte
// machine.
func isFilePath(source string) bool {
	lower := strings.ToLower(source)

	return !strings.HasPrefix(lower, "http://") && !strings.HasPrefix(lower, "https://")
}

func fetchSocketsFromFile(cfg *Config, log *logger) ([]byte, error) {
	log.Debugf("fetching sockets from file (%s)", cfg.Source)

	raw, err := libgor2.ReadFile(cfg.Source)
	if err != nil {
		return nil, fmt.Errorf("error reading %s: %w", cfg.Source, err)
	}

	if len(raw) == 0 {
		return nil, fmt.Errorf("%s is empty", cfg.Source)
	}

	return raw, nil
}

// fetchSocketsFromRemote loads the sockets to be monitored from a remote
// RESTful API endpoint.
//
// There is no cache here and no fallback to one: see the note on -cache in
// README.md.  A source that cannot be reached is a failed run, which the exit
// code reports.
func fetchSocketsFromRemote(cfg *Config, stack *r2net.Stack, log *logger) ([]byte, error) {
	header := map[string]string{}
	if cfg.ApiHeaderName != "" && cfg.ApiHeaderValue != "" {
		header[cfg.ApiHeaderName] = cfg.ApiHeaderValue
	}

	res, err := stack.Get(cfg.Source, header, time.Duration(cfg.TimeoutSeconds)*time.Second)
	if err != nil {
		return nil, fmt.Errorf("network request failed: %w", err)
	}

	if res.Status != 200 {
		return nil, fmt.Errorf("failed to fetch sockets from remote source --- got %d (%s)", res.Status, res.Reason)
	}

	log.Infof("socket list fetched from %s", cfg.Source)

	return res.Body, nil
}
