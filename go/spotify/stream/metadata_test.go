package stream

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"strings"
	"testing"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"github.com/krustowski/rou2exOS-apps/go/spotify/third_party/shannon"
)

func metadataResponse(uri string, kind uint64, payload []byte) []byte {
	data := join(blob(1, number(1, 200)), text(2, uri), blob(3, join(text(1, "type.googleapis.com/test"), blob(2, payload))))
	return blob(2, join(number(2, kind), blob(3, data)))
}

type deadlineSocket struct {
	input   *bytes.Reader
	output  bytes.Buffer
	timeout time.Duration
}

func (s *deadlineSocket) Read(b []byte) (int, error) {
	if s.timeout == 0 {
		return 0, errors.New("old AP deadline expired")
	}
	return s.input.Read(b)
}
func (s *deadlineSocket) Write(b []byte) (int, error) { return s.output.Write(b) }
func (*deadlineSocket) Close() error                  { return nil }
func (s *deadlineSocket) SetDeadline(d time.Duration) { s.timeout = d }

// Exercise Session.Audio, not just file selection: modern metadata has no
// legacy file field, and the AP deadline has expired while HTTPS was busy.
func TestAudioUsesSeparateFilesExtensionAndRenewsAPDeadline(t *testing.T) {
	const id = "0000000000000000000001"
	uri := "spotify:track:" + id
	file := bytes.Repeat([]byte{0x42}, 20)
	key := bytes.Repeat([]byte{0x31}, 16)
	shannonKey := bytes.Repeat([]byte{7}, 32)
	var reply bytes.Buffer
	tx := packetConn{rw: &reply, send: shannon.New(shannonKey)}
	if err := tx.write(0x0d, append(make([]byte, 4), key...)); err != nil {
		t.Fatal(err)
	}
	sock := &deadlineSocket{input: bytes.NewReader(reply.Bytes())}
	session := Session{spHost: "test.spotify.com", ap: &Accesspoint{socket: sock,
		packets: packetConn{rw: sock, send: shannon.New(shannonKey), recv: shannon.New(shannonKey)}}}
	calls := 0
	session.HTTP = func(method, url string, _ map[string]string, body []byte) (wire.Response, error) {
		calls++
		r := wire.Response{Status: 200}
		if method == "POST" {
			request := parse(body).sub(2)
			if string(request.bytes(1)) != uri {
				t.Fatal("wrong metadata URI")
			}
			switch kind := request.sub(2).num(1); kind {
			case 10:
				r.Body = metadataResponse(uri, kind, number(7, 360000))
			case 5:
				files := join(blob(1, blob(1, join(blob(1, file), number(2, 1)))),
					blob(1, blob(1, join(blob(1, file), number(2, 0)))))
				r.Body = metadataResponse(uri, kind, files)
			default:
				t.Fatalf("unexpected metadata kind %d", kind)
			}
		} else {
			if !strings.Contains(url, "/interactive/0/"+strings.Repeat("42", 20)) {
				t.Fatalf("wrong storage file: %s", url)
			}
			r.Body = join(number(1, 0), text(2, "https://audio4-fa.scdn.co/audio/test"))
		}
		return r, nil
	}
	source, gotKey, err := session.Audio(id)
	if err != nil {
		t.Fatal(err)
	}
	if calls != 3 || source.DurationMS != 180000 || !bytes.Equal(gotKey, key) || sock.timeout != 30*time.Second {
		t.Fatal("incomplete native playback resolution")
	}
	// Decrypt the request to prove the audio key was requested for the right IDs.
	rx := packetConn{rw: bytes.NewBuffer(sock.output.Bytes()), recv: shannon.New(shannonKey)}
	kind, request, err := rx.read()
	if err != nil || kind != 0x0c || len(request) != 42 || !bytes.Equal(request[:20], file) || binary.BigEndian.Uint64(request[28:36]) != 1 {
		t.Fatal("wrong AP audio-key request", err)
	}
}

func TestMetadataErrorsDoNotMasqueradeAsUnsupportedCodec(t *testing.T) {
	uri := "spotify:track:test"
	for _, response := range [][]byte{
		nil,
		metadataResponse("spotify:track:other", 10, number(7, 1)),
		metadataResponse(uri, 5, number(7, 1)),
		metadataResponse(uri, 10, nil),
		metadataResponse(uri, 10, []byte{0x80}),
		blob(2, join(number(2, 10), blob(3, join(blob(1, number(1, 403)), text(2, uri))))),
	} {
		if _, err := extendedData(parse(response), 10, uri); err == nil {
			t.Fatalf("invalid metadata accepted: %x", response)
		}
	}
}

func TestAudioFileFormatsAndLegacyMetadata(t *testing.T) {
	vorbis := bytes.Repeat([]byte{1}, 20)
	mp3 := bytes.Repeat([]byte{2}, 20)
	files := join(blob(1, blob(1, join(blob(1, mp3), number(2, 3)))), blob(1, blob(1, join(blob(1, vorbis), number(2, 2)))))
	if file, format := pickAudioFiles(parse(files)); !bytes.Equal(file, vorbis) || format != 2 {
		t.Fatal("wrong format selected")
	}
	if file, _ := pickAudioFiles(parse(blob(1, blob(1, join(blob(1, mp3), number(2, 3)))))); len(file) != 0 {
		t.Fatal("MP3 accepted as Vorbis")
	}
	if file, format := pickVorbis(parse(blob(12, join(blob(1, vorbis), number(2, 0))))); !bytes.Equal(file, vorbis) || format != 0 {
		t.Fatal("legacy Vorbis rejected")
	}
}

var _ io.ReadWriteCloser = (*deadlineSocket)(nil)
