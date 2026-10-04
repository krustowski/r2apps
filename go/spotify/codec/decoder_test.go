package codec

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"github.com/krustowski/rou2exOS-apps/go/spotify/stream"
	"io"
	"os"
	"testing"
)

func TestIntegerVorbisDecode(t *testing.T) {
	data, e := os.ReadFile("testdata/tone.ogg")
	if e != nil {
		t.Fatal(e)
	}
	r, e := Open(bytes.NewReader(data))
	if e != nil {
		t.Fatal(e)
	}
	defer r.Close()
	if r.Rate != 44100 {
		t.Fatalf("rate %d", r.Rate)
	}
	var pcm [8192]byte
	frames, peak := 0, 0
	for {
		n, e := r.Read(pcm[:])
		if n%4 != 0 {
			t.Fatal("unaligned PCM")
		}
		frames += n / 4
		for i := 0; i < n; i += 2 {
			v := int(int16(binary.LittleEndian.Uint16(pcm[i:])))
			if v < 0 {
				v = -v
			}
			if v > peak {
				peak = v
			}
		}
		if e == io.EOF {
			break
		}
		if e != nil {
			t.Fatal(e)
		}
	}
	if frames < 10000 || frames > 12000 || peak < 100 {
		t.Fatalf("frames %d peak %d", frames, peak)
	}
	// Spotify's additional first Ogg page contains a 0x81 metadata packet.
	// Its contents must be skipped without shifting AES-CTR's source offset.
	var page [29]byte
	copy(page[:], "OggS")
	page[26] = 1
	page[27] = 1
	page[28] = 0x81
	r2, e := Open(bytes.NewReader(append(page[:], data...)))
	if e != nil {
		t.Fatal(e)
	}
	r2.Close()
}
func TestMalformedVorbis(t *testing.T) {
	if r, e := Open(bytes.NewReader(make([]byte, 100))); e == nil {
		r.Close()
		t.Fatal("invalid Ogg accepted")
	}
}

func TestEncryptedRangeToVorbisPCM(t *testing.T) {
	data, e := os.ReadFile("testdata/tone.ogg")
	if e != nil {
		t.Fatal(e)
	}
	plain, e := Open(bytes.NewReader(data))
	if e != nil {
		t.Fatal(e)
	}
	expected, e := io.ReadAll(plain)
	plain.Close()
	if e != nil {
		t.Fatal(e)
	}
	key := []byte("0123456789abcdef")
	block, e := aes.NewCipher(key)
	if e != nil {
		t.Fatal(e)
	}
	iv := []byte{0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93}
	// Include the additional Spotify metadata page in the encrypted offsets.
	page := make([]byte, 29)
	copy(page, "OggS")
	page[26] = 1
	page[27] = 1
	page[28] = 0x81
	encrypted := append(page, data...)
	cipher.NewCTR(block, iv).XORKeyStream(encrypted, encrypted)
	requests := 0
	source := &stream.CDNReader{URL: "https://audio.test.scdn.co/fixture", Total: -1, Do: func(method, url string, h map[string]string, body []byte) (wire.Response, error) {
		requests++
		if method != "GET" || h["Authorization"] != "" {
			t.Fatal("invalid audio range request")
		}
		var start, end int
		if _, err := fmt.Sscanf(h["Range"], "bytes=%d-%d", &start, &end); err != nil || end-start+1 != 4096 {
			t.Fatal("invalid startup range size")
		}
		if end >= len(encrypted) {
			end = len(encrypted) - 1
		}
		return wire.Response{Status: 206, Header: map[string]string{"content-range": fmt.Sprintf("bytes %d-%d/%d", start, end, len(encrypted))}, Body: append([]byte(nil), encrypted[start:end+1]...)}, nil
	}}
	decrypted, e := stream.NewDecryptor(source, key)
	if e != nil {
		t.Fatal(e)
	}
	decoder, e := Open(decrypted)
	if e != nil {
		t.Fatal(e)
	}
	defer decoder.Close()
	actual, e := io.ReadAll(decoder)
	if e != nil {
		t.Fatal(e)
	}
	if !bytes.Equal(actual, expected) || len(actual) == 0 || requests != 2 {
		t.Fatalf("pipeline mismatch: bytes=%d requests=%d", len(actual), requests)
	}
}

func TestDecoderCoalescesSmallPCMBlocks(t *testing.T) {
	data, e := os.ReadFile("testdata/tone.ogg")
	if e != nil {
		t.Fatal(e)
	}
	r, e := Open(bytes.NewReader(data))
	if e != nil {
		t.Fatal(e)
	}
	defer r.Close()
	var pcm [8192]byte
	if n, e := r.Read(pcm[:]); e != nil || n != len(pcm) {
		t.Fatalf("first PCM buffer: %d %v", n, e)
	}
}
