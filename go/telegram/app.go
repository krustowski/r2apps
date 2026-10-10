package main

import (
	"strconv"
	"strings"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
	"github.com/krustowski/rou2exOS-apps/go/telegram/media"
)

// The client: Memento's TelegramWindow, out of Memento.  It is a bot: ask
// @BotFather for one, type the token it gives you, and anyone who writes to
// the bot shows up as a chat on the left; what you type is sent back as the
// bot.  All of it runs on one goroutine; requests go to the network worker
// (job) and come back (result) one at a time.

// magic is in front of the block Memento's Telegram window hands over (host.h).
const magic = 0x31474554 // "TEG1"

const (
	shippedToken = "/mnt/tar/opt/memento/telegram.txt"
	typedToken   = "/mnt/fat/TELEGRAM.CFG"

	tokenCap = 64
	inCap    = 200
	textCap  = 480
	maxMsgs  = 96
	maxChats = 12
	maxOut   = 4
	nameCap  = 28

	maxReactOut = 4
	maxReacts   = 4 // kinds of reaction kept per message
	rowsCap     = 128
	fileIDCap   = 128

	// Photos: those still to fetch, and the decoded ones, each kept for the
	// message it belongs to.
	maxWanted     = 8
	maxPhotos     = 8
	photoMaxH     = 200             // pixels
	gifBudget     = 2 * 1024 * 1024 // the frames of one GIF, at most
	gifMaxBytes   = 3 * 1024 * 1024 // a bigger file is shown by Telegram's still
	maxScreenshot = 8 * 1024 * 1024
	maxAnimated   = 3  // GIFs that move at once
	maxGifRefs    = 16 // GIFs of recent messages, for Copy GIF
)

// Geometry, in pixels: Memento draws a window at 2 pixels a unit, and its
// 6-unit font is the 6x12 Terminus.
const (
	cw     = glyphW
	ch     = glyphH
	rowH   = ch + 2
	chatsW = 144
)

// The reactions offered, 1-9 in the menu.  Bots may use only the standard
// set, one at a time on a message.  The font has no emoji, so each has a few
// letters to be shown by.
type reaction struct {
	cp    rune   // its first code point, to know it again
	utf8  string // as sent
	label string // as shown
	name  string
}

var reactions = [...]reaction{
	{0x2764, "❤", "<3", "heart"}, {0x1F44D, "\U0001F44D", "+1", "thumbs up"},
	{0x1F44E, "\U0001F44E", "-1", "thumbs down"}, {0x1F525, "\U0001F525", "fire", "fire"},
	{0x1F601, "\U0001F601", ":D", "grin"}, {0x1F914, "\U0001F914", "hmm", "thinking"},
	{0x1F622, "\U0001F622", ":'(", "crying"}, {0x1F389, "\U0001F389", "yay", "party"},
	{0x1F44C, "\U0001F44C", "ok", "OK"},
}

const (
	otherReaction = len(reactions) // anything else, shown as "*"

	// The context menu: the reactions first, then these.
	miRemove = len(reactions) + iota
	miSep
	miReply
	miCopy
	miCopyGif
	miCount
)

type request int

const (
	rqNone request = iota
	rqGetMe
	rqPoll
	rqSend
	rqFileInfo  // getFile: where a photo is
	rqFile      // the photo itself
	rqSendPhoto // a screenshot going out
	rqReact     // setMessageReaction
)

type chat struct {
	id     string // as the API writes it; groups are negative
	name   string
	unread int
}

type reactCount struct{ r, n int } // r: index into reactions or otherReaction, -1 unused

type msg struct {
	chat    int
	mine    bool
	photo   int   // index into photos, or -1
	id      int64 // Telegram's message_id in its chat; 0 for one never sent
	myReact int   // the bot's reaction, index into reactions, or -1
	reacts  [maxReacts]reactCount
	text    string // "Name: what they said", in the font's code page
}

// Pictures to fetch.
const (
	wStill = iota // a photo, or the still Telegram has of a GIF
	wGIF          // a GIF file: every frame
	wMP4          // a GIF as Telegram keeps most of them: an H.264 MP4
)

type wanted struct {
	seq    int
	kind   int
	fileID string
	path   string // filled in by getFile
}

type photo struct {
	seq   int // -1: a free slot
	pic   media.Picture
	anim  media.Animation
	shown int // the frame of anim last painted, -1 when not on screen
}

func (p *photo) any() bool { return p.pic.Px != nil || p.anim.Px != nil }
func (p *photo) size() (int, int) {
	if p.anim.Px != nil {
		return p.anim.W, p.anim.H
	}
	return p.pic.W, p.pic.H
}
func (p *photo) release() {
	p.pic.Release()
	p.anim.Release()
	p.shown = -1
}

type gifRef struct {
	seq int
	id  string
}

type outMsg struct {
	chat    int
	replyTo int64  // message_id answered, 0 for none
	gif     string // a GIF to send with text as its caption, "" for text
	text    string
}

type react struct {
	chat  int
	msgID int64
	r     int // -1: none
}

type App struct {
	// What the client needs from where it runs.
	now       func() uint64 // milliseconds
	readFile  func(path string, limit int) ([]byte, error)
	writeFile func(path string, data []byte) error
	user      func() string     // who is at the keyboard
	submit    func(job)         // to the network worker
	cancel    func()            // the request in flight is to stop
	copyText  func(string) bool // onto Memento's clipboard
	attention func()
	notify    func(string) bool // a clock bubble; false while Memento's queue is full
	notices   []string
	closeWin  func()
	log       func(string) // a line for the diagnostic log
	colours   int

	dirty                bool // to be painted again
	notificationsEnabled bool
	settingsOpen         bool
	settingsNote         string

	token   string
	api     string // "https://api.telegram.org", or what the configuration says
	setup   bool   // asking for the token
	botName string
	offset  int64

	request request // in flight
	stale   bool    // and its answer is no longer wanted
	retryAt uint64

	reactOut []react

	// The context menu: the message by its number, -1 when closed; the item
	// under the bar; its geometry as last painted, for the mouse.
	menuSeq, menuSel                 int
	menuMoved                        bool
	menuL, menuT, menuW, menuAnchorY int
	rowSeq                           [rowsCap]int // the message each row showed

	// A screenshot pasted in (PNG), waiting for Enter; and one on its way.
	attachment         []byte
	attachW, attachH   int
	photoBody          [][]byte
	photoQueued        bool
	photoChat          int
	photoCaption       string
	attachGif, clipGif string // a GIF pasted in, and one copied here, by file_id

	replyChat, replySeq int
	replyID             int64

	chats    []chat
	cur      int // the chat on the right
	msgs     [maxMsgs]msg
	msgTotal int // ever added; the last maxMsgs of them are kept
	wanted   []wanted
	photos   [maxPhotos]photo
	mp4      media.Mp4
	mp4Seq   int
	gifRefs  [maxGifRefs]gifRef
	nextGif  int
	outbox   []outMsg

	input       []byte
	scroll      int // wrapped lines up from the bottom
	status      string
	visibleRows int

	// As last painted.
	width, height      int
	msgX, msgW, msgTop int
	cv                 canvas
	lines              []line
}

