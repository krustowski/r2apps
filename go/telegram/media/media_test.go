package media

import (
	"os"
	"strings"
	"testing"
)

// The fixtures and expectations of Memento's own web/tests/unit.cpp, so that
// this package is held to what the C++ engine does.
func img(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile("../../../cpp/memento-hello/web/tests/img/" + name)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestPictures(t *testing.T) {
	// Red: dithered between the two reds of the 16; in the cube, a red
	// with no green or blue in it.
	red := img(t, "red.jpg")
	p, err := DecodePicture(red, 100, 100, 16, 0xFFFFFF)
	if err != nil || p.W != 16 || p.H != 16 {
		t.Fatalf("red.jpg: %v %dx%d", err, p.W, p.H)
	}
	for _, v := range p.Px[:256] {
		if v != 4 && v != 12 {
			t.Fatalf("red.jpg has colour %d", v)
		}
	}
	p, err = DecodePicture(red, 100, 100, 256, 0xFFFFFF)
	for _, v := range p.Px[:256] {
		if err != nil || v < 16+36*4 || v > 231 || (v-16)%36 != 0 {
			t.Fatalf("red.jpg in the cube has colour %d (%v)", v, err)
		}
	}
	// Transparent: whatever it is laid over.
	clear := img(t, "clear.png")
	if p, err := DecodePicture(clear, 100, 100, 16, 0xFFFFFF); err != nil || p.Px[0] != 15 {
		t.Fatal("clear.png over white")
	}
	if p, err := DecodePicture(clear, 100, 100, 16, 0); err != nil || p.Px[5] != 0 {
		t.Fatal("clear.png over black")
	}
	// Made to fit, keeping its shape; never enlarged.
	wide := img(t, "wide.gif")
	if w, h, ok := PictureSize(wide); !ok || w != 100 || h != 50 {
		t.Fatal("wide.gif size")
	}
	if p, err := DecodePicture(wide, 40, 40, 16, 0); err != nil || p.W != 40 || p.H != 20 || p.Px[0] != 15 {
		t.Fatalf("wide.gif in 40x40: %dx%d", p.W, p.H)
	}
	if p, err := DecodePicture(wide, 1000, 10, 16, 0); err != nil || p.W != 20 || p.H != 10 {
		t.Fatalf("wide.gif in 1000x10: %dx%d", p.W, p.H)
	}
	if p, err := DecodePicture(img(t, "blue.bmp"), 100, 100, 16, 0); err != nil || p.W != 6 || p.H != 3 || p.Px[0] != 1 {
		t.Fatal("blue.bmp")
	}
	// Not a picture, or cut short: an answer, not a crash.
	if _, err := DecodePicture([]byte("<html>"), 100, 100, 16, 0); err == nil {
		t.Fatal("<html> decoded")
	}
	if p, err := DecodePicture(red[:len(red)/3], 100, 100, 16, 0); err == nil && p.W != 16 {
		t.Fatal("a cut JPEG")
	}
}

// Animated GIFs: every frame, each held as long as the file says (0 ms as
// browsers take it, 100), and when they do not all fit, every other one left
// out as often as it takes, its time given to the one before.
func TestAnimations(t *testing.T) {
	anim := img(t, "anim.gif")
	a, err := DecodeAnimation(anim, 100, 100, 16, 0xFFFFFF, 64*1024)
	if err != nil || a.W != 20 || a.H != 10 || a.Frames != 3 {
		t.Fatalf("anim.gif: %v %dx%d %d frames", err, a.W, a.H, a.Frames)
	}
	if a.Delay[0] != 50 || a.Delay[1] != 100 || a.Delay[2] != 100 || a.Length != 250 {
		t.Fatalf("anim.gif delays %v", a.Delay)
	}
	if f0 := a.Frame(0)[0]; (f0 != 4 && f0 != 12) || a.Frame(1)[0] != 0 || a.Frame(2)[199] != 15 {
		t.Fatal("anim.gif colours")
	}
	for ms, want := range map[uint64]int{0: 0, 49: 0, 50: 1, 149: 1, 150: 2, 250: 0, 250*7 + 60: 1} {
		if got := a.FrameAt(ms); got != want {
			t.Fatalf("FrameAt(%d) = %d, want %d", ms, got, want)
		}
	}
	if a, err := DecodeAnimation(anim, 10, 10, 16, 0xFFFFFF, 64*1024); err != nil || a.W != 10 || a.H != 5 {
		t.Fatal("anim.gif made smaller")
	}
	// Ten frames of 8x8, a white column moving right, in room for four:
	// frames 0, 4 and 8, the last of them with 9's time too.
	ten := img(t, "ten.gif")
	a, err = DecodeAnimation(ten, 100, 100, 16, 0, 4*64)
	if err != nil || a.Frames != 3 || a.Length != 400 || a.Delay[0] != 160 || a.Delay[1] != 160 || a.Delay[2] != 80 {
		t.Fatalf("ten.gif in room for four: %v %d frames %v", err, a.Frames, a.Delay)
	}
	if a.Frame(0)[0] != 15 || a.Frame(0)[1] != 0 || a.Frame(1)[4] != 15 || a.Frame(1)[0] != 0 || a.Frame(2)[0] != 15 {
		t.Fatal("ten.gif frames kept")
	}
	if a, err := DecodeAnimation(ten, 100, 100, 16, 0, 64*1024); err != nil || a.Frames != 10 || a.Length != 400 ||
		a.Frame(9)[1] != 15 || a.Frame(9)[0] != 0 {
		t.Fatal("ten.gif with room")
	}
	if a, err := DecodeAnimation(ten, 100, 100, 16, 0, 100); err == nil || a.Px != nil {
		t.Fatal("not room for two frames")
	}
	if a, err := DecodeAnimation(img(t, "wide.gif"), 100, 100, 16, 0, 64*1024); err != nil || a.Frames != 1 || a.FrameAt(12345) != 0 {
		t.Fatal("a still GIF")
	}
	if _, err := DecodeAnimation(img(t, "red.jpg"), 100, 100, 16, 0, 64*1024); err == nil {
		t.Fatal("a JPEG animated")
	}
	if a, err := DecodeAnimation(ten[:len(ten)*2/3], 100, 100, 16, 0, 64*1024); err != nil || a.Frames < 1 || a.Frames >= 10 {
		t.Fatalf("a cut GIF: %v %d", err, a.Frames)
	}
	if _, err := DecodeAnimation(ten[:20], 100, 100, 16, 0, 64*1024); err == nil {
		t.Fatal("20 bytes of GIF")
	}
}

func decodeMp4(t *testing.T, name string, cut, maxW, budget int) (Animation, error) {
	file := img(t, name)
	if cut > 0 {
		file = file[:cut]
	}
	var m Mp4
	if err := m.Start(file, maxW, 100, 16, 0xFFFFFF, budget); err != nil {
		return Animation{}, err
	}
	if !m.Busy() {
		t.Fatal("not busy after Start")
	}
	clock := uint64(0)
	for steps := 0; m.Step(0, func() uint64 { clock++; return clock }) && steps < 1000; steps++ {
	}
	return m.Finish()
}

func mostly(a Animation, f int, c1, c2 byte) bool {
	n := 0
	for _, v := range a.Frame(f) {
		if v == c1 || v == c2 {
			n++
		}
	}
	return n*10 >= a.W*a.H*9
}

// colours.mp4 is 36x20 at 10 frames a second, black, white, red, blue,
// white (Baseline); high.mp4 the same with its avcC saying High.
func TestMp4(t *testing.T) {
	a, err := decodeMp4(t, "colours.mp4", 0, 100, 64*1024)
	if err != nil || a.W != 36 || a.H != 20 || a.Frames != 5 || a.Length != 500 {
		t.Fatalf("colours.mp4: %v %dx%d %d frames %d ms", err, a.W, a.H, a.Frames, a.Length)
	}
	if a.Delay[0] != 100 || a.Delay[4] != 100 || !mostly(a, 0, 0, 0) || !mostly(a, 1, 15, 15) ||
		!mostly(a, 2, 4, 12) || !mostly(a, 3, 1, 9) || !mostly(a, 4, 15, 15) {
		t.Fatal("colours.mp4 frames")
	}
	if a, err := decodeMp4(t, "colours.mp4", 0, 18, 64*1024); err != nil || a.W != 18 || a.H != 10 {
		t.Fatal("colours.mp4 made smaller")
	}
	if a, err := decodeMp4(t, "colours.mp4", 0, 100, 36*20*2); err != nil || a.Frames != 2 || a.Length != 500 {
		t.Fatalf("colours.mp4 in room for two: %v %d %d", err, a.Frames, a.Length)
	}
	if w, h, profile, ok := Probe(img(t, "colours.mp4")); !ok || w != 36 || h != 20 || profile != 66 {
		t.Fatal("probe")
	}
	if _, err := decodeMp4(t, "high.mp4", 0, 100, 64*1024); err == nil || !strings.Contains(err.Error(), "High") {
		t.Fatalf("high.mp4: %v", err)
	}
	// Cut short, or not an MP4 at all: an answer, not a crash.
	if a, err := decodeMp4(t, "colours.mp4", 600, 100, 64*1024); err == nil && a.Frames > 5 {
		t.Fatal("cut at 600")
	}
	if _, err := decodeMp4(t, "colours.mp4", 40, 100, 64*1024); err == nil {
		t.Fatal("cut at 40")
	}
	if _, err := decodeMp4(t, "anim.gif", 0, 100, 64*1024); err == nil {
		t.Fatal("a GIF as an MP4")
	}
}
