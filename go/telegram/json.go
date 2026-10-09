package main

// Just enough JSON to read what the Bot API answers: a cursor over the text
// that can skip a value, find a member of an object, walk an array and take a
// string out already turned into the font's code page.  Nothing is built.
// A position is an index into the text, -1 for none; every function takes -1
// and gives -1 back, so a chain of lookups needs one check at its end.

func jws(b []byte, p int) int {
	for p >= 0 && p < len(b) && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r') {
		p++
	}
	return p
}

// jskip is past the value at p.
func jskip(b []byte, p int) int {
	p = jws(b, p)
	if p < 0 || p >= len(b) {
		return -1
	}
	switch b[p] {
	case '"':
		for p++; p < len(b); p++ {
			if b[p] == '\\' {
				p++
			} else if b[p] == '"' {
				return p + 1
			}
		}
		return -1
	case '{', '[':
		end := byte('}')
		if b[p] == '[' {
			end = ']'
		}
		p = jws(b, p+1)
		if p < len(b) && b[p] == end {
			return p + 1
		}
		for {
			if end == '}' {
				if p = jws(b, jskip(b, p)); p < 0 || p >= len(b) || b[p] != ':' { // the key
					return -1
				}
				p++
			}
			if p = jws(b, jskip(b, p)); p < 0 || p >= len(b) {
				return -1
			}
			if b[p] == ',' {
				p++
				continue
			}
			if b[p] == end {
				return p + 1
			}
			return -1
		}
	}
	// A number, true, false or null.
	s := p
	for p < len(b) && b[p] != ',' && b[p] != '}' && b[p] != ']' && b[p] != ' ' && b[p] != '\n' && b[p] != '\r' && b[p] != '\t' {
		p++
	}
	if p > s {
		return p
	}
	return -1
}

// jmember is the value of member key of the object at p.
func jmember(b []byte, p int, key string) int {
	if p = jws(b, p); p < 0 || p >= len(b) || b[p] != '{' {
		return -1
	}
	for p = jws(b, p+1); p < len(b) && b[p] == '"'; {
		ke := jskip(b, p)
		if ke < 0 {
			return -1
		}
		k := b[p+1 : ke-1]
		if p = jws(b, ke); p >= len(b) || b[p] != ':' {
			return -1
		}
		v := jws(b, p+1)
		if string(k) == key {
			return v
		}
		if p = jws(b, jskip(b, v)); p < 0 || p >= len(b) || b[p] != ',' {
			return -1
		}
		p = jws(b, p+1)
	}
	return -1
}

// jfirst is the first element of the array at p; jnext the one after the
// element at p.
func jfirst(b []byte, p int) int {
	if p = jws(b, p); p < 0 || p >= len(b) || b[p] != '[' {
		return -1
	}
	if p = jws(b, p+1); p < len(b) && b[p] != ']' {
		return p
	}
	return -1
}

func jnext(b []byte, p int) int {
	if p = jws(b, jskip(b, p)); p < 0 || p >= len(b) || b[p] != ',' {
		return -1
	}
	return jws(b, p+1)
}

// jraw is a number, or true/false, as its text.
func jraw(b []byte, p int) (string, bool) {
	if p < 0 || p >= len(b) || b[p] == '"' || b[p] == '{' || b[p] == '[' {
		return "", false
	}
	q := jskip(b, p)
	if q < 0 {
		return "", false
	}
	return string(b[p:q]), true
}

func jnumber(b []byte, p int) int64 {
	t, ok := jraw(b, p)
	if !ok {
		return 0
	}
	neg := t != "" && t[0] == '-'
	if neg {
		t = t[1:]
	}
	var v int64
	for i := 0; i < len(t) && t[i] >= '0' && t[i] <= '9'; i++ {
		v = v*10 + int64(t[i]-'0')
	}
	if neg {
		return -v
	}
	return v
}

func jtrue(b []byte, p int) bool {
	t, _ := jraw(b, p)
	return t == "true"
}

func hexval(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

func hex4(b []byte, p int) rune {
	var cp rune
	for i := 0; i < 4; i++ {
		cp = cp*16 + rune(max(hexval(b[p+i]), 0))
	}
	return cp
}

// jchar is the character at p inside a string, unescaped and decoded from
// UTF-8, and where the next one starts.
func jchar(b []byte, p int) (rune, int) {
	if b[p] == '\\' && p+1 < len(b) {
		c := b[p+1]
		p += 2
		if c == 'u' && p+4 <= len(b) {
			cp := hex4(b, p)
			p += 4
			// A surrogate pair is one character (an emoji, mostly).
			if cp >= 0xD800 && cp < 0xDC00 && p+6 <= len(b) && b[p] == '\\' && b[p+1] == 'u' {
				cp = 0x10000 + (cp-0xD800)<<10 + (hex4(b, p+2) - 0xDC00)
				p += 6
			}
			return cp, p
		}
		switch c {
		case 'n':
			return '\n', p
		case 't', 'r', 'b', 'f':
			return ' ', p
		}
		return rune(c), p
	}
	c := b[p]
	p++
	more, cp := 0, rune(c)
	switch {
	case c >= 0xF0:
		more, cp = 3, rune(c&7)
	case c >= 0xE0:
		more, cp = 2, rune(c&15)
	case c >= 0xC0:
		more, cp = 1, rune(c&31)
	}
	for ; more > 0 && p < len(b) && b[p]&0xC0 == 0x80; more-- {
		cp = cp<<6 | rune(b[p]&0x3F)
		p++
	}
	return cp, p
}

// mark is a byte to put into a string at a character position, as Telegram
// counts them: in UTF-16 code units.  Sorted by at.
type mark struct {
	at   int
	byte byte
}

// jstr is the string at p, unescaped and decoded from UTF-8, one glyph per
// character (what the font has no glyph for comes out as '?'), at most
// limit-1 bytes.  Line breaks become spaces unless lines; marks go in where
// they say.  False when there is no string at p.
func jstr(b []byte, p, limit int, lines bool, marks []mark) (string, bool) {
	if p < 0 || p >= len(b) || b[p] != '"' {
		return "", false
	}
	out := make([]byte, 0, min(limit, 64))
	u16, mk := 0, 0
	for p++; p < len(b) && b[p] != '"'; {
		for ; mk < len(marks) && marks[mk].at <= u16; mk++ {
			if len(out)+1 < limit {
				out = append(out, marks[mk].byte)
			}
		}
		var cp rune
		cp, p = jchar(b, p)
		u16++
		if cp >= 0x10000 {
			u16++
		}
		g := glyphFor(cp)
		if cp == '\n' {
			g = ' '
			if lines {
				g = '\n'
			}
		}
		if g != 0 && len(out)+1 < limit {
			out = append(out, g)
		}
	}
	// Whatever closes at the very end.  When the text was cut short, the
	// last byte is given up for it rather than leave a block open.
	for ; mk < len(marks); mk++ {
		if len(out)+1 >= limit && len(out) > 0 {
			out = out[:len(out)-1]
		}
		if len(out)+1 < limit {
			out = append(out, marks[mk].byte)
		}
	}
	return string(out), true
}

// jrunes is the first code points of the string at p, at most n of them.
func jrunes(b []byte, p, n int) []rune {
	if p < 0 || p >= len(b) || b[p] != '"' {
		return nil
	}
	var out []rune
	for p++; p < len(b) && b[p] != '"' && len(out) < n; {
		var cp rune
		cp, p = jchar(b, p)
		out = append(out, cp)
	}
	return out
}
