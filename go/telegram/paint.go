package main

import "strconv"

// paint draws the window into a.cv at its size.  Positions are Memento's
// window's, at 2 pixels a unit.
func (a *App) paint() {
	a.cv.resize(a.width, a.height)
	a.cv.fill(0, 0, a.width, a.height, white)
	if a.setup {
		a.paintSetup()
	} else {
		a.paintChat()
	}
	a.paintSettingsButton()
	if a.settingsOpen {
		a.paintSettings()
	}
	a.dirty = false
}

func (a *App) paintSetup() {
	c, w, h := &a.cv, a.width, a.height
	lines := [...]string{
		"Telegram, through a bot of your own:",
		"",
		"1. In Telegram, write /newbot to @BotFather.",
		"2. Type the token it answers with here and press",
		"   Enter (or ship it in /mnt/tar/opt/memento/telegram.txt).",
		"3. Write to your bot from your phone: the chat",
		"   shows up here, and you answer as the bot.",
		"",
		"In a group the bot sees only /commands and replies",
		"to it, unless @BotFather's /setprivacy says otherwise.",
	}
	y := 8
	for _, l := range lines {
		c.text(12, y, w-24, l, black)
		y += rowH
	}
	y += 8
	c.fill(12, y-2, w-24, rowH+4, darkGrey)
	c.fill(13, y-1, w-26, rowH+2, white)
	c.text(16, y, w-32, tail(string(a.input)+"_", (w-36)/cw), black)
	if a.status != "" {
		x, _, buttonW, _ := a.settingsButtonBox()
		left := x + buttonW + 8
		c.text(left, h-rowH-4, w-left-12, a.status, blue)
	}
}

