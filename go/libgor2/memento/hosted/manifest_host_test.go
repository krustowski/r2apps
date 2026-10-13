//go:build !r2

package hosted

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

func TestWriteManifest(t *testing.T) {
	options := Options{Magic: 0x31484f47, Title: "Hello / Go", Width: 480, Height: 240}
	var out bytes.Buffer
	if err := WriteManifest(&out, options, false, true); err != nil {
		t.Fatal(err)
	}
	var m struct {
		Magic   uint32 `json:"magic"`
		Version int    `json:"version"`
		Width   int    `json:"width"`
		Height  int    `json:"height"`
		Flags   uint32 `json:"flags"`
		Title   string `json:"title"`
	}
	if err := json.Unmarshal(out.Bytes(), &m); err != nil {
		t.Fatal(err)
	}
	if m.Magic != options.Magic || m.Version != Version || m.Width != options.Width ||
		m.Height != options.Height || m.Flags != 2 || m.Title != options.Title {
		t.Fatalf("manifest %+v", m)
	}
	out.Reset()
	if err := WriteManifest(&out, options, true, true); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), `"flags":3`) {
		t.Fatal("picture capability missing")
	}
}

func TestInvalidManifest(t *testing.T) {
	valid := Options{Magic: 1, Title: "App", Width: 32, Height: 24}
	for _, change := range []func(*Options){
		func(o *Options) { o.Magic = 0 },
		func(o *Options) { o.Width = 0 },
		func(o *Options) { o.Height = MaxDimension + 1 },
		func(o *Options) { o.Title = "" },
		func(o *Options) { o.Title = strings.Repeat("x", 80) },
		func(o *Options) { o.Title = "App\n" },
		func(o *Options) { o.Title = "App\x00" },
	} {
		options := valid
		change(&options)
		var out bytes.Buffer
		if WriteManifest(&out, options, false, false) == nil || out.Len() != 0 {
			t.Fatal("invalid metadata emitted")
		}
	}
}
