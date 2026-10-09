package media

import (
	"encoding/binary"
	"errors"
)

// An MP4 of H.264 video made into an Animation: what Telegram makes of a GIF.
// Its apps turn one into a silent MP4 before it is sent, and the GIFs its
// search finds are MP4s already, so almost every "GIF" in a chat is one.
//
// The file is read for its first video track: the parameter sets from the
// avcC box, then where each sample is and how long it is shown, from the
// sample tables (stsz, stsc, stco/co64, stts).  Samples go to h264bsd a NAL
// unit at a time; each picture it gives back goes to a Builder.  h264bsd
// decodes the Baseline profile, which is what GIF sites serve: a Main or High
// profile file is refused before anything is decoded.
//
// Decoding takes a while, so it goes a step at a time: Start, then Step from
// an idle loop until it says it is done, then Finish.

var (
	errNotMP4     = errors.New("not an MP4")
	errNoIndex    = errors.New("an MP4 without its index (moov)")
	errBroken     = errors.New("an MP4 with a broken video track")
	errH265       = errors.New("H.265 video, past this decoder")
	errNotH264    = errors.New("video that is not H.264")
	errNoVideo    = errors.New("an MP4 with no video in it")
	errSettings   = errors.New("an MP4 with broken H.264 settings")
	errMain       = errors.New("H.264 in the Main profile: this decodes Baseline only")
	errHigh       = errors.New("H.264 in the High profile: this decodes Baseline only")
	errProfile    = errors.New("H.264 in a profile this does not decode")
	errVideoBig   = errors.New("a video too big to animate")
	errTables     = errors.New("an MP4 with broken sample tables")
	errFragmented = errors.New("a fragmented MP4, which this does not read")
	errCutShort   = errors.New("an MP4 cut short")
	errNotDecoded = errors.New("not decoded")
)

// The picture sizes this decodes: its reference pictures are kept whole, up
// to 16 of them, and 640x640 is a little over half a megabyte each.
const (
	maxVideoPixels = 640 * 640
	maxSamples     = 4096
)

type box struct {
	typ  string
	data []byte // what is inside, nil when there is no such box
	off  int    // where data starts in the file
}

// boxes walks the boxes in b, which starts at file offset base.
func boxes(b []byte, base int, fn func(box) bool) {
	for at := 0; len(b)-at >= 8; {
		size := uint64(binary.BigEndian.Uint32(b[at:]))
		head := 8
		if size == 1 {
			if len(b)-at < 16 {
				return
			}
			size, head = binary.BigEndian.Uint64(b[at+8:]), 16
		} else if size == 0 {
			size = uint64(len(b) - at) // to the end of what holds it
		}
		if size < uint64(head) || size > uint64(len(b)-at) {
			return
		}
		bx := box{typ: string(b[at+4 : at+8]), data: b[at+head : at+int(size)], off: base + at + head}
		if !fn(bx) {
			return
		}
		at += int(size)
	}
}

func child(in box, typ string) box {
	var found box
	if in.data == nil {
		return found
	}
	boxes(in.data, in.off, func(b box) bool {
		if b.typ == typ {
			found = b
			return false
		}
		return true
	})
	return found
}

type track struct {
	stbl, avcC box
	timescale  uint32
	w, h       int
	codec      string
}

// findVideo finds the first track whose handler is "vide".
func findVideo(file []byte) (track, error) {
	var t track
	root := box{data: file}
	moov := child(root, "moov")
	if moov.data == nil {
		if child(root, "ftyp").data != nil {
			return t, errNoIndex
		}
		return t, errNotMP4
	}
	err := errNoVideo
	boxes(moov.data, moov.off, func(trak box) bool {
		if trak.typ != "trak" {
			return true
		}
		mdia := child(trak, "mdia")
		hdlr := child(mdia, "hdlr")
		if len(hdlr.data) < 12 || string(hdlr.data[8:12]) != "vide" {
			return true
		}
		if mdhd := child(mdia, "mdhd"); len(mdhd.data) >= 24 {
			at := 12
			if mdhd.data[0] == 1 {
				at = 20
			}
			t.timescale = binary.BigEndian.Uint32(mdhd.data[at:])
		}
		t.stbl = child(child(mdia, "minf"), "stbl")
		stsd := child(t.stbl, "stsd")
		// Its first sample entry, a VisualSampleEntry: 78 bytes of
		// fields, the width and height among them, then boxes of its own.
		if len(stsd.data) < 8+8+78 {
			err = errBroken
			return false
		}
		var entry box
		boxes(stsd.data[8:], stsd.off+8, func(b box) bool { entry = b; return false })
		if len(entry.data) < 78 {
			err = errBroken
			return false
		}
		t.codec = entry.typ
		t.w = int(binary.BigEndian.Uint16(entry.data[24:]))
		t.h = int(binary.BigEndian.Uint16(entry.data[26:]))
		boxes(entry.data[78:], entry.off+78, func(b box) bool {
			if b.typ == "avcC" {
				t.avcC = b
				return false
			}
			return true
		})
		switch {
		case (t.codec != "avc1" && t.codec != "avc3") || t.avcC.data == nil:
			if t.codec == "hvc1" || t.codec == "hev1" {
				err = errH265
			} else {
				err = errNotH264
			}
		case t.stbl.data == nil || t.timescale == 0:
			err = errBroken
		default:
			err = nil
		}
		return false
	})
	return t, err
}

