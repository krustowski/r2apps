package main

import "unicode/utf8"

// Text inside the client is bytes in the font's code page (437), as
// Memento's own windows keep it: what is typed arrives that way, and what
// comes from Telegram is mapped on the way in.  Formatting rides along in a
// message's text as bytes glyphFor never gives: a code block between preOn
// and preOff, drawn in a box with its line breaks kept; inline code between
// two codes.  '\n' is a line break.  A reply starts with quote and the
// message it answers, "Name: the start of it", up to a line break.
const (
	preOn  = '\x01'
	preOff = '\x02'
	code   = '\x03'
	quote  = '\x04'
)

// glyphFor is the glyph for a code point: ASCII as it is, what the font can
// show past it (uniMap), 0 for control characters, '?' for the rest.
func glyphFor(cp rune) byte {
	if cp >= 0x20 && cp < 0x7F {
		return byte(cp)
	}
	if cp < 0x20 || cp == 0x7F {
		return 0
	}
	lo, hi := 0, len(uniMap)
	for lo < hi {
		mid := (lo + hi) / 2
		switch m := rune(uniMap[mid][0]); {
		case m == cp:
			if g := byte(uniMap[mid][1]); g > quote {
				return g
			}
			return '?' // never one of the marks
		case m < cp:
			lo = mid + 1
		default:
			hi = mid
		}
	}
	return '?'
}

// cp437 is the upper half of code page 437, for text going out.
var cp437 = [128]rune{
	'Ç', 'ü', 'é', 'â', 'ä', 'à', 'å', 'ç', 'ê', 'ë', 'è', 'ï', 'î', 'ì', 'Ä', 'Å',
	'É', 'æ', 'Æ', 'ô', 'ö', 'ò', 'û', 'ù', 'ÿ', 'Ö', 'Ü', '¢', '£', '¥', '₧', 'ƒ',
	'á', 'í', 'ó', 'ú', 'ñ', 'Ñ', 'ª', 'º', '¿', '⌐', '¬', '½', '¼', '¡', '«', '»',
	'░', '▒', '▓', '│', '┤', '╡', '╢', '╖', '╕', '╣', '║', '╗', '╝', '╜', '╛', '┐',
	'└', '┴', '┬', '├', '─', '┼', '╞', '╟', '╚', '╔', '╩', '╦', '╠', '═', '╬', '╧',
	'╨', '╤', '╥', '╙', '╘', '╒', '╓', '╫', '╪', '┘', '┌', '█', '▄', '▌', '▐', '▀',
	'α', 'ß', 'Γ', 'π', 'Σ', 'σ', 'µ', 'τ', 'Φ', 'Θ', 'Ω', 'δ', '∞', 'φ', 'ε', '∩',
	'≡', '±', '≥', '≤', '⌠', '⌡', '÷', '≈', '°', '∙', '·', '√', 'ⁿ', '²', '■', ' ',
}

// toUTF8 is code page text as Telegram takes it.  Every character of it is
// one UTF-16 unit, so offsets counted in bytes before stay right.
func toUTF8(s string) string {
	ascii := true
	for i := 0; i < len(s); i++ {
		if s[i] >= 0x80 {
			ascii = false
			break
		}
	}
	if ascii {
		return s
	}
	out := make([]byte, 0, len(s)+8)
	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x80 {
			out = append(out, c)
		} else {
			out = utf8.AppendRune(out, cp437[c-0x80])
		}
	}
	return string(out)
}

// tail is the end of s that fits in cells, for an input box.
func tail(s string, cells int) string {
	if cells < 0 {
		cells = 0
	}
	if len(s) > cells {
		return s[len(s)-cells:]
	}
	return s
}

// afterQuote is a message's text past the line it quotes, if it is a reply.
func afterQuote(t string) string {
	if t == "" || t[0] != quote {
		return t
	}
	for i := 0; i < len(t); i++ {
		if t[i] == '\n' {
			return t[i+1:]
		}
	}
	return ""
}

// plain is text without the bytes that mark its code.
func plain(s string) string {
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		if c := s[i]; c != preOn && c != preOff && c != code {
			out = append(out, c)
		}
	}
	return string(out)
}

// line is a piece of a message as it is drawn on one row.
type line struct {
	s      string // CODE marks included
	block  bool   // a line of a code block
	codeOn bool   // inside inline code where it starts
	quote  bool   // a line of what a reply answers
}

// layout cuts a message into the lines it is drawn as: word-wrapped to cols,
// broken where it has line breaks, and its code blocks on lines of their own,
// one cell in from each side and wrapped where they reach it rather than at
// a space.
func layout(text string, cols int, out []line) []line {
	out = out[:0]
	pre, inCode, quoted := false, false, false
	s := 0
	for s < len(text) && len(out) < cap(out) {
		switch text[s] {
		case quote:
			quoted = true
			s++
			continue
		case preOn, preOff:
			pre = text[s] == preOn
			inCode = false
			s++
			if !pre && s < len(text) && text[s] == '\n' {
				s++ // the block's end is a line break already
			}
			continue
		}
		width := cols
		if pre {
			width = cols - 2
		}
		width = max(width, 4)
		startCode := inCode
		p, space, codeAtSpace, vis := s, -1, false, 0
		for p < len(text) && text[p] != '\n' && text[p] != preOn && text[p] != preOff && vis < width {
			if text[p] == code {
				inCode = !inCode
			} else {
				if text[p] == ' ' && vis > width/2 {
					space, codeAtSpace = p, inCode
				}
				vis++
			}
			p++
		}
		next := p
		if vis == width && !pre && p < len(text) && text[p] != ' ' && text[p] != '\n' &&
			text[p] != preOn && text[p] != preOff && space >= 0 {
			p, next = space, space // the word that did not fit goes to the next line
			inCode = codeAtSpace
		}
		out = append(out, line{s: text[s:p], block: pre, codeOn: startCode, quote: quoted})
		s = next
		if s < len(text) && text[s] == '\n' {
			s++
			quoted = false
		} else if !pre {
			for s < len(text) && text[s] == ' ' {
				s++
			}
		}
	}
	return out
}
