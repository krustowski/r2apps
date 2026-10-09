package main

import (
	"strconv"
	"strings"
)

// What goes out to the Bot API, form encoded (or multipart, for a photo).

const boundary = "r2telegram-5c1e7f3a9b"

// formEncode percent-encodes everything but letters and digits.  The text is
// UTF-8 by then (toUTF8).
func formEncode(s string) string {
	const hx = "0123456789ABCDEF"
	var b strings.Builder
	b.Grow(len(s) * 3)
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' {
			b.WriteByte(c)
		} else {
			b.WriteByte('%')
			b.WriteByte(hx[c>>4])
			b.WriteByte(hx[c&15])
		}
	}
	return b.String()
}

// replyParameters is the message answered, form encoded ("" for none).  It
// goes even when that message has been deleted meanwhile, as a message that
// answers nothing.
func replyParameters(id int64) string {
	if id == 0 {
		return ""
	}
	return "&reply_parameters=" + formEncode(`{"message_id":`+strconv.FormatInt(id, 10)+`,"allow_sending_without_reply":true}`)
}

// codeEntities reads backticks in what is typed as Telegram's apps do:
// `inline code` and ```a code block```.  It gives the text without them and
// the JSON array of entities for it ("" for none).  The text is code page 437,
// a byte a character, and each character is one UTF-16 unit in UTF-8 too.
func codeEntities(in string) (string, string) {
	var p []byte
	var ents strings.Builder
	n := 0
	entity := func(typ string, off, length int) {
		if length <= 0 {
			return
		}
		one := `{"type":"` + typ + `","offset":` + strconv.Itoa(off) + `,"length":` + strconv.Itoa(length) + "}"
		if ents.Len()+len(one)+3 >= 256 {
			return
		}
		if n == 0 {
			ents.WriteByte('[')
		} else {
			ents.WriteByte(',')
		}
		n++
		ents.WriteString(one)
	}
	for i := 0; i < len(in); {
		fence := strings.HasPrefix(in[i:], "```")
		end := -1
		if fence {
			if j := strings.Index(in[i+3:], "```"); j >= 0 {
				end = i + 3 + j
			}
		} else if in[i] == '`' {
			if j := strings.IndexByte(in[i+1:], '`'); j >= 0 {
				end = i + 1 + j
			}
		}
		open := 1
		if fence {
			open = 3
		}
		if end > i+open {
			s, e := i+open, end
			if fence && in[s] == '\n' {
				s++
			}
			if fence && e > s && in[e-1] == '\n' {
				e--
			}
			start := len(p)
			if s < e {
				p = append(p, in[s:e]...)
			}
			typ := "code"
			if fence {
				typ = "pre"
			}
			entity(typ, start, len(p)-start)
			i = end + open
			continue
		}
		p = append(p, in[i])
		i++
	}
	if n > 0 {
		ents.WriteByte(']')
	}
	return string(p), ents.String()
}

// photoBody is sendPhoto's body: the chat, the caption and the PNG, as
// multipart/form-data.
func photoBody(chatID, caption string, reply int64, png []byte) []byte {
	var b strings.Builder
	part := func(name, extra string) {
		b.WriteString("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"" + extra + "\r\n\r\n")
	}
	part("chat_id", "")
	b.WriteString(chatID + "\r\n")
	if caption != "" {
		part("caption", "")
		b.WriteString(caption + "\r\n")
	}
	if reply != 0 {
		part("reply_parameters", "")
		b.WriteString(`{"message_id":` + strconv.FormatInt(reply, 10) + `,"allow_sending_without_reply":true}` + "\r\n")
	}
	part("photo", "; filename=\"screenshot.png\"\r\nContent-Type: image/png")
	out := make([]byte, 0, b.Len()+len(png)+len(boundary)+8)
	out = append(out, b.String()...)
	out = append(out, png...)
	return append(out, "\r\n--"+boundary+"--\r\n"...)
}