// Probe reads the first video track's size and profile from the header
// alone: false when it is not an MP4 of H.264.
func Probe(file []byte) (w, h, profile int, ok bool) {
	t, err := findVideo(file)
	if err != nil || len(t.avcC.data) < 4 {
		return 0, 0, 0, false
	}
	return t.w, t.h, int(t.avcC.data[1]), true
}

type sample struct {
	off, size int
	ms        int // how long its picture is shown
}

// Mp4 decodes one file into an animation a step at a time.
type Mp4 struct {
	samples     []sample
	next, shown int // the next sample to feed; pictures that have come
	nalLen      int
	dec         h264
	file        []byte // the decoder's copy of the file
	b           *Builder
	failed      error
}

func be32(b []byte, at int) uint32 { return binary.BigEndian.Uint32(b[at:]) }

// Start reads the file's tables and starts the decoder, which keeps a copy of
// the file.  An error says why it cannot be decoded.
func (m *Mp4) Start(file []byte, maxW, maxH, colours int, bg uint32, budget int) error {
	m.Cancel()
	t, err := findVideo(file)
	if err != nil {
		return err
	}
	// avcC: version, profile, its compatibility flags, level, the size of
	// the lengths in front of NAL units, then the parameter sets.
	c := t.avcC.data
	if len(c) < 7 || c[0] != 1 {
		return errSettings
	}
	profile, compat := c[1], c[2]
	// Baseline, or another profile that says it keeps to Baseline's tools
	// (constraint_set0_flag).
	if profile != 66 && compat&0x80 == 0 {
		switch {
		case profile == 77:
			return errMain
		case profile >= 100:
			return errHigh
		}
		return errProfile
	}
	if t.w <= 0 || t.h <= 0 {
		return errBroken
	}
	if t.w*t.h > maxVideoPixels {
		return errVideoBig
	}
	samples, err := sampleTable(file, t)
	if err != nil {
		return err
	}
	dec, ok := openH264(file)
	if !ok {
		return ErrNoMemory
	}
	m.samples, m.dec, m.nalLen, m.file = samples, dec, int(c[4]&3)+1, dec.file()
	m.b = NewBuilder(maxW, maxH, colours, bg, budget)
	// The parameter sets first: numOfSequenceParameterSets (low five bits),
	// each a 16-bit length and the NAL unit, then the picture ones likewise.
	at, end := 5, len(c)
	for kind := 0; kind < 2 && at < end; kind++ {
		n := int(c[at])
		if kind == 0 {
			n &= 31
		}
		at++
		for i := 0; i < n && end-at >= 2; i++ {
			l := int(binary.BigEndian.Uint16(c[at:]))
			at += 2
			if end-at < l {
				break
			}
			if l > 0 && m.dec.feed(t.avcC.off+at, l) < 0 {
				m.Cancel()
				return ErrNoMemory
			}
			at += l
		}
	}
	return nil
}