func newApp() *App {
	a := &App{log: func(string) {}, api: "https://api.telegram.org", setup: true, notificationsEnabled: true, cur: -1, menuSeq: -1, replyChat: -1, replySeq: -1,
		mp4Seq: -1, width: 600, height: 340, msgW: 400, visibleRows: 1, dirty: true,
		lines: make([]line, 0, textCap)}
	for i := range a.photos {
		a.photos[i] = photo{seq: -1, shown: -1}
	}
	for i := range a.gifRefs {
		a.gifRefs[i].seq = -1
	}
	return a
}

func (a *App) repaint()           { a.dirty = true }
func (a *App) say(s string)       { a.status = s; a.dirty = true }
func (a *App) oldest() int        { return max(a.msgTotal-maxMsgs, 0) }
func (a *App) msgAt(seq int) *msg { return &a.msgs[seq%maxMsgs] }

// capped is s cut to fit a C buffer of n bytes, as the window had them.
func capped(s string, n int) string {
	if len(s) > n-1 {
		return s[:n-1]
	}
	return s
}

// ── Token ──────────────────────────────────────────────────────────────────

// start reads the token (the boot medium's, else the one typed in before)
// and starts reading the bot's chats when there is one.
func (a *App) start() {
	a.loadSettings()
	if !a.loadToken(shippedToken) {
		a.loadToken(typedToken)
	}
	// The address of the API may be given for tests: "api=http://host:port".
	for _, path := range []string{shippedToken, typedToken} {
		if _, api := a.readConfig(path); api != "" {
			a.api = api
		}
	}
	if a.token != "" {
		a.startChatting()
	}
}

func (a *App) readConfig(path string) (token, api string) {
	b, err := a.readFile(path, 512)
	if err != nil {
		return "", ""
	}
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(strings.TrimRight(l, "\x00"))
		if v, ok := strings.CutPrefix(l, "api="); ok {
			api = strings.TrimRight(strings.TrimSpace(v), "/")
		} else if token == "" {
			// The file is written a whole sector at a time: the token ends
			// at the first thing that cannot be in one.
			n := 0
			for n < len(l) && n < tokenCap-1 && tokenChar(l[n]) {
				n++
			}
			token = l[:n]
		}
	}
	return token, api
}

func (a *App) loadToken(path string) bool {
	t, _ := a.readConfig(path)
	if !validToken(t) {
		return false
	}
	a.token = t
	return true
}

func tokenChar(c byte) bool {
	return c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c == ':' || c == '_' || c == '-'
}

// validToken: "123456789:AAH...": the bot's number, a colon and a secret.
func validToken(t string) bool {
	digits := 0
	for digits < len(t) && t[digits] >= '0' && t[digits] <= '9' {
		digits++
	}
	if digits == 0 || digits >= len(t) || t[digits] != ':' || len(t)-digits-1 < 20 {
		return false
	}
	for i := 0; i < len(t); i++ {
		if !tokenChar(t[i]) {
			return false
		}
	}
	return true
}

func (a *App) saveToken() {
	text := a.token + "\n"
	if _, api := a.readConfig(typedToken); api != "" {
		text += "api=" + api + "\n"
	}
	_ = a.writeFile(typedToken, []byte(text))
}

func (a *App) startChatting() {
	a.setup = false
	a.botName = ""
	a.retryAt = 0
	a.say("Checking the token...")
}

func (a *App) backToSetup(why string) {
	if a.request != rqNone {
		a.stale = true
		a.cancel()
	}
	a.setup = true
	a.say(why)
	a.input = append(a.input[:0], a.token...)
}

// ── Chats and messages ─────────────────────────────────────────────────────

func (a *App) chatFor(id, name string) int {
	for i := range a.chats {
		if a.chats[i].id == id {
			if name != "" {
				a.chats[i].name = name
			}
			return i
		}
	}
	// Full: the last chat on the list makes room.  Its messages stay in the
	// ring but belong to nobody and are no longer shown.
	at := len(a.chats)
	if at < maxChats {
		a.chats = append(a.chats, chat{})
	} else {
		at = maxChats - 1
		for i := range a.msgs {
			if a.msgs[i].chat == at {
				a.msgs[i].chat = -1
			}
		}
	}
	if name == "" {
		name = id
	}
	a.chats[at] = chat{id: id, name: capped(name, nameCap)}
	if a.cur < 0 {
		a.cur = at
	}
	return at
}

func (a *App) addMsg(chat int, mine bool, who, text string, id int64, quoted string) {
	m := a.msgAt(a.msgTotal)
	if a.msgTotal >= maxMsgs {
		if m.photo >= 0 {
			a.photos[m.photo].release()
			a.photos[m.photo].seq = -1
		}
		if a.menuSeq == a.msgTotal-maxMsgs {
			a.menuSeq = -1 // the one the menu is on leaves the ring
		}
		if a.replySeq == a.msgTotal-maxMsgs {
			a.replySeq = -1 // still answered, but no longer shown above the input
		}
	}
	*m = msg{chat: chat, mine: mine, photo: -1, id: id, myReact: -1}
	for i := range m.reacts {
		m.reacts[i].r = -1
	}
	var t strings.Builder
	if quoted != "" {
		t.WriteByte(quote)
		t.WriteString(quoted)
		t.WriteByte('\n')
	}
	t.WriteString(who)
	t.WriteString(": ")
	t.WriteString(text)
	m.text = capped(t.String(), textCap)
	a.msgTotal++
	if chat == a.cur {
		a.scroll = 0
	} else if chat >= 0 && !mine {
		a.chats[chat].unread++
	}
	// Incoming messages get a clock bubble as well as window attention.
	if !mine {
		a.alert(chat, who+": "+text)
	}
	a.dirty = true
}

// takeMessage shows one Message object: a chat to put it in, who wrote it
// and what.
func (a *App) takeMessage(b []byte, m int) {
	chatObj := jmember(b, m, "chat")
	id, ok := jraw(b, jmember(b, chatObj, "id"))
	if !ok {
		return
	}
	name, ok := jstr(b, jmember(b, chatObj, "title"), nameCap, false, nil)
	if !ok {
		name, _ = jstr(b, jmember(b, chatObj, "first_name"), nameCap, false, nil)
		if last, ok := jstr(b, jmember(b, chatObj, "last_name"), nameCap, false, nil); ok && last != "" {
			name = capped(name+" "+last, nameCap)
		}
	}
	from := jmember(b, m, "from")
	who, _ := jstr(b, jmember(b, from, "first_name"), nameCap, false, nil)
	mine := jtrue(b, jmember(b, from, "is_bot")) && a.botName != "" && a.usernameIs(b, from)
	if who == "" {
		who = name
		if who == "" {
			who = "?"
		}
	}
	text, isText := jstr(b, jmember(b, m, "text"), textCap, true, codeMarks(b, jmember(b, m, "entities")))
	mediaLabel := a.wantMedia(b, m)
	if !isText {
		// Not text: say what it was, and its caption if it has one.
		text = "[something this client cannot show]"
		for _, k := range [...][2]string{
			{"photo", "[photo]"}, {"sticker", "[sticker]"}, {"voice", "[voice message]"},
			{"video", "[video]"}, {"video_note", "[video]"}, {"animation", "[GIF]"},
			{"document", "[file]"}, {"audio", "[audio]"}, {"location", "[location]"},
			{"contact", "[contact]"}, {"poll", "[poll]"}, {"new_chat_members", "[joined]"},
			{"left_chat_member", "[left]"},
		} {
			if jmember(b, m, k[0]) >= 0 {
				text = k[1]
				break
			}
		}
		if mediaLabel != "" {
			text = mediaLabel
		}
		marks := codeMarks(b, jmember(b, m, "caption_entities"))
		if caption, ok := jstr(b, jmember(b, m, "caption"), textCap, true, marks); ok && caption != "" {
			text = capped(text+" "+caption, textCap)
		}
	}
	quoted := a.quoteOf(b, jmember(b, m, "reply_to_message"))
	if mine {
		who = a.myName()
	}
	a.addMsg(a.chatFor(id, name), mine, who, text, jnumber(b, jmember(b, m, "message_id")), quoted)
}

