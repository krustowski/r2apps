package stream

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
)

const CredentialFileSize = 4096

// Credentials contains only renewable playback credentials, never a password.
// The fixed-size record overwrites trailing bytes on FAT's WriteFileAt API.
type Credentials struct {
	RefreshToken string `json:"refresh_token"`
	Device       string `json:"device_id,omitempty"`
}

func (c Credentials) valid() bool {
	if c.RefreshToken == "" || len(c.RefreshToken) > 2048 {
		return false
	}
	if c.Device != "" {
		b, e := hex.DecodeString(c.Device)
		if e != nil || len(b) != 20 {
			return false
		}
	}
	return true
}
func (c Credentials) Encode() ([]byte, error) {
	if !c.valid() {
		return nil, errors.New("invalid playback key")
	}
	b, e := json.Marshal(c)
	if e != nil || len(b) > CredentialFileSize {
		return nil, errors.New("playback key too large")
	}
	record := bytes.Repeat([]byte{' '}, CredentialFileSize)
	copy(record, b)
	return record, nil
}
func DecodeCredentials(b []byte) (Credentials, error) {
	var c Credentials
	if len(b) > CredentialFileSize || json.Unmarshal(b, &c) != nil || !c.valid() {
		return c, errors.New("invalid saved playback key")
	}
	return c, nil
}