// sampleTable is where each sample is and how long it is shown.
func sampleTable(file []byte, t track) ([]sample, error) {
	stsz, stsc := child(t.stbl, "stsz").data, child(t.stbl, "stsc").data
	stco, stts := child(t.stbl, "stco"), child(t.stbl, "stts").data
	wide := false
	if stco.data == nil {
		stco, wide = child(t.stbl, "co64"), true
	}
	if len(stsz) < 12 || len(stsc) < 8 || len(stco.data) < 8 || len(stts) < 8 {
		return nil, errTables
	}
	fixed, count := int(be32(stsz, 4)), int(be32(stsz, 8))
	if count == 0 {
		return nil, errFragmented
	}
	count = min(count, maxSamples) // the first few minutes are plenty for a GIF
	if fixed == 0 && len(stsz) < 12+4*count {
		return nil, errTables
	}
	s := make([]sample, count)
	for i := range s {
		s[i].size = fixed
		if fixed == 0 {
			s[i].size = int(be32(stsz, 12+4*i))
		}
	}
	// Offsets: chunk by chunk, stsc saying how many samples each has, the
	// samples of a chunk one after another from its offset.
	co := stco.data
	nChunks, nRuns := int(be32(co, 4)), int(be32(stsc, 4))
	step := 4
	if wide {
		step = 8
	}
	if len(co) < 8+step*nChunks || len(stsc) < 8+12*nRuns {
		return nil, errTables
	}
	n := 0
	for r := 0; r < nRuns && n < count; r++ {
		first, per := int(be32(stsc, 8+12*r)), int(be32(stsc, 12+12*r))
		last := nChunks // chunks are counted from 1
		if r+1 < nRuns {
			last = int(be32(stsc, 8+12*(r+1))) - 1
		}
		for ch := first; ch >= 1 && ch <= last && ch <= nChunks && n < count; ch++ {
			var off uint64
			if wide {
				off = binary.BigEndian.Uint64(co[8+8*(ch-1):])
			} else {
				off = uint64(be32(co, 8+4*(ch-1)))
			}
			for k := 0; k < per && n < count; k, n = k+1, n+1 {
				if off+uint64(s[n].size) > uint64(len(file)) {
					return nil, errCutShort
				}
				s[n].off = int(off)
				off += uint64(s[n].size)
			}
		}
	}
	s = s[:n] // tables that do not add up: what they do say
	// How long each is shown: stts in the track's time scale, made
	// milliseconds from the running total so that rounding does not add up.
	nTimes := int(be32(stts, 4))
	if len(stts) < 8+8*nTimes {
		return nil, errTables
	}
	var ticks, lastMS uint64
	i := 0
	for r := 0; r < nTimes && i < len(s); r++ {
		k, d := int(be32(stts, 8+8*r)), uint64(be32(stts, 12+8*r))
		for ; k > 0 && i < len(s); k, i = k-1, i+1 {
			ticks += d
			ms := ticks * 1000 / uint64(t.timescale)
			s[i].ms = int(ms - lastMS)
			lastMS = ms
		}
	}
	for ; i < len(s); i++ {
		s[i].ms = 100
		if i > 0 {
			s[i].ms = s[i-1].ms
		}
	}
	return s, nil
}

// Busy is true from Start until Step has nothing left to do.
func (m *Mp4) Busy() bool { return m.dec.p != nil }

// took gives the builder what the decoder finished: the last picture, shown
// for the samples it stands for.
func (m *Mp4) took(pictures int) {
	if pictures < 0 {
		m.failed = ErrNoMemory
		return
	}
	if pictures == 0 || m.failed != nil {
		return
	}
	ms := 0
	for ; pictures > 0; pictures-- {
		ms += m.samples[min(m.shown, len(m.samples)-1)].ms
		m.shown++
	}
	rgba, w, h := m.dec.picture()
	if rgba != nil && !m.b.Add(rgba, w, h, ms) {
		m.failed = m.b.Err()
	}
}

// Step decodes for about ms milliseconds of now(); true while there is more.
func (m *Mp4) Step(ms uint64, now func() uint64) bool {
	if m.dec.p == nil {
		return false
	}
	from := now()
	for m.next < len(m.samples) && m.failed == nil {
		s := m.samples[m.next]
		m.next++
		// A sample is NAL units, each behind its length.
		for at, end := s.off, s.off+s.size; m.failed == nil && end-at > m.nalLen; {
			l := 0
			for i := 0; i < m.nalLen; i++ {
				l = l<<8 | int(m.file[at+i])
			}
			at += m.nalLen
			if l == 0 || l > end-at {
				break
			}
			m.took(m.dec.feed(at, l))
			at += l
		}
		if now()-from >= ms {
			return true
		}
	}
	if m.failed == nil {
		m.took(m.dec.flush())
	}
	// The file and the decoder are done with; the frames are kept for Finish.
	m.dec.close()
	m.dec, m.file = h264{}, nil
	return false
}

// Finish hands over the animation, or why there is none.  Everything else
// is let go.
func (m *Mp4) Finish() (Animation, error) {
	if m.b == nil {
		return Animation{}, errNotDecoded
	}
	var a Animation
	err := m.failed
	if err == nil {
		a, err = m.b.Finish()
	}
	m.Cancel()
	return a, err
}

// Cancel stops and lets everything go.
func (m *Mp4) Cancel() {
	if m.dec.p != nil {
		m.dec.close()
	}
	if m.b != nil {
		m.b.Cancel()
	}
	*m = Mp4{}
}
