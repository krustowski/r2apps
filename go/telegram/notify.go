package main

import (
	"strings"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

const maxNotices = 32

// Incoming messages mark the window for attention and show a clock bubble.
func (a *App) alert(chat int, event string) {
	if !a.notificationsEnabled {
		return
	}
	a.attention()
	a.bubble(chat, event)
}

// Queue a clock bubble without changing window attention. Under a sustained
// flood keep the newest notices, like Memento's visible bubble stack.
func (a *App) bubble(chat int, event string) {
	if !a.notificationsEnabled || a.notify == nil {
		return
	}
	text := "Telegram: "
	if chat >= 0 && chat < len(a.chats) {
		text += a.chats[chat].name + " - "
	}
	text = noticeText(text + event)
	if len(a.notices) == maxNotices {
		copy(a.notices, a.notices[1:])
		a.notices = a.notices[:maxNotices-1]
	}
	a.notices = append(a.notices, text)
	a.flushNotices()
}

func (a *App) flushNotices() {
	if !a.notificationsEnabled {
		a.notices = nil
		return
	}
	for len(a.notices) > 0 && a.notify != nil && a.notify(a.notices[0]) {
		copy(a.notices, a.notices[1:])
		a.notices[len(a.notices)-1] = ""
		a.notices = a.notices[:len(a.notices)-1]
	}
}

// A bubble has one line: strip formatting, fold whitespace, and keep the
// font's code page bytes. Truncation leaves an ellipsis within the ABI limit.
func noticeText(s string) string {
	var out strings.Builder
	space := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == preOn || c == preOff || c == code || c == quote {
			continue
		}
		if c <= ' ' || c == 0x7f {
			space = out.Len() > 0
			continue
		}
		if space {
			out.WriteByte(' ')
			space = false
		}
		out.WriteByte(c)
	}
	text := out.String()
	if len(text) >= hosted.NotificationTextCapacity {
		text = text[:hosted.NotificationTextCapacity-4] + "..."
	}
	return text
}

// Compare the actual reaction, even when several unsupported emoji share
// the display label "*". JSON escape spelling does not affect identity.
func reactionKey(b []byte, r int) string {
	kind, _ := jstr(b, jmember(b, r, "type"), 24, false, nil)
	emoji := string(jrunes(b, jmember(b, r, "emoji"), 16))
	custom, _ := jstr(b, jmember(b, r, "custom_emoji_id"), fileIDCap, false, nil)
	return kind + ":" + emoji + ":" + custom
}

// Emit one bubble per update with an added reaction; removals and unchanged
// totals still update the message but have no new reaction to announce.
func (a *App) reactionAlert(m *msg, who string, changes [otherReaction + 1]int) {
	var labels []string
	for r, n := range changes {
		if n > 0 {
			label := "*"
			if r < len(reactions) {
				label = reactions[r].label
			}
			labels = append(labels, label)
		}
	}
	if len(labels) > 0 {
		a.bubble(m.chat, who+" reacted "+strings.Join(labels, ", ")+" to "+plain(afterQuote(m.text)))
	}
}