func (a *App) paintChat() {
	c, w, h := &a.cv, a.width, a.height

	// The chats down the left.
	c.fill(0, 0, chatsW, h, lightGrey)
	_, buttonY, _, _ := a.settingsButtonBox()
	for i, ch := range a.chats {
		y := 6 + i*rowH
		if y+rowH > buttonY-4 {
			break
		}
		label := ch.name
		if ch.unread > 0 {
			label = "* " + label
		}
		colour := byte(black)
		if i == a.cur {
			c.fill(2, y, chatsW-4, rowH, blue)
			colour = white
		}
		c.text(6, y, chatsW-10, label, colour)
	}
	if len(a.chats) == 0 {
		c.text(6, 6, chatsW-10, "(no chats)", darkGrey)
	}

	// The status line and the input box along the bottom.
	mx := chatsW + 6
	mw := w - mx - 6
	statusY := h - rowH - 2
	inputY := statusY - rowH - 6
	c.text(mx, statusY, mw, a.status, darkGrey)
	// What is being answered, over the input box.
	replying := a.replyID != 0 && a.replyChat == a.cur
	if replying {
		ry := inputY - rowH - 4
		r := "Reply to a message no longer shown"
		if a.replySeq >= 0 {
			s := plain(afterQuote(a.msgAt(a.replySeq).text))
			b := []byte("Reply to ")
			for i := 0; i < len(s) && len(b) < 79; i++ {
				if s[i] == '\n' {
					b = append(b, ' ')
				} else {
					b = append(b, s[i])
				}
			}
			r = string(b)
		}
		c.fill(mx-2, ry, 2, rowH, blue)
		c.text(mx+2, ry, mw-4, r, blue)
	}
	c.fill(mx-2, inputY-2, mw+4, rowH+4, darkGrey)
	c.fill(mx-1, inputY-1, mw+2, rowH+2, white)
	shown := []byte{}
	if a.attachment != nil {
		shown = append(shown, "[screenshot] "...)
	} else if a.attachGif != "" {
		shown = append(shown, "[GIF] "...)
	}
	for _, b := range append(a.input, '_') {
		if b == '\n' {
			b = '\x14' // the font's pilcrow
		}
		shown = append(shown, b)
	}
	c.text(mx+2, inputY, mw-4, tail(string(shown), (mw-6)/cw), black)

	// The messages of the chat on the right, wrapped, newest at the bottom.
	// Walked from the newest back, a message at a time, until the lines
	// above the scroll position and the window are filled.  A message is
	// its lines, then its photo, then its reactions.
	top, bottom := 6, inputY-6
	if replying {
		bottom -= rowH + 4
	}
	a.visibleRows = max((bottom-top)/rowH, 1)
	cols := max(mw/cw, 10)
	a.msgW, a.msgX, a.msgTop = mw, mx, top

	// How many lines there are, to keep the scroll in range, and where the
	// menu's message is, to bring it into view.
	total, selFrom, selTo := 0, -1, -1
	for k := a.msgTotal - 1; k >= a.oldest(); k-- {
		m := a.msgAt(k)
		if m.chat != a.cur {
			continue
		}
		n := len(layout(m.text, cols, a.lines)) + a.photoRows(m)
		if reactLine(m) != "" {
			n++
		}
		if k == a.menuSeq {
			selFrom, selTo = total, total+n
		}
		total += n
	}
	if a.menuMoved && selFrom >= 0 {
		if selTo > a.scroll+a.visibleRows {
			a.scroll = selTo - a.visibleRows
		}
		if selFrom < a.scroll {
			a.scroll = selFrom
		}
		a.menuMoved = false
	}
	a.scroll = min(a.scroll, max(total-a.visibleRows, 0))

	for r := range a.rowSeq {
		a.rowSeq[r] = -1
	}
	for i := range a.photos {
		a.photos[i].shown = -1
	}
	a.menuAnchorY = -1
	c.setClip(mx, top, mw, bottom-top)
	shownLines := 0 // lines from the bottom, counting up the screen
	for k := a.msgTotal - 1; k >= a.oldest() && shownLines < a.scroll+a.visibleRows; k-- {
		m := a.msgAt(k)
		if m.chat != a.cur {
			continue
		}
		lines := layout(m.text, cols, a.lines)
		n, pr, reacts := len(lines), a.photoRows(m), reactLine(m)
		height := n + pr
		if reacts != "" {
			height++
		}
		rowBottom := a.visibleRows - 1 - (shownLines - a.scroll)
		rowTop := rowBottom - height + 1
		shownLines += height
		if rowBottom < 0 {
			continue
		}
		for r := max(rowTop, 0); r <= rowBottom && r < rowsCap; r++ {
			a.rowSeq[r] = k
		}
		if k == a.menuSeq {
			c.fill(mx, top+rowTop*rowH, mw, height*rowH, yellow)
			a.menuAnchorY = top + max(rowTop, 0)*rowH
		}
		colour := byte(black)
		if m.mine {
			colour = blue
		}
		for j, l := range lines {
			if row := rowTop + j; row >= 0 && row < a.visibleRows {
				a.drawLine(mx, top+row*rowH, mw, l, colour)
			}
		}
		// Its photo under the text, drawn where its rows fall.
		if pr > 0 {
			a.paintPhoto(&a.photos[m.photo], mx, top+(rowTop+n)*rowH)
		}
		if reacts != "" {
			c.text(mx, top+rowBottom*rowH, mw, reacts, darkGrey)
		}
	}
	c.clearClip()
	if a.scroll > 0 {
		c.textEnd(mx, top, mw, "^ "+strconv.Itoa(a.scroll), darkGrey)
	}
	if a.menuSeq >= 0 {
		a.paintMenu()
	}
}

// drawLine draws one line of a message: a code block's on its box, the rest
// in runs with inline code on a box of its own.
func (a *App) drawLine(x, y, w int, l line, colour byte) {
	c := &a.cv
	switch {
	case l.quote:
		c.fill(x, y, 2, rowH, blue)
		c.text(x+cw, y, w-cw, l.s, darkGrey)
	case l.block:
		c.fill(x, y, w, rowH, lightGrey)
		c.fill(x, y, 2, rowH, darkGrey)
		c.text(x+cw, y, w-2*cw, l.s, colour)
	default:
		inCode, col := l.codeOn, 0
		for p := 0; p < len(l.s); {
			q := p
			for q < len(l.s) && l.s[q] != code {
				q++
			}
			if n := q - p; n > 0 {
				if inCode {
					c.fill(x+col*cw, y, n*cw, rowH, lightGrey)
				}
				c.text(x+col*cw, y, w-col*cw, l.s[p:q], colour)
				col += n
			}
			if q < len(l.s) {
				inCode = !inCode
				q++
			}
			p = q
		}
	}
}

