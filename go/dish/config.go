package main

import (
	"errors"
	"flag"
	"fmt"
	"strings"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

// Config holds the configuration parameters.
//
// The first half of it is upstream dish's, flag for flag, so that a command
// line written for one runs on the other.  The second half is what a machine
// with no resolver, no DHCP client and no routing table has to be told.
type Config struct {
	InstanceName         string
	ApiHeaderName        string
	ApiHeaderValue       string
	Source               string
	Verbose              bool
	PushgatewayURL       string
	TimeoutSeconds       uint
	ApiURL               string
	WebhookURL           string
	TextNotifySuccess    bool
	MachineNotifySuccess bool

	// Net is the link r2net should use: "eth" or "slip".
	Net string

	// LocalIP, Netmask, Gateway and DNS are this machine's idea of the
	// network.  Each one falls back to something sensible when it is not
	// given; see r2net.Options.
	LocalIP r2net.IP
	Netmask r2net.IP
	Gateway r2net.IP
	DNS     r2net.IP

	// SerialLog mirrors the log to COM1, which is how a headless run is
	// read from the host.  Not available on SLIP, where COM1 is the wire.
	SerialLog bool

	// NetDebug traces every packet.
	NetDebug bool

	// ConfigPath is the file the settings were read from, if any.
	ConfigPath string

	// unsupported collects what dish was asked for and will not do, to be
	// reported once the logger exists.
	unsupported []string
}

const (
	defaultInstanceName   = "generic-dish"
	defaultTimeoutSeconds = 10
	defaultNet            = "eth"
)

// errNoSourceProvided is returned when no source of sockets is specified.
var errNoSourceProvided = errors.New("no source provided")

// console is the io.Writer that puts bytes on the r2 console.
type console struct{}

func (console) Write(p []byte) (int, error) {
	fmt.Print(string(p))

	return len(p), nil
}

// newConfig parses args into a Config.
//
// The flags that dish upstream has and r2 cannot honour are still accepted, so
// that an existing command line does not die on an unknown flag --- but using
// one produces a warning rather than silence, because a monitoring tool that
// quietly does not alert is worse than one that refuses to start.
func newConfig(fs *flag.FlagSet, args []string) (*Config, error) {
	if fs == nil {
		return nil, fmt.Errorf("flagset argument cannot be nil")
	}

	// The flag package writes usage and parse errors to os.Stderr, which on
	// this target is a stub that leads nowhere.  Everything it has to say
	// goes through the console writer instead.
	fs.SetOutput(console{})

	cfg := &Config{
		InstanceName:   defaultInstanceName,
		TimeoutSeconds: defaultTimeoutSeconds,
		Net:            defaultNet,
	}

	var (
		source string

		localIP, netmask, gateway, dns string

		telegramBotToken, telegramChatID  string
		discordBotToken, discordChannelID string
		cacheSockets                      bool
		cacheDir                          string
		cacheTTL                          uint
	)

	// System flags
	fs.StringVar(&cfg.InstanceName, "name", defaultInstanceName, "a string, dish instance name")
	fs.UintVar(&cfg.TimeoutSeconds, "timeout", defaultTimeoutSeconds, "an int, timeout in seconds for http and tcp calls")
	fs.BoolVar(&cfg.Verbose, "verbose", false, "a bool, console logging toggle")

	// Integration channels flags
	fs.BoolVar(&cfg.TextNotifySuccess, "textNotifySuccess", false, "a bool, specifies whether successful checks with no failures should be reported to text channels")
	fs.BoolVar(&cfg.MachineNotifySuccess, "machineNotifySuccess", false, "a bool, specifies whether successful checks with no failures should be reported to machine channels")

	// API socket source
	fs.StringVar(&cfg.ApiHeaderName, "hname", "", "a string, name of a custom additional header to be used when fetching and pushing results to the remote API (used mainly for auth purposes)")
	fs.StringVar(&cfg.ApiHeaderValue, "hvalue", "", "a string, value of the custom additional header to be used when fetching and pushing results to the remote API (used mainly for auth purposes)")

	// Machine channels
	fs.StringVar(&cfg.PushgatewayURL, "target", "", "a string, result update path/URL to pushgateway, plaintext/byte output")
	fs.StringVar(&cfg.ApiURL, "updateURL", "", "a string, API endpoint URL for pushing results")
	fs.StringVar(&cfg.WebhookURL, "webhookURL", "", "a string, URL of webhook endpoint")

	// Accepted and refused: every one of these needs TLS, and r2 has none.
	fs.StringVar(&telegramBotToken, "telegramBotToken", "", "a string, Telegram bot private token (unsupported on r2: needs TLS)")
	fs.StringVar(&telegramChatID, "telegramChatID", "", "a string, Telegram chat/channel ID (unsupported on r2: needs TLS)")
	fs.StringVar(&discordBotToken, "discordBotToken", "", "a string, Discord bot token (unsupported on r2: needs TLS)")
	fs.StringVar(&discordChannelID, "discordChannelId", "", "a string, Discord channel ID (unsupported on r2: needs TLS)")

	// Accepted and ignored: caching the socket list needs a writable
	// filesystem and a modification time, and the ABI gives neither.
	fs.BoolVar(&cacheSockets, "cache", false, "a bool, cache the socket list fetched from the remote API source (unsupported on r2)")
	fs.StringVar(&cacheDir, "cacheDir", ".cache", "a string, directory used to cache the socket list (unsupported on r2)")
	fs.UintVar(&cacheTTL, "cacheTTL", 10, "an int, minutes for which the cached socket list is valid (unsupported on r2)")

	// r2-specific
	fs.StringVar(&cfg.ConfigPath, "config", "", "a string, path to a configuration file holding these same settings, one `key = value` per line")
	fs.StringVar(&source, "source", "", "a string, the socket list source, for when there is no room for a positional argument")
	fs.StringVar(&cfg.Net, "net", defaultNet, "a string, link to run the checks over: eth or slip")
	fs.StringVar(&localIP, "ip", "", "a string, this machine's IPv4 address (default: from the kernel, else 10.3.4.2)")
	fs.StringVar(&netmask, "mask", "", "a string, network mask deciding what is on the local link (default 255.255.255.0)")
	fs.StringVar(&gateway, "gw", "", "a string, gateway for destinations off the local link (default: the .1 of the local network)")
	fs.StringVar(&dns, "dns", "", "a string, DNS server to resolve host names with (default: none, addresses only)")
	fs.BoolVar(&cfg.SerialLog, "serialLog", false, "a bool, mirror the log to the serial port (eth only)")
	fs.BoolVar(&cfg.NetDebug, "netDebug", false, "a bool, trace every packet sent and received")

	// The file is applied first and the command line second, so a flag
	// given on the line overrides what the file said.
	if path := configPathFrom(args); path != "" {
		if err := applyConfigFile(fs, path); err != nil {
			return nil, err
		}

		cfg.ConfigPath = path
	}

	if err := fs.Parse(args); err != nil {
		return nil, fmt.Errorf("error parsing flags: %w", err)
	}

	switch cfg.Net {
	case "eth", "slip":
	default:
		return nil, fmt.Errorf("unknown link %q: use eth or slip", cfg.Net)
	}

	for _, a := range []struct {
		name  string
		value string
		dst   *r2net.IP
	}{
		{"ip", localIP, &cfg.LocalIP},
		{"mask", netmask, &cfg.Netmask},
		{"gw", gateway, &cfg.Gateway},
		{"dns", dns, &cfg.DNS},
	} {
		if a.value == "" {
			continue
		}

		ip, ok := r2net.ParseIP(a.value)
		if !ok {
			return nil, fmt.Errorf("-%s: %q is not an IPv4 address", a.name, a.value)
		}

		*a.dst = ip
	}

	// Collected here rather than warned about one by one at the point of
	// use, so that everything dish was asked for and will not do is said
	// before the first check runs.
	if telegramBotToken != "" || telegramChatID != "" {
		cfg.unsupported = append(cfg.unsupported, "Telegram alerting needs TLS, which r2 has not: -telegramBotToken/-telegramChatID ignored")
	}

	if discordBotToken != "" || discordChannelID != "" {
		cfg.unsupported = append(cfg.unsupported, "Discord alerting needs TLS, which r2 has not: -discordBotToken/-discordChannelId ignored")
	}

	if cacheSockets {
		cfg.unsupported = append(cfg.unsupported, "socket list caching is not implemented on r2: -cache ignored, the source is always fetched")
	}

	if cfg.SerialLog && cfg.Net == "slip" {
		cfg.SerialLog = false

		cfg.unsupported = append(cfg.unsupported, "-serialLog is not available on SLIP: the serial port is the network")
	}

	// A positional argument is how dish is invoked everywhere else, so it
	// wins; -source and the `source` key in a configuration file exist
	// because a command line here has room for eight tokens in total.
	cfg.Source = source

	if parsedArgs := fs.Args(); len(parsedArgs) > 0 {
		cfg.Source = parsedArgs[0]
	}

	if cfg.Source == "" {
		return nil, errNoSourceProvided
	}

	return cfg, nil
}

// configPathFrom finds -config in the raw arguments, before the flag package
// has looked at them, because what the file says has to be in place before the
// command line is parsed over the top of it.
func configPathFrom(args []string) string {
	for i, a := range args {
		switch {
		case a == "-config" || a == "--config":
			if i+1 < len(args) {
				return args[i+1]
			}

		case strings.HasPrefix(a, "-config="):
			return strings.TrimPrefix(a, "-config=")

		case strings.HasPrefix(a, "--config="):
			return strings.TrimPrefix(a, "--config=")
		}
	}

	return ""
}

// applyConfigFile reads `key = value` lines and sets the flag each key names.
//
// This exists because of a hard limit in the kernel: run_elf passes at most
// eight argv tokens to a program, and every `-flag value` pair costs two of
// them.  dish has more settings than that on any interesting run, so the
// settings have to be able to live in a file --- the same arrangement garn
// uses, and the same file format, so that both can be edited by someone who
// has seen one of them.
//
// Keys are flag names, values are what the flag would have been given, and a
// boolean takes 1 or 0 as well as true or false.
func applyConfigFile(fs *flag.FlagSet, path string) error {
	raw, err := libgor2.ReadFile(path)
	if err != nil {
		return fmt.Errorf("error reading config file %s: %w", path, err)
	}

	for n, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(strings.TrimRight(line, "\r"))

		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return fmt.Errorf("%s:%d: expected `key = value`, got %q", path, n+1, line)
		}

		key := strings.TrimSpace(line[:eq])
		value := strings.TrimSpace(line[eq+1:])

		if fs.Lookup(key) == nil {
			return fmt.Errorf("%s:%d: unknown setting %q", path, n+1, key)
		}

		if err := fs.Set(key, value); err != nil {
			return fmt.Errorf("%s:%d: %s: %w", path, n+1, key, err)
		}
	}

	return nil
}
