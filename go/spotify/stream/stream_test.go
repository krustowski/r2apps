package stream

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"github.com/krustowski/rou2exOS-apps/go/spotify/third_party/shannon"
	"io"
	"strconv"
	"strings"
	"testing"
)

func TestProtoBounds(t *testing.T) {
	b := join(number(10, 300), blob(50, blob(10, []byte("nested"))))
	m := parse(b)
	if m.err != nil || m.num(10) != 300 || string(m.sub(50).bytes(10)) != "nested" {
		t.Fatal("protobuf roundtrip")
	}
	for _, b := range [][]byte{{0}, {0x80}, {10, 100, 1}, {11}, {8, 0x80}} {
		if parse(b).err == nil {
			t.Fatalf("accepted malformed protobuf %x", b)
		}
	}
}
func TestPacketAuthenticationAndNonces(t *testing.T) {
	var network bytes.Buffer
	key := bytes.Repeat([]byte{7}, 32)
	tx := packetConn{rw: &network, send: shannon.New(key)}
	if e := tx.write(0xab, []byte("first")); e != nil {
		t.Fatal(e)
	}
	if e := tx.write(0x0c, []byte("second")); e != nil {
		t.Fatal(e)
	}
	rx := packetConn{rw: &network, recv: shannon.New(key)}
	for i, want := range []string{"first", "second"} {
		kind, p, e := rx.read()
		if e != nil || string(p) != want || (i == 0 && kind != 0xab) || (i == 1 && kind != 0x0c) {
			t.Fatalf("packet %d %x %q %v", i, kind, p, e)
		}
	}
	network.Reset()
	tx = packetConn{rw: &network, send: shannon.New(key)}
	tx.write(0x0d, bytes.Repeat([]byte{2}, 20))
	network.Bytes()[network.Len()-1] ^= 1
	rx = packetConn{rw: &network, recv: shannon.New(key)}
	if _, _, e := rx.read(); e == nil {
		t.Fatal("corrupt MAC accepted")
	}
}
func TestRangedAudioDecryption(t *testing.T) {
	key := bytes.Repeat([]byte{0x42}, 16)
	plain := make([]byte, ChunkSize*2+123)
	for i := range plain {
		plain[i] = byte(i*13 + 5)
	}
	encrypted := append([]byte(nil), plain...)
	block, _ := aes.NewCipher(key)
	cipher.NewCTR(block, audioIV[:]).XORKeyStream(encrypted, encrypted)
	calls := 0
	source := &CDNReader{URL: "https://audio4-fa.scdn.co/audio/fixture", Total: -1, Do: func(method, url string, h map[string]string, body []byte) (wire.Response, error) {
		calls++
		if method != "GET" || h["Authorization"] != "" {
			t.Fatal("bad CDN request")
		}
		span := strings.Split(strings.TrimPrefix(h["Range"], "bytes="), "-")
		start, _ := strconv.Atoi(span[0])
		end, _ := strconv.Atoi(span[1])
		if end >= len(encrypted) {
			end = len(encrypted) - 1
		}
		return wire.Response{Status: 206, Header: map[string]string{"content-range": fmt.Sprintf("bytes %d-%d/%d", start, end, len(encrypted))}, Body: append([]byte(nil), encrypted[start:end+1]...)}, nil
	}}
	d, e := NewDecryptor(source, key)
	if e != nil {
		t.Fatal(e)
	}
	for _, offset := range []int64{0, 1, 15, 16, 17, ChunkSize - 5, ChunkSize + 11, int64(len(plain) - 4)} {
		out := make([]byte, 37)
		n, e := d.ReadAt(out, offset)
		want := 37
		if int(offset)+want > len(plain) {
			want = len(plain) - int(offset)
			if e != io.EOF {
				t.Fatal("missing EOF")
			}
		} else if e != nil {
			t.Fatal(e)
		}
		if n != want || !bytes.Equal(out[:n], plain[offset:int(offset)+n]) {
			t.Fatalf("CTR offset %d mismatch", offset)
		}
	}
	if calls < 2 {
		t.Fatal("ranges not exercised")
	}
	if _, e := d.ReadAt(make([]byte, 1), -1); e == nil {
		t.Fatal("negative offset accepted")
	}
}
func TestRejectInvalidRangesAndHosts(t *testing.T) {
	for _, s := range []string{"bytes 3-2/4", "bytes 0-4/4", "bytes 0-1/*", "bytes -1-2/4", "bytes 0-1/9999999999"} {
		if _, _, _, e := contentRange(s); e == nil {
			t.Fatalf("bad range %s", s)
		}
	}
	for _, s := range []string{"http://audio4-fa.scdn.co/audio", "https://evil.com/audio", "https://scdn.co.evil.com/audio", "https://x@audio4-fa.scdn.co/audio"} {
		if validCDNURL(s) {
			t.Fatalf("bad CDN URL %s", s)
		}
	}
	source := &CDNReader{URL: "https://audio4-fa.scdn.co/audio/x", Total: -1, Do: func(string, string, map[string]string, []byte) (wire.Response, error) {
		return wire.Response{Status: 200, Body: []byte("entire file")}, nil
	}}
	if _, e := source.ReadAt(make([]byte, 1), 0); e == nil {
		t.Fatal("non-ranged download accepted")
	}
}
func TestHashcashAndCancel(t *testing.T) {
	suffix, e := solveHashcash([]byte("context"), []byte("prefix"), 8, nil)
	if e != nil || len(suffix) != 16 {
		t.Fatal(e)
	}
	cancelled := errors.New("cancelled")
	if _, e = solveHashcash(nil, nil, 24, func() error { return cancelled }); e != cancelled {
		t.Fatal("work not cancelled")
	}
	if _, e = solveHashcash(nil, nil, 100, nil); e == nil {
		t.Fatal("unbounded work accepted")
	}
}
func TestGIDAndSignatureRejection(t *testing.T) {
	gid, e := GID("0000000000000000000001")
	if e != nil || len(gid) != 16 || binary.BigEndian.Uint64(gid[8:]) != 1 {
		t.Fatal("GID conversion")
	}
	if _, e := GID("invalid"); e == nil {
		t.Fatal("bad ID accepted")
	}
	if verifyAPSignature([]byte("bad"), make([]byte, 256)) {
		t.Fatal("invalid AP signature accepted")
	}
}

func TestMetadataDurationZigzag(t *testing.T) {
	if d := metadataDuration(parse(number(7, 360000))); d != 180000 {
		t.Fatalf("duration %d", d)
	}
	if d := metadataDuration(parse(number(7, 1))); d != 0 {
		t.Fatal("negative duration accepted")
	}
}