// paintMenu draws the menu beside the top of its message (or the top of the
// messages when that is scrolled off), kept inside the window.
func (a *App) paintMenu() {
	c := &a.cv
	widest := 0
	for i := 0; i < miCount; i++ {
		label, keys := a.menuLabel(i)
		n := len(label) - 1
		if keys != "" {
			n += len(keys) + 2
		}
		widest = max(widest, n)
	}
	a.menuW = widest*cw + 16
	h := miCount*rowH + 8
	a.menuL, a.menuT = a.msgX+24, a.msgTop
	if a.menuAnchorY >= 0 {
		a.menuT = a.menuAnchorY
	}
	if a.menuL+a.menuW > a.width-4 {
		a.menuL = a.width - 4 - a.menuW
	}
	if a.menuT+h > a.height-4 {
		a.menuT = a.height - 4 - h
	}
	a.menuL, a.menuT = max(a.menuL, 2), max(a.menuT, 2)
	// A shadow, a border, then the items.
	l, t, w := a.menuL, a.menuT, a.menuW
	c.fill(l+3, t+3, w, h, darkGrey)
	c.fill(l, t, w, h, black)
	c.fill(l+1, t+1, w-2, h-2, white)
	iy := t + 4
	for i := 0; i < miCount; i, iy = i+1, iy+rowH {
		if i == miSep {
			c.fill(l+4, iy+rowH/2, w-8, 1, lightGrey)
			continue
		}
		label, keys := a.menuLabel(i)
		on, sel := a.menuEnabled(i), i == a.menuSel
		if sel {
			c.fill(l+2, iy, w-4, rowH, blue)
		}
		colour := byte(black)
		switch {
		case !on:
			colour = darkGrey
		case sel:
			colour = white
		}
		shown, accel := make([]byte, 0, len(label)), -1
		for j := 0; j < len(label); j++ {
			if label[j] == '&' && accel < 0 {
				accel = len(shown)
				continue
			}
			shown = append(shown, label[j])
		}
		tx := l + 8
		c.text(tx, iy+1, w-16, string(shown), colour)
		if accel >= 0 && on {
			c.fill(tx+accel*cw, iy+ch, cw-1, 1, colour)
		}
		if keys != "" {
			kc := byte(darkGrey)
			if sel {
				kc = white
			}
			c.text(l+w-8-len(keys)*cw, iy+1, len(keys)*cw, keys, kc)
		}
	}
}

// photoSize is the size a picture is drawn at: as it is, or smaller to fit
// the width of the messages (it was made to fit when it came, but the window
// may have been made narrower since).
func (a *App) photoSize(p *photo) (int, int) {
	w, h := p.size()
	if a.msgW > 0 && w > a.msgW {
		h, w = h*a.msgW/w, a.msgW
	}
	return w, max(h, 1)
}

func (a *App) photoRows(m *msg) int {
	if m.photo < 0 || !a.photos[m.photo].any() {
		return 0
	}
	_, h := a.photoSize(&a.photos[m.photo])
	return (h + 2 + rowH - 1) / rowH // with a little room under it
}

// paintPhoto draws a picture with its top left at (x, y), cut to the message
// area.  A GIF's frame is the one for the time it is now: every GIF on show
// keeps to the clock, and the idle loop paints again when one has moved on.
func (a *App) paintPhoto(p *photo, x, y int) {
	if !p.any() {
		return
	}
	px, pw, ph := p.pic.Px, p.pic.W, p.pic.H
	if p.anim.Px != nil {
		p.shown = p.anim.FrameAt(a.now())
		px, pw, ph = p.anim.Frame(p.shown), p.anim.W, p.anim.H
	}
	w, h := a.photoSize(p)
	a.cv.blit(x, y, w, h, px, pw, ph, a.msgX+a.msgW)
}