// quoteOf is the line over a reply: "Name: the start of what it answers"
// ("" when it answers nothing).
func (a *App) quoteOf(b []byte, r int) string {
	if r < 0 {
		return ""
	}
	from := jmember(b, r, "from")
	who, _ := jstr(b, jmember(b, from, "first_name"), nameCap, false, nil)
	if jtrue(b, jmember(b, from, "is_bot")) && a.botName != "" && a.usernameIs(b, from) {
		who = capped(a.myName(), nameCap)
	}
	if who == "" {
		who, _ = jstr(b, jmember(b, jmember(b, r, "chat"), "title"), nameCap, false, nil)
	}
	what, ok := jstr(b, jmember(b, r, "text"), 64, false, nil)
	if !ok {
		what, ok = jstr(b, jmember(b, r, "caption"), 64, false, nil)
	}
	if !ok {
		switch {
		case jmember(b, r, "animation") >= 0:
			what = "[GIF]"
		case jmember(b, r, "photo") >= 0:
			what = "[photo]"
		default:
			what = "[...]"
		}
	}
	if who == "" {
		who = "?"
	}
	return capped(who+": "+what, 64)
}

// codeMarks are the code and pre entities of a text, as the marks that go
// into it: sorted by position, what closes before what opens at the same
// place.
func codeMarks(b []byte, ents int) []mark {
	type m struct {
		mark
		opens bool
	}
	var ms []m
	for en := jfirst(b, ents); en >= 0 && len(ms)+2 <= 16; en = jnext(b, en) {
		typ, _ := jstr(b, jmember(b, en, "type"), 8, false, nil)
		pre := typ == "pre"
		if !pre && typ != "code" {
			continue
		}
		off, n := int(jnumber(b, jmember(b, en, "offset"))), int(jnumber(b, jmember(b, en, "length")))
		if off < 0 || n <= 0 {
			continue
		}
		open, shut := byte(code), byte(code)
		if pre {
			open, shut = preOn, preOff
		}
		ms = append(ms, m{mark{off, open}, true}, m{mark{off + n, shut}, false})
	}
	// Insertion sort, stable; an opening mark goes after a closing one at
	// the same position.
	for i := 1; i < len(ms); i++ {
		for j := i; j > 0; j-- {
			x, y := ms[j-1], ms[j]
			if !(x.at > y.at || x.at == y.at && x.opens && !y.opens) {
				break
			}
			ms[j-1], ms[j] = y, x
		}
	}
	out := make([]mark, len(ms))
	for i := range ms {
		out[i] = ms[i].mark
	}
	return out
}

// wantMedia notes a picture in message m, to be fetched for the message
// about to be added, and says what to call it instead of what its kind says
// ("" for that).
//
// A photo: the size nearest 320 pixels wide that is not wider (the smallest
// when all are).  A GIF: the file, when Telegram kept it as one.  Mostly it
// did not: a GIF sent from its apps becomes a silent H.264 MP4 (an
// "animation" whose mime_type is video/mp4).  Telegram's still of it comes
// first, then the MP4, decoded to move in place of the still.  A GIF sent as
// a file is a document.
func (a *App) wantMedia(b []byte, m int) string {
	anim := jmember(b, m, "animation")
	doc := anim
	if doc < 0 {
		doc = jmember(b, m, "document")
	}
	if doc >= 0 {
		mime, _ := jstr(b, jmember(b, doc, "mime_type"), 32, false, nil)
		gif := mime == "image/gif"
		if anim < 0 && !gif {
			return "" // a file of some other kind
		}
		if anim >= 0 {
			if id, _ := jstr(b, jmember(b, anim, "file_id"), fileIDCap, false, nil); id != "" {
				a.gifRefs[a.nextGif%maxGifRefs] = gifRef{seq: a.msgTotal, id: id}
				a.nextGif++
			}
		}
		fits := jnumber(b, jmember(b, doc, "file_size")) <= gifMaxBytes
		id, _ := jstr(b, jmember(b, doc, "file_id"), fileIDCap, false, nil)
		if gif && fits && id != "" {
			a.want(id, wGIF)
			return "[GIF]"
		}
		thumb := jmember(b, doc, "thumbnail")
		if thumb < 0 {
			thumb = jmember(b, doc, "thumb") // what the API called it before 6.6
		}
		if t, _ := jstr(b, jmember(b, thumb, "file_id"), fileIDCap, false, nil); t != "" {
			a.want(t, wStill)
		}
		if !gif && fits && id != "" {
			a.want(id, wMP4)
		}
		if fits {
			return "[GIF]"
		}
		return "[GIF, too big: a still of it]"
	}
	sizes := jmember(b, m, "photo")
	best, bestW := "", -1
	for ps := jfirst(b, sizes); ps >= 0; ps = jnext(b, ps) {
		id, ok := jstr(b, jmember(b, ps, "file_id"), fileIDCap, false, nil)
		if _, haveW := jraw(b, jmember(b, ps, "width")); !ok || !haveW {
			continue
		}
		w := int(jnumber(b, jmember(b, ps, "width")))
		if bestW < 0 || w <= 320 && (bestW > 320 || w > bestW) || w > 320 && bestW > 320 && w < bestW {
			best, bestW = id, w
		}
	}
	if best != "" {
		a.want(best, wStill)
	}
	return ""
}

func (a *App) want(fileID string, kind int) {
	if len(a.wanted) == maxWanted {
		a.wanted = append(a.wanted[:0], a.wanted[1:]...)
	}
	a.wanted = append(a.wanted, wanted{seq: a.msgTotal, kind: kind, fileID: fileID})
}

func (a *App) dropWanted() {
	if len(a.wanted) > 0 {
		a.wanted = append(a.wanted[:0], a.wanted[1:]...)
	}
}

// gifOf is the GIF of message seq, by file_id, or "".
func (a *App) gifOf(seq int) string {
	for _, g := range a.gifRefs {
		if g.seq == seq && seq >= 0 {
			return g.id
		}
	}
	return ""
}

