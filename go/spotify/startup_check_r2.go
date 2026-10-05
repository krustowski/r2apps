//go:build r2 && r2netcheck

package main

import (
	"bytes"
	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/r2net"
	"github.com/krustowski/rou2exOS-apps/go/spotify/codec"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
	"io"
	"runtime"
)

func startupChecks() string {
	if e := r2net.NativeTCPCheck(); e != nil {
		return "REGRESSION FAILED: " + e.Error()
	}
	codebook := make(chan bool, 1)
	go func() { codebook <- codec.NativeCodebookCheck() }()
	if !<-codebook {
		return "REGRESSION FAILED: large Vorbis codebook"
	}
	diagnosticStage("native decoder stress")
	fixture := make([]byte, 16384)
	n, err := r2.ReadFileAt("/mnt/tar/opt/music/stress.ogg", fixture, 0)
	if err != nil || n == 0 {
		return "REGRESSION FAILED: decoder fixture missing"
	}
	fixture = fixture[:n]
	baseline, _ := codec.MemoryStats()
	var pcm [8192]byte
	for cycle := 0; cycle < 100; cycle++ {
		decoder, err := codec.Open(bytes.NewReader(fixture))
		if err != nil {
			return "REGRESSION FAILED: decoder reopen"
		}
		if cycle%2 == 0 {
			for {
				_, err = decoder.Read(pcm[:])
				if err != nil {
					break
				}
			}
			if err != io.EOF {
				decoder.Close()
				return "REGRESSION FAILED: decoder read"
			}
		}
		decoder.Close()
		decoder.Close()
		decoder = nil
		runtime.GC()
		if used, _ := codec.MemoryStats(); used != baseline {
			return "REGRESSION FAILED: decoder leaked"
		}
	}
	diagnosticStage("100 native decoder cycles passed")
	key := stream.Credentials{RefreshToken: "native-test-key", Device: "0123456789012345678901234567890123456789"}
	data, e := key.Encode()
	if e != nil {
		return "Key encoding failed"
	}
	if n, e := r2.WriteFileAt("/mnt/tmp/STEST.KEY", data, 0); e != nil || n != len(data) {
		return "TCP passed; tmp key write failed"
	}
	// Overwrite a longer record, then reopen it with a fresh read buffer.
	key.RefreshToken = "short"
	data, _ = key.Encode()
	r2.WriteFileAt("/mnt/tmp/STEST.KEY", data, 0)
	read := make([]byte, stream.CredentialFileSize)
	n, e = r2.ReadFileAt("/mnt/tmp/STEST.KEY", read, 0)
	if e != nil || n != len(read) {
		return "TCP passed; tmp key read failed"
	}
	restored, e := stream.DecodeCredentials(read[:n])
	if e != nil || restored != key {
		return "TCP passed; tmp key reopen failed"
	}
	return "TCP + Vorbis book + 100 decoder cycles + tmp key passed."
}
