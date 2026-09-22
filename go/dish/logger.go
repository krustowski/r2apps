package main

import (
	"fmt"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// A levelled console logger, the same shape as upstream dish's.
//
// Two things are different, and both come from the machine.  There is no
// colour: the kernel's console writes bytes to a VGA text buffer and does not
// interpret ANSI escapes, so the sequences upstream emits would appear as
// literal rubbish on screen.  And the timestamp comes from the RTC rather than
// from the log package, because r2's clock starts at boot and a log line
// stamped "00:00:07" says nothing about when the run happened.

type logLevel int32

const (
	levelTrace logLevel = iota
	levelDebug
	levelInfo
	levelWarn
	levelError
)

var logLabel = map[logLevel]string{
	levelTrace: "TRACE",
	levelDebug: "DEBUG",
	levelInfo:  "INFO",
	levelWarn:  "WARN",
	levelError: "ERROR",
}

type logger struct {
	level  logLevel
	serial bool
}

// newLogger returns a logger at TRACE when dish was started with -verbose and
// at INFO otherwise, matching upstream.
func newLogger(cfg *Config) *logger {
	l := &logger{level: levelInfo, serial: cfg.SerialLog}

	if cfg.Verbose {
		l.level = levelTrace
	}

	if l.serial {
		// The kernel does not bring COM1 up for us unless it was built
		// with serial debugging on.
		_ = libgor2.SerialInit()
	}

	for _, msg := range cfg.unsupported {
		l.Warn(msg)
	}

	return l
}

// log prints a message when the level allows it.
func (l *logger) log(level logLevel, format string, v ...any) {
	if l.level > level {
		return
	}

	msg := fmt.Sprint(v...)
	if format != "" {
		msg = fmt.Sprintf(format, v...)
	}

	line := timestamp() + " [ " + logLabel[level] + " ]: " + msg + "\n"

	fmt.Print(line)

	if l.serial {
		l.mirror(line)
	}
}

// mirror puts the line on the serial port as well, which is the only way a
// machine with no screen attached reports anything.
func (l *logger) mirror(line string) {
	for i := 0; i < len(line); i++ {
		if line[i] == '\n' {
			_ = libgor2.SerialWrite('\r')
		}

		_ = libgor2.SerialWrite(line[i])
	}
}

// packetTracer is what r2net traces through when -netDebug is set.  Each line
// goes out at TRACE level, so -netDebug without -verbose is quiet: the two
// switches are independent on purpose, since a packet trace is almost always
// wanted with the rest of the log rather than instead of it.
func (l *logger) packetTracer(cfg *Config) func(string) {
	if !cfg.NetDebug {
		return nil
	}

	return func(line string) {
		l.log(levelTrace, "", "r2net: ", line)
	}
}

// timestamp reads the wall clock.  A run that cannot read it still logs.
func timestamp() string {
	var t libgor2.RTC
	if err := libgor2.ReadRTC(&t); err != nil {
		return "--:--:--"
	}

	return fmt.Sprintf("%02d:%02d:%02d", t.Hours, t.Minutes, t.Seconds)
}

func (l *logger) Trace(v ...any)            { l.log(levelTrace, "", v...) }
func (l *logger) Tracef(f string, v ...any) { l.log(levelTrace, f, v...) }
func (l *logger) Debug(v ...any)            { l.log(levelDebug, "", v...) }
func (l *logger) Debugf(f string, v ...any) { l.log(levelDebug, f, v...) }
func (l *logger) Info(v ...any)             { l.log(levelInfo, "", v...) }
func (l *logger) Infof(f string, v ...any)  { l.log(levelInfo, f, v...) }
func (l *logger) Warn(v ...any)             { l.log(levelWarn, "", v...) }
func (l *logger) Warnf(f string, v ...any)  { l.log(levelWarn, f, v...) }
func (l *logger) Error(v ...any)            { l.log(levelError, "", v...) }
func (l *logger) Errorf(f string, v ...any) { l.log(levelError, f, v...) }