// myName is what the bot's messages --- what is typed here --- are signed
// with on this side: the system user, or "me" when there is none.
func (a *App) myName() string {
	if u := a.user(); u != "" {
		return u
	}
	return "me"
}

func (a *App) usernameIs(b []byte, from int) bool {
	u, ok := jstr(b, jmember(b, from, "username"), 40, false, nil)
	return ok && u == a.botName
}

// ── Reactions ──────────────────────────────────────────────────────────────

// findMsg is message id in the chat whose object is at chatObj, if it is
// still in the ring.
func (a *App) findMsg(b []byte, chatObj int, id int64) *msg {
	cid, ok := jraw(b, jmember(b, chatObj, "id"))
	if id == 0 || !ok {
		return nil
	}
	c := -1
	for i := range a.chats {
		if a.chats[i].id == cid {
			c = i
		}
	}
	for k := a.msgTotal - 1; c >= 0 && k >= a.oldest(); k-- {
		if m := a.msgAt(k); m.chat == c && m.id == id {
			return m
		}
	}
	return nil
}

// reactionOf a ReactionType object: which of reactions, otherReaction for a
// custom emoji, a paid reaction or one not on the list.
func reactionOf(b []byte, rt int) int {
	if cp := jrunes(b, jmember(b, rt, "emoji"), 1); len(cp) > 0 {
		for i, r := range reactions {
			if r.cp == cp[0] {
				return i
			}
		}
	}
	return otherReaction
}

// countReact counts n more (or fewer) of reaction r on m.
func countReact(m *msg, r, n int) {
	at, spare := -1, -1
	for i := range m.reacts {
		if m.reacts[i].r == r {
			at = i
		} else if m.reacts[i].r < 0 && spare < 0 {
			spare = i
		}
	}
	if at < 0 {
		if n <= 0 || spare < 0 {
			return
		}
		at = spare
		m.reacts[at] = reactCount{r: r}
	}
	m.reacts[at].n = min(max(m.reacts[at].n+n, 0), 255)
	if m.reacts[at].n == 0 {
		m.reacts[at].r = -1
	}
}

// takeReaction: someone changed theirs.  Every reaction they took off is
// one fewer, every one put on one more.
func (a *App) takeReaction(b []byte, u int) {
	m := a.findMsg(b, jmember(b, u, "chat"), jnumber(b, jmember(b, u, "message_id")))
	if m == nil {
		return
	}
	for r := jfirst(b, jmember(b, u, "old_reaction")); r >= 0; r = jnext(b, r) {
		kind := reactionOf(b, r)
		countReact(m, kind, -1)
	}
	var changes [otherReaction + 1]int
	for r := jfirst(b, jmember(b, u, "new_reaction")); r >= 0; r = jnext(b, r) {
		kind := reactionOf(b, r)
		countReact(m, kind, 1)
		key := reactionKey(b, r)
		found := false
		for old := jfirst(b, jmember(b, u, "old_reaction")); old >= 0; old = jnext(b, old) {
			if reactionKey(b, old) == key {
				found = true
				break
			}
		}
		if !found {
			changes[kind]++
		}
	}
	user := jmember(b, u, "user")
	if a.usernameIs(b, user) {
		return
	}
	who, _ := jstr(b, jmember(b, user, "first_name"), nameCap, false, nil)
	if who == "" {
		who, _ = jstr(b, jmember(b, jmember(b, u, "actor_chat"), "title"), nameCap, false, nil)
	}
	if who == "" {
		who = "Someone"
	}
	a.reactionAlert(m, who, changes)
}

// takeReactionCount: the totals of anonymous reactions (channels, groups
// with anonymous admins), which replace what was counted.
func (a *App) takeReactionCount(b []byte, u int) {
	m := a.findMsg(b, jmember(b, u, "chat"), jnumber(b, jmember(b, u, "message_id")))
	if m == nil {
		return
	}
	var changes [otherReaction + 1]int
	for _, r := range m.reacts {
		if r.r >= 0 {
			changes[r.r] -= r.n
		}
	}
	for i := range m.reacts {
		m.reacts[i].r = -1
	}
	for r := jfirst(b, jmember(b, u, "reactions")); r >= 0; r = jnext(b, r) {
		countReact(m, reactionOf(b, jmember(b, r, "type")), int(jnumber(b, jmember(b, r, "total_count"))))
	}
	for _, r := range m.reacts {
		if r.r >= 0 {
			changes[r.r] += r.n
		}
	}
	a.reactionAlert(m, "Someone", changes)
}

// reactLine is what is under a message with reactions, e.g. "  <3 2  +1
// me: fire"; "" when it has none.
func reactLine(m *msg) string {
	var s strings.Builder
	for _, r := range m.reacts {
		if r.r < 0 {
			continue
		}
		s.WriteString("  ")
		if r.r < len(reactions) {
			s.WriteString(reactions[r.r].label)
		} else {
			s.WriteString("*")
		}
		if r.n > 1 {
			s.WriteString(" " + strconv.Itoa(r.n))
		}
	}
	if m.myReact >= 0 {
		s.WriteString("   me: " + reactions[m.myReact].label)
	}
	return capped(s.String(), 64)
}

// react sets (or with r = -1 takes off) the bot's reaction on message seq.
func (a *App) react(seq, r int) {
	m := a.msgAt(seq)
	if m.id == 0 || m.chat < 0 {
		a.say("That message never reached Telegram.")
		return
	}
	if len(a.reactOut) == maxReactOut {
		a.say("Still sending the last few reactions; wait a moment.")
		return
	}
	a.reactOut = append(a.reactOut, react{chat: m.chat, msgID: m.id, r: r})
	a.interruptPoll()
	a.say("Reacting...")
}

func (a *App) dropReact() {
	if len(a.reactOut) > 0 {
		a.reactOut = append(a.reactOut[:0], a.reactOut[1:]...)
	}
}

// ── The context menu ───────────────────────────────────────────────────────

// openMenu on message seq (-1: the newest in the chat on the right).
func (a *App) openMenu(seq int, fromKey bool) {
	for k := a.msgTotal - 1; seq < 0 && k >= a.oldest(); k-- {
		if a.msgAt(k).chat == a.cur {
			seq = k
		}
	}
	if seq < 0 {
		a.say("No message here to open a menu on.")
		return
	}
	a.menuSeq, a.menuMoved = seq, fromKey
	a.menuSel = 0
	if mr := a.msgAt(seq).myReact; mr >= 0 {
		a.menuSel = mr
	}
	if !a.menuEnabled(a.menuSel) {
		a.menuSel = miCopy
	}
	a.repaint()
}

func (a *App) closeMenu() {
	a.menuSeq = -1
	a.repaint()
}

// menuToMessage moves the menu to the message before (dir -1) or after (+1)
// its own in the chat on the right.
func (a *App) menuToMessage(dir int) {
	for k := a.menuSeq + dir; k >= a.oldest() && k < a.msgTotal; k += dir {
		if a.msgAt(k).chat == a.cur {
			a.openMenu(k, true)
			return
		}
	}
}

