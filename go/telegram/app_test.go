package main

import (
	"errors"
	"image"
	"image/color"
	"image/png"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

const testToken = "123456789:AAHabcdefghijklmnopqrstuvwxyz01234"

// fake is everything the client is given, recorded.
type fake struct {
	t         *testing.T
	clock     uint64
	files     map[string][]byte
	jobs      []job
	cancels   int
	copied    []string
	attention int
	closed    bool
}

func newFake(t *testing.T) (*App, *fake) {
	f := &fake{t: t, files: map[string][]byte{}}
	a := newApp()
	a.colours = 16
	a.now = func() uint64 { return f.clock }
	a.readFile = func(path string, limit int) ([]byte, error) {
		b, ok := f.files[path]
		if !ok {
			return nil, errors.New("no such file")
		}
		return b, nil
	}
	a.writeFile = func(path string, data []byte) error { f.files[path] = append([]byte(nil), data...); return nil }
	a.user = func() string { return "krusty" }
	a.submit = func(j job) { f.jobs = append(f.jobs, j) }
	a.cancel = func() { f.cancels++ }
	a.copyText = func(s string) bool { f.copied = append(f.copied, s); return true }
	a.attention = func() { f.attention++ }
	a.closeWin = func() { f.closed = true }
	return a, f
}

// next runs the loop once and returns the job it started.
func (f *fake) next(a *App) job {
	f.t.Helper()
	n := len(f.jobs)
	a.idle()
	if len(f.jobs) != n+1 {
		f.t.Fatalf("no request started (status %q)", a.status)
	}
	return f.jobs[n]
}

func method(j job) string {
	p := j.path[strings.LastIndexByte(j.path, '/')+1:]
	if i := strings.IndexByte(p, '?'); i >= 0 {
		p = p[:i]
	}
	return p
}

func keyChar(c byte) *hosted.Key { return &hosted.Key{Down: true, IsChar: true, Char: c} }
func typeText(a *App, s string) {
	for i := 0; i < len(s); i++ {
		a.key(keyChar(s[i]), clipboard{})
	}
}

func ok(body string) result {
	return result{status: 200, body: []byte(`{"ok":true,"result":` + body + `}`)}
}

func TestTokenSetup(t *testing.T) {
	a, f := newFake(t)
	a.start()
	if !a.setup {
		t.Fatal("chatting without a token")
	}
	typeText(a, "123:short")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	if !a.setup || !strings.Contains(a.status, "token") {
		t.Fatalf("a short token taken: %q", a.status)
	}
	a.input = a.input[:0]
	// Pasted, with the space a copied token brings along.
	a.key(&hosted.Key{Down: true, IsChar: true, Char: 'v', LeftControl: true}, clipboard{text: " " + testToken})
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	if a.setup || a.token != testToken || string(f.files[typedToken]) != testToken+"\n" {
		t.Fatalf("token not taken: setup %v, file %q", a.setup, f.files[typedToken])
	}
	if j := f.next(a); method(j) != "getMe" || j.path != "/bot"+testToken+"/getMe" || j.api != "https://api.telegram.org" {
		t.Fatalf("first request %+v", j)
	}
	// A token the server does not know: back to the setup, the token to fix.
	a.finished(result{status: 401, body: []byte(`{"ok":false,"description":"Unauthorized"}`)})
	if !a.setup || string(a.input) != testToken {
		t.Fatal("not back at the setup")
	}

	// Shipped on the boot medium, with an address for a test server.
	b, f2 := newFake(t)
	f2.files[shippedToken] = []byte(testToken + "\napi=http://10.0.2.2:8081/\n\x00\x00")
	b.start()
	if b.setup || b.api != "http://10.0.2.2:8081" {
		t.Fatalf("shipped token: setup %v api %q", b.setup, b.api)
	}
}

// chatting is a client past getMe, with one message in from Alice.
func chatting(t *testing.T) (*App, *fake) {
	a, f := newFake(t)
	f.files[shippedToken] = []byte(testToken + "\n")
	a.start()
	f.next(a)
	a.finished(ok(`{"id":1,"is_bot":true,"first_name":"R2","username":"r2bot"}`))
	if a.botName != "r2bot" || !strings.Contains(a.status, "waiting") {
		t.Fatalf("getMe: %q %q", a.botName, a.status)
	}
	j := f.next(a)
	if method(j) != "getUpdates" || !strings.Contains(j.path, "offset=0") || !strings.Contains(j.path, "timeout=20") {
		t.Fatalf("poll %q", j.path)
	}
	a.finished(ok(`[{"update_id":41,"message":{"message_id":7,"from":{"id":5,"is_bot":false,"first_name":"Alice"},` +
		`"chat":{"id":5,"first_name":"Alice","last_name":"Liddell","type":"private"},"date":1,` +
		`"text":"hi \u00e9 \ud83d\ude00 run ls now","entities":[{"offset":12,"length":2,"type":"code"}]}}]`))
	return a, f
}

func TestPollAndShow(t *testing.T) {
	a, f := chatting(t)
	if a.offset != 42 || len(a.chats) != 1 || a.chats[0].name != "Alice Liddell" || a.chats[0].id != "5" || a.cur != 0 {
		t.Fatalf("offset %d chats %+v", a.offset, a.chats)
	}
	// é is in the font; the emoji is not; "ls" is inline code at UTF-16
	// offset 12, after the two units of the emoji.
	if m := a.msgAt(0); m.text != "Alice: hi \x82 ? run \x03ls\x03 now" || m.id != 7 || m.mine {
		t.Fatalf("message %q", m.text)
	}
	if f.attention != 1 {
		t.Fatal("no attention for a message in")
	}
	if j := f.next(a); !strings.Contains(j.path, "offset=42") {
		t.Fatalf("next poll %q", j.path)
	}
	a.paint()
	if a.cv.px[0] != lightGrey || a.cv.px[6*a.width+4] != blue {
		t.Fatal("the chat list is not drawn")
	}
	if a.rowSeq[a.visibleRows-1] != 0 {
		t.Fatalf("the last row shows message %d", a.rowSeq[a.visibleRows-1])
	}
	snapshot(t, a, "chat")
}

func TestSendInterruptsThePoll(t *testing.T) {
	a, f := chatting(t)
	f.next(a) // the poll, in flight
	typeText(a, "see `x` and ```\ny\n```")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	if f.cancels != 1 || len(a.outbox) != 1 || len(a.input) != 0 {
		t.Fatalf("cancels %d outbox %d", f.cancels, len(a.outbox))
	}
	a.idle()
	if len(f.jobs) != 3 {
		t.Fatal("a request started while the poll was still out")
	}
	a.finished(result{err: errStopped})
	if a.status != "Sending..." {
		t.Fatalf("the stopped poll said %q", a.status)
	}
	j := f.next(a)
	form, _ := url.ParseQuery(string(j.body))
	if method(j) != "sendMessage" || form.Get("chat_id") != "5" || form.Get("text") != "see x and y" ||
		form.Get("entities") != `[{"type":"code","offset":4,"length":1},{"type":"pre","offset":10,"length":1}]` {
		t.Fatalf("sent %s %q", method(j), j.body)
	}
	a.finished(ok(`{"message_id":8,"from":{"id":1,"is_bot":true,"first_name":"R2","username":"r2bot"},` +
		`"chat":{"id":5,"first_name":"Alice"},"text":"see x and y"}`))
	if m := a.msgAt(1); !m.mine || m.text != "krusty: see x and y" || len(a.outbox) != 0 {
		t.Fatalf("sent message shown as %q", m.text)
	}
	// One that does not go: shown as not sent.
	typeText(a, "lost")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	f.next(a)
	a.finished(result{err: errors.New("TCP api.telegram.org: timeout")})
	if m := a.msgAt(2); m.text != "not sent: lost" || a.retryAt != 5000 {
		t.Fatalf("lost message %q retry %d", m.text, a.retryAt)
	}
}

func TestMenuReactAndReply(t *testing.T) {
	a, f := chatting(t)
	a.paint()
	a.key(&hosted.Key{Down: true, IsChar: true, Char: ' ', LeftControl: true}, clipboard{})
	if a.menuSeq != 0 || a.menuSel != 0 {
		t.Fatalf("menu on %d at %d", a.menuSeq, a.menuSel)
	}
	a.paint()
	snapshot(t, a, "menu")
	a.key(keyChar('4'), clipboard{}) // fire
	if a.menuSeq != -1 || len(a.reactOut) != 1 {
		t.Fatal("no reaction queued")
	}
	j := f.next(a)
	form, _ := url.ParseQuery(string(j.body))
	if method(j) != "setMessageReaction" || form.Get("message_id") != "7" || form.Get("reaction") != `[{"type":"emoji","emoji":"🔥"}]` {
		t.Fatalf("reaction %q", j.body)
	}
	a.finished(ok("true"))
	if a.msgAt(0).myReact != 3 || reactLine(a.msgAt(0)) != "   me: fire" {
		t.Fatalf("reaction kept as %q", reactLine(a.msgAt(0)))
	}
	// Someone else's, counted under it.
	f.next(a)
	a.finished(ok(`[{"update_id":42,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[],` +
		`"new_reaction":[{"type":"emoji","emoji":"❤"}]}}]`))
	if reactLine(a.msgAt(0)) != "  <3   me: fire" {
		t.Fatalf("reactions %q", reactLine(a.msgAt(0)))
	}

	// A right click on the message: the menu there; R replies.
	a.paint()
	a.click(a.msgX+10, a.msgTop+(a.visibleRows-2)*rowH+2, true)
	if a.menuSeq != 0 {
		t.Fatal("right click did not open the menu")
	}
	a.key(keyChar('r'), clipboard{})
	if a.replyID != 7 || a.replyChat != 0 {
		t.Fatal("not replying")
	}
	a.paint()
	snapshot(t, a, "reply")
	typeText(a, "yes")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	j = f.next(a)
	if form, _ := url.ParseQuery(string(j.body)); form.Get("reply_parameters") != `{"message_id":7,"allow_sending_without_reply":true}` {
		t.Fatalf("reply %q", j.body)
	}
}

func TestCopyPaste(t *testing.T) {
	a, f := chatting(t)
	a.key(&hosted.Key{Down: true, IsChar: true, Char: 'c', LeftControl: true}, clipboard{})
	if len(f.copied) != 1 || f.copied[0] != "hi \x82 ? run ls now" {
		t.Fatalf("copied %q", f.copied)
	}
	// A GIF in a message, copied and pasted back: sent again by its id.
	f.next(a)
	a.finished(ok(`[{"update_id":50,"message":{"message_id":9,"from":{"first_name":"Alice"},"chat":{"id":5},` +
		`"animation":{"file_id":"GIFID","mime_type":"video/mp4","file_size":9000,"thumbnail":{"file_id":"THUMB"}},` +
		`"document":{"file_id":"GIFID"}}}]`))
	if m := a.msgAt(1); m.text != "Alice: [GIF]" || len(a.wanted) != 2 || a.wanted[0].fileID != "THUMB" || a.wanted[1].kind != wMP4 {
		t.Fatalf("GIF message %q wanted %+v", m.text, a.wanted)
	}
	a.openMenu(1, true)
	a.key(keyChar('g'), clipboard{})
	if f.copied[len(f.copied)-1] != "[GIF]" || a.clipGif != "GIFID" {
		t.Fatal("GIF not copied")
	}
	a.key(&hosted.Key{Down: true, IsChar: true, Char: 'v', LeftControl: true}, clipboard{text: "[GIF]"})
	if a.attachGif != "GIFID" {
		t.Fatal("GIF not attached")
	}
	typeText(a, "again")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	for i := 0; i < 2; i++ { // the pictures wanted go after what is typed
		if j := f.next(a); method(j) == "sendAnimation" {
			form, _ := url.ParseQuery(string(j.body))
			if form.Get("animation") != "GIFID" || form.Get("caption") != "again" {
				t.Fatalf("GIF sent as %q", j.body)
			}
			return
		}
		a.finished(result{err: errStopped})
	}
	t.Fatal("the GIF never went")
}

func TestScreenshot(t *testing.T) {
	a, f := chatting(t)
	pngData := append([]byte("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x02\x80\x00\x00\x01\x90"), make([]byte, 40)...)
	f.files["/mnt/tmp/CLIP.PNG"] = append(append([]byte(nil), pngData...), "stale tail"...)
	a.key(&hosted.Key{Down: true, IsChar: true, Char: 'v', LeftControl: true},
		clipboard{image: "/mnt/tmp/CLIP.PNG", imageLen: len(pngData)})
	if len(a.attachment) != len(pngData) || a.attachW != 640 || a.attachH != 400 {
		t.Fatalf("attached %d bytes, %dx%d: %q", len(a.attachment), a.attachW, a.attachH, a.status)
	}
	typeText(a, "look")
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	j := f.next(a)
	if method(j) != "sendPhoto" || !strings.HasPrefix(j.contentType, "multipart/form-data; boundary=") ||
		!strings.Contains(string(j.body), "name=\"caption\"\r\n\r\nlook\r\n") || !strings.Contains(string(j.body), string(pngData)) ||
		strings.Contains(string(j.body), "stale") {
		t.Fatalf("photo body %q", j.body)
	}
}

func TestEsc(t *testing.T) {
	a, f := chatting(t)
	a.replyTo(0)
	a.key(&hosted.Key{Down: true, Escape: true}, clipboard{})
	if a.replyID != 0 || f.closed {
		t.Fatal("Esc did not just stop replying")
	}
	a.key(&hosted.Key{Down: true, Escape: true}, clipboard{})
	if !f.closed {
		t.Fatal("Esc did not close")
	}
}

// snapshot writes the window as a PNG when TELEGRAM_SNAPSHOTS names a
// directory, to look at.
func snapshot(t *testing.T, a *App, name string) {
	dir := os.Getenv("TELEGRAM_SNAPSHOTS")
	if dir == "" {
		return
	}
	ega := [16][3]uint8{{0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170}, {170, 0, 0}, {170, 0, 170},
		{170, 85, 0}, {170, 170, 170}, {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
		{255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}}
	pal := color.Palette{}
	for _, c := range ega {
		pal = append(pal, color.RGBA{c[0], c[1], c[2], 255})
	}
	img := image.NewPaletted(image.Rect(0, 0, a.cv.w, a.cv.h), pal)
	copy(img.Pix, a.cv.px)
	out, err := os.Create(filepath.Join(dir, name+".png"))
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	if err := png.Encode(out, img); err != nil {
		t.Fatal(err)
	}
}
