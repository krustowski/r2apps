// dish is a one-shot monitoring service: it reads a list of sockets, checks
// every one of them over HTTP, TCP or ICMP, and reports what it found to
// whichever channels are configured.
//
// This is go.vxn.dev/dish carried over to rou2exOS.  The socket schema, the
// flags, the exit codes and the results payload are the ones upstream uses, so
// the same configuration file and the same remote API serve both.  What is
// different is underneath: there is no net package on r2 and no TLS, so the
// checks run on r2net --- this repository's own TCP/IP stack --- and the
// channels that need HTTPS cannot be reached at all.  See README.md.
//
//	dish [FLAGS] SOURCE
package main

import (
	"errors"
	"flag"
	"fmt"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

// Exit codes, the same ones upstream uses.  A shell script that switches on
// them does not have to care which machine dish ran on.
const (
	exitOK           = 0
	exitNoSource     = 1
	exitBadFlags     = 2
	exitTestsFailed  = 3
	exitSocketFailed = 4
)

// maxArgv is the kernel's limit on how many space-separated tokens reach a
// program: push_user_args in the kernel's elf loader stops at eight, argv[0]
// included.  It is why -config exists.
const maxArgv = 8

func main() {
	libgor2.Exit(run(libgor2.Args()))
}

// run is everything main does, with the exit code as its result so that the
// whole program can be read in one place.
func run(argv []string) int {
	args := []string(nil)
	if len(argv) > 1 {
		args = argv[1:]
	}

	cfg, err := newConfig(flag.NewFlagSet("dish", flag.ContinueOnError), args)
	if err != nil {
		// -h is a request, not a failure: the flag package has already
		// listed everything by the time the error comes back.
		if errors.Is(err, flag.ErrHelp) {
			return exitOK
		}

		if errors.Is(err, errNoSourceProvided) {
			printHelp()

			return exitNoSource
		}

		fmt.Printf("error loading config: %v\n", err)

		return exitBadFlags
	}

	log := newLogger(cfg)
	log.Info("dish run: started")

	if len(argv) >= maxArgv {
		// run_elf stops after eight tokens and says nothing, so a
		// command line that was one flag too long simply loses its
		// tail.  This is the only place that can notice.
		log.Warnf("the kernel passes at most %d arguments to a program and this run used all of them: anything after %q was dropped --- use -config", maxArgv, argv[maxArgv-1])
	}

	if cfg.ConfigPath != "" {
		log.Debugf("settings read from %s", cfg.ConfigPath)
	}

	stack, err := r2net.Open(r2net.Options{
		Link:    cfg.Net,
		LocalIP: cfg.LocalIP,
		Netmask: cfg.Netmask,
		Gateway: cfg.Gateway,
		DNS:     cfg.DNS,
		Trace:   log.packetTracer(cfg),
	})
	if err != nil {
		log.Errorf("error bringing up the network on %s: %v", cfg.Net, err)

		return exitTestsFailed
	}

	defer stack.Close()

	log.Debugf("network: link %s, address %v, gateway %v, icmp %v",
		stack.Link(), stack.LocalIP(), cfg.Gateway, stack.CanICMP())

	if !stack.CanICMP() {
		// Worth saying once, up front, rather than once per check: on
		// this machine the reason is never the host being tested.
		log.Warn("another process holds the Ethernet driver: ICMP checks and name resolution are unavailable")
	}

	res, err := runTests(cfg, stack, log)
	if err != nil {
		log.Error(err)

		return exitTestsFailed
	}

	newAlerter(cfg, stack, log).handleAlerts(res)

	if res.failedCount > 0 {
		log.Warn("dish run: some tests failed:\n", res.messengerText)

		return exitSocketFailed
	}

	log.Info("dish run: all tests ok")

	return exitOK
}

func printHelp() {
	fmt.Print("Usage: dish [FLAGS] SOURCE\n\n")
	fmt.Print("A lightweight, one-shot socket checker\n\n")
	fmt.Print("SOURCE must be a path to a JSON file with a list of sockets to be checked\n")
	fmt.Print("(e.g. /mnt/fat/DISH/SOCKETS.JSN) or an http:// URL leading to a remote JSON\n")
	fmt.Print("API from which that list can be retrieved\n\n")
	fmt.Print("The kernel passes at most 8 arguments to a program, which is not many when\n")
	fmt.Print("every setting is a flag and a value, so the same settings can be put in a\n")
	fmt.Print("file instead, one `key = value` per line:\n\n")
	fmt.Print("    fg DISH -config /mnt/fat/DISH/DISH.CFG\n\n")
	fmt.Print("Use the `-h` flag for a list of available flags\n")
}