func (a *App) menuEnabled(i int) bool {
	m := a.msgAt(a.menuSeq)
	switch {
	case i < len(reactions):
		return m.id != 0 // only what reached Telegram has a number to react to
	case i == miRemove:
		return m.id != 0 && m.myReact >= 0
	case i == miReply:
		return m.id != 0 && m.chat >= 0
	case i == miCopyGif:
		return a.gifOf(a.menuSeq) != ""
	}
	return i == miCopy
}

func (a *App) moveMenuSel(dir int) {
	for n := 0; n < miCount; n++ {
		a.menuSel = (a.menuSel + dir + miCount) % miCount
		if a.menuEnabled(a.menuSel) {
			break
		}
	}
	a.repaint()
}

// menuLabel is an item's label, its accelerator after a '&', and what goes
// on the right.
func (a *App) menuLabel(i int) (label, keys string) {
	switch {
	case i < len(reactions):
		label = "&" + string(rune('1'+i)) + " " + reactions[i].label + "  " + reactions[i].name
		if a.msgAt(a.menuSeq).myReact == i {
			keys = "(set)"
		}
	case i == miRemove:
		label = "&0 No reaction"
	case i == miReply:
		label = "&Reply"
	case i == miCopy:
		label, keys = "&Copy text", "Ctrl+C"
	case i == miCopyGif:
		label = "Copy &GIF"
	}
	return
}

func (a *App) pickMenu(i int) {
	if a.menuSeq < 0 || i < 0 || i >= miCount || !a.menuEnabled(i) {
		return
	}
	seq := a.menuSeq
	a.menuSeq = -1
	a.repaint()
	switch {
	case i < len(reactions):
		a.react(seq, i)
	case i == miRemove:
		a.react(seq, -1)
	case i == miReply:
		a.replyTo(seq)
	case i == miCopy:
		a.copyMessage(a.msgAt(seq))
	case i == miCopyGif:
		// Memento's clipboard gets "[GIF]"; pasted back in here, that is
		// this GIF again.
		a.clipGif = a.gifOf(seq)
		a.copyText("[GIF]")
		a.say("Copied the GIF: Ctrl+V in any chat here attaches it.")
	}
}

// replyTo makes what is typed next answer message seq.
func (a *App) replyTo(seq int) {
	m := a.msgAt(seq)
	a.replyChat, a.replyID, a.replySeq = m.chat, m.id, seq
	if a.cur != m.chat {
		a.cur = m.chat
		a.chats[a.cur].unread = 0
		a.scroll = 0
	}
	a.say("Replying: Enter sends, Esc does not reply after all.")
}

func (a *App) cancelReply() { a.replyChat, a.replyID, a.replySeq = -1, 0, -1 }

func (a *App) menuKey(k *hosted.Key) {
	ctrl, alt := k.Ctrl(), k.Alt()
	switch {
	case k.Escape || k.Tab || k.IsChar && k.Char == ' ' && (ctrl || alt):
		a.closeMenu() // the key that opened it closes it again
	case k.ArrowDown:
		a.moveMenuSel(+1)
	case k.ArrowUp:
		a.moveMenuSel(-1)
	case k.PageUp:
		a.menuToMessage(-1)
	case k.PageDown:
		a.menuToMessage(+1)
	case k.Enter || k.IsChar && k.Char == ' ':
		a.pickMenu(a.menuSel)
	case ctrl && k.IsChar && (k.Char == 'c' || k.Char == 'C'):
		a.pickMenu(miCopy)
	case k.IsChar && !ctrl:
		switch c := k.Char; {
		case c >= '1' && c < '1'+byte(len(reactions)):
			a.pickMenu(int(c - '1'))
		case c == '0':
			a.pickMenu(miRemove)
		case c == 'c' || c == 'C':
			a.pickMenu(miCopy)
		case c == 'r' || c == 'R':
			a.pickMenu(miReply)
		case c == 'g' || c == 'G':
			a.pickMenu(miCopyGif)
		}
	}
}

// menuItemAt is the item at (x, y), or -1.
func (a *App) menuItemAt(x, y int) int {
	if x < a.menuL || x >= a.menuL+a.menuW || y < a.menuT+4 {
		return -1
	}
	if i := (y - a.menuT - 4) / rowH; i < miCount {
		return i
	}
	return -1
}

// ── Pictures ───────────────────────────────────────────────────────────────

// slotFor is the slot for message seq's picture: the one it has already (a
// GIF's still, when its frames come), or a free one, or the one kept
// longest, emptied.  -1 when the message has left the ring.
func (a *App) slotFor(seq int) int {
	if seq < a.oldest() {
		return -1
	}
	if have := a.msgAt(seq).photo; have >= 0 && a.photos[have].seq == seq {
		return have
	}
	slot := 0
	for i := range a.photos {
		if a.photos[i].seq < 0 {
			slot = i
			break
		}
		if a.photos[i].seq < a.photos[slot].seq {
			slot = i
		}
	}
	p := &a.photos[slot]
	if p.seq >= a.oldest() && a.msgAt(p.seq).photo == slot {
		a.msgAt(p.seq).photo = -1
	}
	p.release()
	p.seq = -1
	return slot
}

// pictureMaxW is where pictures are made to fit: the width of the messages.
func (a *App) pictureMaxW() int { return max(a.msgW-4, 16) }

// photoArrived decodes a picture and gives it to its message if that is
// still here.
func (a *App) photoArrived(data []byte) {
	w := a.wanted[0]
	slot := a.slotFor(w.seq)
	if slot < 0 {
		return
	}
	p := &a.photos[slot]
	var err error
	// A GIF with every frame; one too big for that as its first.
	if w.kind == wGIF && media.IsGIF(data) {
		if p.anim, err = media.DecodeAnimation(data, a.pictureMaxW(), photoMaxH, a.colours, 0xFFFFFF, gifBudget); err == nil {
			a.keepAnimations(slot)
		}
	}
	if p.anim.Px == nil {
		p.pic, err = media.DecodePicture(data, a.pictureMaxW(), photoMaxH, a.colours, 0xFFFFFF)
	}
	if err != nil {
		if w.kind == wGIF {
			a.say("GIF: " + err.Error())
		} else {
			a.say("Photo: " + err.Error())
		}
		a.log(a.status)
	}
	a.attach(slot, w.seq)
}

func (a *App) attach(slot, seq int) {
	p := &a.photos[slot]
	if !p.any() {
		p.seq = -1
		return
	}
	p.seq = seq
	a.msgAt(seq).photo = slot
	a.repaint()
}

// mp4Arrived starts decoding an MP4, which goes on from the idle loop.
func (a *App) mp4Arrived(data []byte) {
	if a.wanted[0].seq < a.oldest() {
		return
	}
	if err := a.mp4.Start(data, a.pictureMaxW(), photoMaxH, a.colours, 0xFFFFFF, gifBudget); err != nil {
		a.say("GIF: " + err.Error())
		a.log(a.status)
		return
	}
	a.mp4Seq = a.wanted[0].seq
}

