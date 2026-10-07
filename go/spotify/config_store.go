package main

import (
	"encoding/json"
	"fmt"
	"sync"

	"github.com/krustowski/rou2exOS-apps/go/spotify/model"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
)

const (
	configPath          = "/mnt/fat/SPOTIFY.CFG"
	temporaryConfigPath = "/mnt/tmp/SPOTIFY.CFG"
	playbackKeyPath     = "/mnt/tmp/SPOTIFY.KEY" // legacy cache, imported on startup
	configFileSize      = 64 * 1024
)

// The UI owns selection state; the network worker owns its API configuration.
// Serialize writes and merge credentials separately so neither can overwrite
// the other's latest changes while a filesystem syscall yields.
type configStore struct {
	mu                     sync.Mutex
	config                 model.Config
	fromFAT                bool
	temporarySize, fatSize int
	write                  func(string, []byte) (int, error)
}

func (s *configStore) load(read func(string, int) ([]byte, error)) string {
	status := ""
	for _, path := range []string{configPath, "/mnt/tar/opt/spotify/config.josn", "/mnt/tar/opt/spotify/config.json", "/mnt/iso/opt/spotify/config.josn", "/mnt/iso/opt/spotify/config.json"} {
		b, err := read(path, configFileSize)
		if err != nil {
			continue
		}
		c, err := model.ParseConfig(b)
		if err != nil {
			status = err.Error()
			continue
		}
		s.config = c
		s.fromFAT = path == configPath
		if s.fromFAT {
			s.fatSize = len(b)
		}
		break
	}
	temporary := false
	if b, err := read(temporaryConfigPath, configFileSize); err == nil {
		s.temporarySize = len(b)
		if c, err := model.ParseConfig(b); err == nil {
			s.config = c
			temporary = true
		} else {
			status = "Invalid temporary Spotify configuration; using boot configuration."
		}
	}
	// Import the former separate playback cache. A token already saved in the
	// temporary config takes precedence over a leftover legacy key.
	if !temporary || s.config.StreamingRefreshToken == "" {
		if b, err := read(playbackKeyPath, stream.CredentialFileSize); err == nil {
			if key, err := stream.DecodeCredentials(b); err == nil {
				s.config.StreamingAccessToken = ""
				s.config.StreamingRefreshToken = key.RefreshToken
				s.config.StreamingDeviceID = key.Device
			}
		}
	}
	return status
}

func (s *configStore) writeConfig(path string, c model.Config) error {
	b, err := json.Marshal(c)
	if err != nil {
		return err
	}
	if len(b) > configFileSize {
		return fmt.Errorf("Spotify configuration exceeds 64 KiB")
	}
	// WriteFileAt does not truncate. Padding prevents stale JSON or token bytes
	// surviving when a replacement record is shorter.
	size := &s.temporarySize
	if path == configPath {
		size = &s.fatSize
	}
	if len(b) > *size {
		*size = len(b)
	}
	record := make([]byte, *size)
	copy(record, b)
	for i := len(b); i < len(record); i++ {
		record[i] = ' '
	}
	n, err := s.write(path, record)
	if err != nil || n != len(record) {
		return fmt.Errorf("cannot save Spotify configuration to %s", path)
	}
	return nil
}

func (s *configStore) saveSession(session model.SessionState) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.config.Session = &session
	return s.writeConfig(temporaryConfigPath, s.config)
}

func (s *configStore) saveCredentials(c model.Config) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.config.RefreshToken = c.RefreshToken
	s.config.StreamingAccessToken = c.StreamingAccessToken
	s.config.StreamingRefreshToken = c.StreamingRefreshToken
	s.config.StreamingDeviceID = c.StreamingDeviceID
	tmpErr := s.writeConfig(temporaryConfigPath, s.config)
	if s.fromFAT {
		fat := s.config
		fat.Session = nil
		if err := s.writeConfig(configPath, fat); err != nil {
			return err
		}
	}
	return tmpErr
}
