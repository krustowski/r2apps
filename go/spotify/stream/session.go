package stream

import (
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/krustowski/rou2exOS-apps/go/r2tls/wire"
	"math/bits"
	"strconv"
	"strings"
)

// Device client identity used by the published librespot playback protocol;
// this is a public application identifier, not an account credential.
const playbackClientID = "65b708073fc0480ea92a077233ca87bd"

type HTTP func(method, url string, headers map[string]string, body []byte) (wire.Response, error)
type Session struct {
	HTTP                       HTTP
	Dial                       func(host string, port uint16) (Socket, error)
	Random                     func([]byte) error
	Yield                      func() error
	Device                     string
	ap                         *Accesspoint
	spHost, clientToken, token string
	Progress                   func(string)
}

func (s *Session) Close() {
	if s.ap != nil {
		s.ap.Close()
		s.ap = nil
	}
}
func (s *Session) protobuf(method, url string, b []byte, headers map[string]string) (message, error) {
	if headers == nil {
		headers = make(map[string]string)
	}
	headers["Accept"] = "application/x-protobuf"
	headers["Content-Type"] = "application/x-protobuf"
	r, e := s.HTTP(method, url, headers, b)
	if e != nil {
		return message{}, e
	}
	if r.Status != 200 {
		return message{}, fmt.Errorf("Spotify playback HTTP %d", r.Status)
	}
	m := parse(r.Body)
	return m, m.err
}
func (s *Session) Start(accessToken string) error {
	s.Close()
	if s.Device == "" {
		var id [20]byte
		if e := s.Random(id[:]); e != nil {
			return e
		}
		s.Device = hex.EncodeToString(id[:])
	}
	r, e := s.HTTP("GET", "https://apresolve.spotify.com/?type=accesspoint&type=spclient", nil, nil)
	if e != nil {
		return e
	}
	if r.Status != 200 {
		return fmt.Errorf("AP resolver HTTP %d", r.Status)
	}
	var endpoints struct{ Accesspoint, Spclient []string }
	if json.Unmarshal(r.Body, &endpoints) != nil {
		return errors.New("invalid AP resolver JSON")
	}
	var addr string
	for _, a := range endpoints.Accesspoint {
		if strings.HasSuffix(a, ":443") {
			addr = a
			break
		}
	}
	if addr == "" && len(endpoints.Accesspoint) > 0 {
		addr = endpoints.Accesspoint[0]
	}
	if addr == "" || len(endpoints.Spclient) == 0 {
		return errors.New("missing Spotify playback endpoints")
	}
	split := strings.LastIndexByte(addr, ':')
	if split < 1 {
		return errors.New("invalid AP endpoint")
	}
	port, e := strconv.ParseUint(addr[split+1:], 10, 16)
	if e != nil || port == 0 || !spotifyHost(addr[:split]) {
		return errors.New("invalid AP host/port")
	}
	sp := endpoints.Spclient[0]
	if strings.HasSuffix(sp, ":443") {
		sp = strings.TrimSuffix(sp, ":443")
	}
	if !spotifyHost(sp) {
		return errors.New("invalid spclient endpoint")
	}
	s.spHost = sp
	socket, e := s.Dial(addr[:split], uint16(port))
	if e != nil {
		return e
	}
	s.ap, e = OpenAccesspoint(socket, accessToken, s.Device, s.Random)
	if e != nil {
		return e
	}
	success := false
	defer func() {
		if !success {
			s.Close()
		}
	}()
	platform := join(text(1, "Linux"), text(2, "6.0"), text(3, "rou2exOS"), text(4, "x86_64"))
	sdk := join(blob(1, blob(5, platform)), text(2, s.Device))
	clientData := join(text(1, "1.2.77.358"), text(2, playbackClientID), blob(3, sdk))
	m, e := s.protobuf("POST", "https://clienttoken.spotify.com/v1/clienttoken", join(number(1, 1), blob(2, clientData)), nil)
	if e != nil {
		return e
	}
	s.clientToken = string(m.sub(2).bytes(1))
	if m.num(1) != 1 || s.clientToken == "" {
		return errors.New("Spotify client-token challenge unsupported")
	}
	clientInfo := join(text(1, playbackClientID), text(2, s.Device))
	credential := join(text(1, s.ap.Username), blob(2, s.ap.Stored))
	request := join(blob(1, clientInfo), blob(100, credential))
	for attempt := 0; attempt < 2; attempt++ {
		m, e = s.protobuf("POST", "https://login5.spotify.com/v3/login", request, map[string]string{"Client-Token": s.clientToken})
		if e != nil {
			return e
		}
		ok := m.sub(1)
		if ok.err != nil {
			return ok.err
		}
		s.token = string(ok.bytes(2))
		if s.token != "" {
			success = true
			return nil
		}
		challenges := m.sub(3).all(1)
		if attempt != 0 || len(challenges) == 0 {
			return fmt.Errorf("Spotify login5 rejected (%d)", m.num(2))
		}
		if len(challenges) > 4 {
			return errors.New("too many login5 challenges")
		}
		var solutions []byte
		for _, b := range challenges {
			challenge := parse(b)
			hash := challenge.sub(1)
			if len(hash.bytes(1)) == 0 {
				return errors.New("Spotify login5 challenge unsupported")
			}
			suffix, e := solveHashcash(m.bytes(5), hash.bytes(1), int(hash.num(2)), s.Yield)
			if e != nil {
				return e
			}
			solutions = append(solutions, blob(1, blob(1, blob(1, suffix)))...)
		}
		request = join(blob(1, clientInfo), blob(2, m.bytes(5)), blob(3, solutions), blob(100, credential))
	}
	return errors.New("Spotify playback login failed")
}
func spotifyHost(host string) bool {
	return strings.HasSuffix(host, ".spotify.com") && !strings.ContainsAny(host, "/:\r\n")
}
func (s *Session) spHeaders() map[string]string {
	return map[string]string{"Authorization": "Bearer " + s.token, "Client-Token": s.clientToken}
}
func (s *Session) Audio(id string) (*CDNReader, []byte, error) {
	if s.ap == nil {
		return nil, nil, errors.New("playback session not connected")
	}
	gid, e := GID(id)
	if e != nil {
		return nil, nil, e
	}
	s.report("Loading Spotify track metadata...")
	track, e := s.extension(id, 10)
	if e != nil {
		return nil, nil, fmt.Errorf("track metadata: %w", e)
	}
	file, format := pickVorbis(track)
	// Some regional releases carry the playable file on an alternative track.
	if len(file) != 20 {
		for i, b := range track.all(13) {
			if i == 8 {
				break
			}
			alt := parse(b)
			if alt.err != nil || len(alt.bytes(1)) != 16 {
				continue
			}
			file, format = pickVorbis(alt)
			if len(file) == 20 {
				gid = append([]byte(nil), alt.bytes(1)...)
				break
			}
		}
	}
	if len(file) != 20 {
		s.report("Loading Spotify audio file metadata...")
		files, err := s.extension(id, 5)
		if err != nil {
			return nil, nil, fmt.Errorf("audio file metadata: %w", err)
		}
		file, format = pickAudioFiles(files)
	}
	if len(file) != 20 {
		return nil, nil, errors.New("audio metadata: no supported Vorbis file")
	}
	s.report("Requesting Spotify audio key...")
	key, e := s.ap.AudioKey(gid, file)
	if e != nil {
		return nil, nil, fmt.Errorf("audio key: %w", e)
	}
	s.report("Resolving Spotify audio storage...")
	path := "https://" + s.spHost + "/storage-resolve/v2/files/audio/interactive/" + strconv.Itoa(format) + "/" + hex.EncodeToString(file)
	storage, e := s.protobuf("GET", path, nil, s.spHeaders())
	if e != nil {
		return nil, nil, fmt.Errorf("audio storage: %w", e)
	}
	if storage.num(1) != 0 {
		return nil, nil, errors.New("Spotify audio storage is restricted or not CDN")
	}
	var urls []string
	for _, u := range storage.all(2) {
		url := string(u)
		if validCDNURL(url) {
			duplicate := false
			for _, old := range urls {
				if url == old {
					duplicate = true
				}
			}
			if !duplicate {
				urls = append(urls, url)
			}
			if len(urls) == 4 {
				break
			}
		}
	}
	if len(urls) > 0 {
		return &CDNReader{Do: s.HTTP, URL: urls[0], URLs: urls, Check: s.Yield, Progress: s.Progress, Total: -1, DurationMS: metadataDuration(track)}, key, nil
	}
	return nil, nil, errors.New("Spotify supplied no supported HTTPS audio CDN URL")
}
func (s *Session) report(status string) {
	if s.Progress != nil {
		s.Progress(status)
	}
}
func (s *Session) extension(id string, kind uint64) (message, error) {
	uri := "spotify:track:" + id
	entity := join(text(1, uri), blob(2, number(1, kind)))
	m, e := s.protobuf("POST", "https://"+s.spHost+"/extended-metadata/v0/extended-metadata", blob(2, entity), s.spHeaders())
	if e != nil {
		return message{}, e
	}
	return extendedData(m, kind, uri)
}
func extendedData(m message, kind uint64, uri string) (message, error) {
	if m.err != nil {
		return message{}, m.err
	}
	for _, arr := range m.all(2) {
		a := parse(arr)
		if a.err != nil {
			return message{}, a.err
		}
		if a.num(2) != kind {
			continue
		}
		for _, data := range a.all(3) {
			d := parse(data)
			if d.err != nil {
				return message{}, d.err
			}
			if string(d.bytes(2)) != uri {
				continue
			}
			header := d.sub(1)
			if header.err != nil {
				return message{}, header.err
			}
			if status := header.num(1); status != 200 {
				return message{}, fmt.Errorf("Spotify metadata status %d", status)
			}
			any := d.sub(3)
			if any.err != nil || len(any.bytes(2)) == 0 {
				return message{}, errors.New("missing Spotify metadata payload")
			}
			value := parse(any.bytes(2))
			return value, value.err
		}
	}
	return message{}, fmt.Errorf("Spotify metadata extension %d missing", kind)
}
func pickAudioFiles(m message) ([]byte, int) {
	var files [][]byte
	for _, b := range m.all(1) {
		f := parse(b)
		if f.err == nil {
			files = append(files, f.bytes(1))
		}
	}
	return pickFiles(files)
}
func pickVorbis(m message) ([]byte, int) { return pickFiles(m.all(12)) }
func pickFiles(files [][]byte) ([]byte, int) {
	best := 3
	var file []byte
	for _, b := range files {
		f := parse(b)
		format := int(f.num(2))
		if f.err == nil && len(f.bytes(1)) == 20 && format >= 0 && format < best {
			file = f.bytes(1)
			best = format
		}
	}
	return append([]byte(nil), file...), best
}
func solveHashcash(context, prefix []byte, difficulty int, yield func() error) ([]byte, error) {
	if difficulty < 0 || difficulty > 24 || len(prefix) > 1024 {
		return nil, errors.New("login5 hashcash difficulty unsupported")
	}
	seed := sha1.Sum(context)
	suffix := make([]byte, 16)
	copy(suffix, seed[12:])
	data := join(prefix, suffix)
	for attempt := 0; attempt < 1<<25; attempt++ {
		if attempt%4096 == 0 && yield != nil {
			if e := yield(); e != nil {
				return nil, e
			}
		}
		copy(data[len(prefix):], suffix)
		hash := sha1.Sum(data)
		zeros := 0
		for i := len(hash) - 1; i >= 0; i-- {
			z := bits.TrailingZeros8(hash[i])
			zeros += z
			if z < 8 {
				break
			}
		}
		if zeros >= difficulty {
			return suffix, nil
		}
		for _, start := range []int{0, 8} {
			for i := start + 7; i >= start; i-- {
				suffix[i]++
				if suffix[i] != 0 {
					break
				}
			}
		}
	}
	return nil, errors.New("login5 hashcash work limit exceeded")
}

func metadataDuration(track message) uint32 {
	encoded := track.num(7) // Spotify metadata uses protobuf sint32 (zigzag).
	if encoded > 0xffffffff || encoded&1 != 0 {
		return 0
	}
	return uint32(encoded >> 1)
}