// decodeSome decodes some of the MP4, and at the end puts its frames in
// place of the still.
func (a *App) decodeSome() {
	if a.mp4.Step(8, a.now) {
		return
	}
	anim, err := a.mp4.Finish()
	slot := -1
	if err != nil {
		a.say("GIF: " + err.Error())
	} else if slot = a.slotFor(a.mp4Seq); slot >= 0 {
		a.photos[slot].anim.Release()
		a.photos[slot].anim = anim
		a.attach(slot, a.mp4Seq)
		a.keepAnimations(slot)
	} else {
		anim.Release()
	}
	a.mp4Seq = -1
	// A picture waiting behind this one waited for a long poll.
	if len(a.wanted) > 0 {
		a.interruptPoll()
	}
	a.repaint()
}

// keepAnimations: no more than maxAnimated GIFs keep their frames; beyond
// that the oldest (but not keep, which has just come) is shown still --- by
// Telegram's still, or its own first frame when it has none.
func (a *App) keepAnimations(keep int) {
	for {
		n, oldest := 0, -1
		for i := range a.photos {
			if a.photos[i].anim.Px != nil {
				n++
				if i != keep && (oldest < 0 || a.photos[i].seq < a.photos[oldest].seq) {
					oldest = i
				}
			}
		}
		if n <= maxAnimated || oldest < 0 {
			return
		}
		p := &a.photos[oldest]
		if p.pic.Px == nil {
			// Its first frame, as a picture of its own.
			if pic, err := p.anim.Still(0); err == nil {
				p.pic = pic
			}
		}
		p.anim.Release()
		p.shown = -1
		if p.pic.Px == nil {
			p.seq = -1 // nothing left to show: a free slot
		}
	}
}

// ── Requests ───────────────────────────────────────────────────────────────

func (a *App) begin(r request, method, query string, body [][]byte, contentType string) {
	a.request = r
	a.submit(job{kind: r, api: a.api, path: "/bot" + a.token + "/" + method + query, body: body,
		contentType: contentType})
}

func form(s string) [][]byte { return [][]byte{[]byte(s)} }

// What each request is called in the log: never its path, which has the
// token in it.
var requestNames = [...]string{"", "getMe", "getUpdates", "send", "getFile", "file", "sendPhoto", "react"}

// startNext is whatever is to go out next, else the long poll.
func (a *App) startNext() {
	if a.botName == "" {
		a.begin(rqGetMe, "getMe", "", nil, "")
		return
	}
	if len(a.outbox) > 0 {
		// Text, or a GIF by its file_id with the text as its caption.
		o := a.outbox[0]
		gif := o.gif != ""
		p, ents := codeEntities(o.text)
		var body strings.Builder
		body.WriteString("chat_id=" + a.chats[o.chat].id)
		if gif {
			body.WriteString("&animation=" + formEncode(o.gif))
		}
		if !gif || p != "" {
			if gif {
				body.WriteString("&caption=")
			} else {
				body.WriteString("&text=")
			}
			body.WriteString(formEncode(toUTF8(p)))
		}
		if ents != "" {
			if gif {
				body.WriteString("&caption_entities=")
			} else {
				body.WriteString("&entities=")
			}
			body.WriteString(formEncode(ents))
		}
		body.WriteString(replyParameters(o.replyTo))
		method := "sendMessage"
		if gif {
			method = "sendAnimation"
		}
		a.begin(rqSend, method, "", form(body.String()), "")
		return
	}
	if len(a.reactOut) > 0 {
		r := a.reactOut[0]
		json := "[]"
		if r.r >= 0 {
			json = `[{"type":"emoji","emoji":"` + reactions[r.r].utf8 + `"}]`
		}
		body := "chat_id=" + a.chats[r.chat].id + "&message_id=" + strconv.FormatInt(r.msgID, 10) +
			"&reaction=" + formEncode(json)
		a.begin(rqReact, "setMessageReaction", "", form(body), "")
		return
	}
	if a.photoQueued {
		a.begin(rqSendPhoto, "sendPhoto", "", a.photoBody, "multipart/form-data; boundary="+boundary)
		a.say("Sending the screenshot...")
		return
	}
	// An MP4 waits while another is decoded: two at once would be two
	// decoders' worth of memory.
	if len(a.wanted) > 0 && !(a.wanted[0].kind == wMP4 && a.mp4.Busy()) {
		if w := a.wanted[0]; w.path != "" {
			a.request = rqFile
			a.submit(job{kind: rqFile, api: a.api, path: "/file/bot" + a.token + "/" + w.path, limit: gifMaxBytes + 64*1024})
		} else {
			a.begin(rqFileInfo, "getFile", "?file_id="+formEncode(w.fileID), nil, "")
		}
		return
	}
	// Reactions are delivered only when asked for by name, and naming them
	// replaces the default list, so what else is read goes too.
	a.begin(rqPoll, "getUpdates", "?timeout=20&limit=20&allowed_updates=%5B%22message%22%2C%22channel_post%22%2C"+
		"%22message_reaction%22%2C%22message_reaction_count%22%5D&offset="+strconv.FormatInt(a.offset, 10), nil, "")
}

// interruptPoll: a long poll would hold what is to go out back for up to
// 20 s.  The updates it would have brought come again, because the offset
// only moves past what has been read.
func (a *App) interruptPoll() {
	if a.request == rqPoll {
		a.cancel()
	}
	a.retryAt = 0
}

func (a *App) outNotSent() {
	o := a.outbox[0]
	t := o.text
	if o.gif != "" {
		t = "[GIF] " + t
	}
	a.addMsg(o.chat, true, "not sent", t, 0, "")
	a.dropOut()
}

func (a *App) dropOut() {
	if len(a.outbox) > 0 {
		a.outbox = append(a.outbox[:0], a.outbox[1:]...)
	}
}

func (a *App) photoNotSent() {
	a.addMsg(a.photoChat, true, "not sent", "[screenshot] "+a.photoCaption, 0, "")
	a.photoQueued = false
	a.photoBody = nil
}

// animate paints again when a GIF on show has a frame due.
func (a *App) animate() {
	now := a.now()
	for i := range a.photos {
		p := &a.photos[i]
		if p.shown >= 0 && p.anim.Frames > 1 && p.anim.FrameAt(now) != p.shown {
			a.repaint()
			return
		}
	}
}

// idle is a turn of the loop: pictures moving, an MP4 decoding, and the next
// request when the last has come back.
func (a *App) idle() {
	a.flushNotices()
	if a.setup {
		return
	}
	a.animate()
	if a.mp4.Busy() {
		a.decodeSome()
	}
	if a.request != rqNone || a.now() < a.retryAt {
		return
	}
	a.startNext()
}

