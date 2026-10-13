//go:build !r2

package hosted

import (
	"encoding/json"
	"errors"
	"io"
)

// WriteManifest emits build-time window metadata from the same Options used
// by Run. Feed it to tools/append_manifest.py after building the ELF, so
// Memento's Files window can launch the app without a desktop adapter.
// This helper is host-only and adds no JSON code to the r2 executable.
func WriteManifest(out io.Writer, options Options, pictures, notifications bool) error {
	if options.Width <= 0 || options.Height <= 0 || options.Width > MaxDimension || options.Height > MaxDimension {
		return ErrWindowSize
	}
	if options.Magic == 0 || len(options.Title) == 0 || len(options.Title) >= 80 {
		return errors.New("hosted: manifest needs a magic and a title of 1..79 ASCII bytes")
	}
	for _, ch := range options.Title {
		if ch < 32 || ch > 126 {
			return errors.New("hosted: manifest title must be printable ASCII")
		}
	}
	flags := uint32(0)
	if pictures {
		flags |= 1
	}
	if notifications {
		flags |= 2
	}
	return json.NewEncoder(out).Encode(struct {
		Magic   uint32 `json:"magic"`
		Version uint32 `json:"version"`
		Width   int    `json:"width"`
		Height  int    `json:"height"`
		Flags   uint32 `json:"flags"`
		Title   string `json:"title"`
	}{options.Magic, Version, options.Width, options.Height, flags, options.Title})
}