// finished takes the answer to the request in flight.
func (a *App) finished(r result) {
	defer r.free()
	was := a.request
	a.request = rqNone
	line := requestNames[r.kind] + " " + strconv.FormatUint(r.took, 10) + " ms"
	if r.fresh {
		line += " (new connection)"
	}
	if r.err != nil {
		line += ": " + r.err.Error()
	} else {
		line += ": HTTP " + strconv.Itoa(r.status) + ", " + strconv.Itoa(len(r.body)) + " bytes"
	}
	// A long poll that brought nothing would push everything else out.
	if !(r.kind == rqPoll && r.err == nil && r.status == 200 && len(r.body) < 64 && !r.fresh) {
		a.log(line)
	}
	if a.stale {
		a.stale = false
		return
	}
	a.repaint()
	if r.err != nil {
		// A poll given up for a message to send: nothing went wrong.
		if was == rqPoll && r.err == errStopped {
			return
		}
		a.say(r.err.Error())
		switch was {
		case rqFileInfo, rqFile:
			a.dropWanted() // a photo is not worth trying for ever
		case rqSendPhoto:
			a.photoNotSent()
		case rqSend:
			a.outNotSent()
		case rqReact:
			a.dropReact()
		}
		a.retryAt = a.now() + 5000
		return
	}
	if was == rqFile {
		if r.status == 200 && len(r.body) > 0 {
			if a.wanted[0].kind == wMP4 {
				a.mp4Arrived(r.body)
			} else {
				a.photoArrived(r.body)
			}
		} else {
			a.say("A photo could not be fetched.")
		}
		a.dropWanted()
		return
	}
	b := r.body
	if !jtrue(b, jmember(b, 0, "ok")) {
		why, _ := jstr(b, jmember(b, 0, "description"), 80, false, nil)
		if why == "" {
			why = "HTTP " + strconv.Itoa(r.status)
		}
		if r.status == 401 || r.status == 404 {
			a.backToSetup("Telegram does not know that token. Check it and press Enter.")
			return
		}
		if r.status == 409 {
			a.say(capped("Another program is reading this bot's updates: "+why, 96))
		} else {
			a.say(capped("Telegram: "+why, 96))
		}
		switch was {
		case rqSend:
			a.outNotSent()
		case rqFileInfo:
			a.dropWanted()
		case rqSendPhoto:
			a.photoNotSent()
		case rqReact:
			// REACTION_INVALID, mostly: a group that allows only some.
			a.say(capped("Reaction refused: "+why, 96))
			a.dropReact()
		}
		a.retryAt = a.now() + 5000
		if r.status == 429 || r.status == 409 {
			a.retryAt = a.now() + 15000
		}
		return
	}
	result := jmember(b, 0, "result")
	switch was {
	case rqGetMe:
		a.botName, _ = jstr(b, jmember(b, result, "username"), 40, false, nil)
		if a.botName == "" {
			a.botName = "bot"
		}
		a.status = "@" + a.botName
		if len(a.chats) == 0 {
			a.status += " - waiting for someone to write to it"
		}
	case rqSend:
		// The answer is the message as sent: shown the same way as any.
		a.dropOut()
		a.takeMessage(b, result)
	case rqReact:
		// Telegram does not tell a bot about its own reactions: kept here.
		rq := a.reactOut[0]
		for k := a.msgTotal - 1; k >= a.oldest(); k-- {
			if m := a.msgAt(k); m.chat == rq.chat && m.id == rq.msgID {
				m.myReact = rq.r
				break
			}
		}
		if rq.r >= 0 {
			a.status = "Reacted."
		} else {
			a.status = "Reaction taken off."
		}
		a.dropReact()
	case rqSendPhoto:
		// The answer is the message with the photo in it: shown, and its
		// picture fetched back, the same way as anyone's.
		a.photoQueued = false
		a.photoBody = nil
		a.status = "Screenshot sent."
		a.takeMessage(b, result)
	case rqFileInfo:
		path, _ := jstr(b, jmember(b, result, "file_path"), 160, false, nil)
		if len(a.wanted) > 0 && path != "" {
			a.wanted[0].path = path
		} else {
			a.dropWanted()
		}
	case rqPoll:
		for u := jfirst(b, result); u >= 0; u = jnext(b, u) {
			if _, ok := jraw(b, jmember(b, u, "update_id")); ok {
				id := jnumber(b, jmember(b, u, "update_id"))
				if id < a.offset {
					continue // a retried poll must not repeat messages or bubbles
				}
				a.offset = id + 1
			}
			m := jmember(b, u, "message")
			if m < 0 {
				m = jmember(b, u, "channel_post")
			}
			if m >= 0 {
				a.takeMessage(b, m)
			} else if mr := jmember(b, u, "message_reaction"); mr >= 0 {
				a.takeReaction(b, mr)
			} else if mc := jmember(b, u, "message_reaction_count"); mc >= 0 {
				a.takeReactionCount(b, mc)
			}
		}
		if a.botName != "" {
			a.status = "@" + a.botName
		}
	}
}

// ── Keys and the mouse ─────────────────────────────────────────────────────

// command is one of the window's input events.
func (a *App) command(c *hosted.Command) {
	switch c.Op {
	case hosted.OpResize:
		if int(c.X) != a.width || int(c.Y) != a.height {
			a.width, a.height = int(c.X), int(c.Y)
			a.repaint()
		}
	case hosted.OpKey:
		k := c.Key()
		if !k.Down {
			return
		}
		var clip clipboard
		if k.Ctrl() && k.IsChar && (k.Char == 'v' || k.Char == 'V') {
			if c.X == hosted.PasteImage {
				clip.image, clip.imageLen = c.TextString(), int(c.Y)
			} else {
				clip.text = c.TextString()
			}
		}
		a.key(&k, clip)
	case hosted.OpMouseButton:
		if c.Extra != 0 { // pressed
			a.click(int(c.X), int(c.Y), c.Value != 0)
		}
	case hosted.OpWheel:
		a.wheel(c.Value != 0)
	}
}

// clipboard is Memento's at a Ctrl+V: text, or a PNG of a picture written
// to the file image, imageLen bytes long.
type clipboard struct {
	text, image string
	imageLen    int
}

// key is a key going down.
func (a *App) key(k *hosted.Key, clip clipboard) {
	ctrl, shift, alt := k.Ctrl(), k.Shift(), k.Alt()
	if ctrl && k.IsChar && (k.Char == 's' || k.Char == 'S') {
		a.toggleSettings()
		return
	}
	if a.settingsOpen {
		a.settingsKey(k)
		return
	}
	if a.menuSeq >= 0 {
		a.menuKey(k)
		return
	}
	switch {
	case !a.setup && k.IsChar && k.Char == ' ' && (ctrl || alt):
		a.openMenu(-1, true)
	case k.Escape:
		if !a.setup && a.replyID != 0 {
			a.cancelReply()
			a.say("Not replying.")
			return
		}
		a.closeWin()
	case k.Backspace:
		switch {
		case len(a.input) > 0:
			a.input = a.input[:len(a.input)-1]
		case a.attachment != nil:
			a.attachment = nil
			a.status = "Screenshot taken off."
		case a.attachGif != "":
			a.attachGif = ""
			a.status = "GIF taken off."
		}
		a.repaint()
	case ctrl && k.IsChar && (k.Char == 'v' || k.Char == 'V'):
		a.paste(clip)
	case ctrl && k.IsChar && (k.Char == 'c' || k.Char == 'C'):
		a.copy()
	case a.setup && k.Enter:
		if !validToken(string(a.input)) {
			a.say("That does not look like a token (digits, ':', then 35 more).")
			return
		}
		a.token = string(a.input)
		a.saveToken()
		a.input = a.input[:0]
		a.startChatting()
	case !a.setup && ctrl && k.IsChar && (k.Char == 't' || k.Char == 'T'):
		a.backToSetup("Type the new token and press Enter.")
	case !a.setup && k.Tab && len(a.chats) > 0:
		a.cancelReply() // it was to someone in the chat being left
		step := 1
		if shift {
			step = len(a.chats) - 1
		}
		a.cur = (max(a.cur, 0) + step) % len(a.chats)
		a.chats[a.cur].unread = 0
		a.scroll = 0
		a.repaint()
	case !a.setup && (k.ArrowUp || k.PageUp):
		a.scroll++
		if k.PageUp {
			a.scroll += a.visibleRows - 2
		}
		a.repaint() // clamped when painted: only then are the lines known
	case !a.setup && (k.ArrowDown || k.PageDown):
		a.scroll--
		if k.PageDown {
			a.scroll -= a.visibleRows - 2
		}
		a.scroll = max(a.scroll, 0)
		a.repaint()
	case !a.setup && k.Enter && shift:
		// A line break, for a ```code block``` of more than one line.
		if len(a.input) < inCap {
			a.input = append(a.input, '\n')
		}
		a.repaint()
	case !a.setup && k.Enter:
		a.send()
	case k.IsChar && !ctrl && len(a.input) < a.inputCap():
		a.input = append(a.input, k.Char)
		a.repaint()
	}
}

func (a *App) inputCap() int {
	if a.setup {
		return tokenCap - 1
	}
	return inCap
}

func (a *App) paste(clip clipboard) {
	text := clip.text
	if !a.setup && clip.image != "" {
		a.attachScreenshot(clip.image, clip.imageLen)
		return
	}
	if !a.setup && a.clipGif != "" && text == "[GIF]" {
		a.attachGif = a.clipGif
		a.attachment = nil // one thing attached at a time
		a.say("GIF attached: Enter sends it, with what is typed as its caption.")
		return
	}
	for i := 0; i < len(text) && len(a.input) < a.inputCap(); i++ {
		c := text[i]
		// A token copied from somewhere else often brings a space along.
		if a.setup && (c == ' ' || c == '\t') {
			continue
		}
		if c >= ' ' {
			a.input = append(a.input, c)
		}
	}
	a.repaint()
}

// attachScreenshot attaches the clipboard's picture, a PNG of n bytes
// Memento wrote at path, to go out with the next Enter.  What lies past n
// in the file is an older, longer file's.
func (a *App) attachScreenshot(path string, n int) {
	if n <= 0 || n > maxScreenshot {
		n = maxScreenshot
	}
	png, err := a.readFile(path, n)
	if err != nil {
		a.log("screenshot " + path + ": " + err.Error())
	}
	if err != nil || len(png) < 24 || string(png[1:4]) != "PNG" {
		a.say("The screenshot could not be read.")
		return
	}
	a.attachGif = ""
	a.attachment = png
	a.attachW = int(png[16])<<24 | int(png[17])<<16 | int(png[18])<<8 | int(png[19])
	a.attachH = int(png[20])<<24 | int(png[21])<<16 | int(png[22])<<8 | int(png[23])
	a.say("Screenshot attached (" + strconv.Itoa(a.attachW) + "x" + strconv.Itoa(a.attachH) + ", " +
		strconv.Itoa((len(png)+1023)/1024) + " KiB): Enter sends it")
}

func (a *App) copy() {
	if len(a.input) > 0 {
		a.copyText(string(a.input))
		a.say("Copied what is typed.")
		return
	}
	if a.setup || a.cur < 0 {
		return
	}
	// The newest message in the chat on the right.
	for k := a.msgTotal - 1; k >= a.oldest(); k-- {
		if m := a.msgAt(k); m.chat == a.cur {
			a.copyMessage(m)
			return
		}
	}
}

// copyMessage puts a message on the clipboard: without the "Name: " in
// front of it or the bytes that mark its code.
func (a *App) copyMessage(m *msg) {
	s := afterQuote(m.text)
	if i := strings.Index(s, ": "); i >= 0 {
		s = s[i+2:]
	}
	a.copyText(plain(s))
	a.say("Copied the message.")
}

func (a *App) send() {
	if len(a.input) == 0 && a.attachment == nil && a.attachGif == "" {
		return
	}
	if a.cur < 0 {
		a.say("No one to write to yet: a chat appears once someone writes to the bot.")
		return
	}
	replyTo := int64(0)
	if a.replyChat == a.cur {
		replyTo = a.replyID
	}
	if a.attachment != nil {
		// A screenshot, with what is typed as its caption.
		if a.photoQueued {
			a.say("Still sending the last screenshot; wait a moment.")
			return
		}
		a.photoBody = photoBody(a.chats[a.cur].id, toUTF8(string(a.input)), replyTo, a.attachment)
		a.attachment = nil
		a.cancelReply()
		a.photoQueued, a.photoChat, a.photoCaption = true, a.cur, string(a.input)
		a.input = a.input[:0]
		a.interruptPoll()
		a.say("Sending the screenshot...")
		return
	}
	if len(a.outbox) == maxOut {
		a.say("Still sending the last few; wait a moment.")
		return
	}
	a.outbox = append(a.outbox, outMsg{chat: a.cur, replyTo: replyTo, gif: a.attachGif, text: string(a.input)})
	a.attachGif = ""
	a.cancelReply()
	a.input = a.input[:0]
	a.interruptPoll()
	a.say("Sending...")
}

// messageAt is the message painted at (x, y), or -1.
func (a *App) messageAt(x, y int) int {
	if x < a.msgX || x >= a.msgX+a.msgW || y < a.msgTop {
		return -1
	}
	if row := (y - a.msgTop) / rowH; row < a.visibleRows && row < rowsCap {
		return a.rowSeq[row]
	}
	return -1
}

func (a *App) click(x, y int, right bool) {
	if a.settingsOpen {
		a.settingsClick(x, y, right)
		return
	}
	if !right && a.settingsButtonAt(x, y) {
		a.toggleSettings()
		return
	}
	if a.setup {
		return
	}
	if a.menuSeq >= 0 {
		// On an item, that; anywhere else, the menu goes away (and a right
		// click on another message brings it up there).
		if i := a.menuItemAt(x, y); i >= 0 {
			if !right {
				a.pickMenu(i)
			}
			return
		}
		a.closeMenu()
	}
	if right {
		if seq := a.messageAt(x, y); seq >= 0 {
			a.openMenu(seq, false)
		}
		return
	}
	if x >= chatsW {
		return
	}
	if row := (y - 6) / rowH; y >= 6 && row < len(a.chats) {
		if row != a.cur {
			a.cancelReply()
		}
		a.cur = row
		a.chats[a.cur].unread = 0
		a.scroll = 0
		a.repaint()
	}
}

func (a *App) wheel(up bool) {
	if a.menuSeq >= 0 || a.settingsOpen {
		return
	}
	if up {
		a.scroll += 3
	} else {
		a.scroll = max(a.scroll-3, 0)
	}
	a.repaint()
}
