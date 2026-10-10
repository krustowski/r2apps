package main

import (
	"strings"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

const settingsFile = "/mnt/tmp/TELEGRAM.CFG"

// The RAM disk keeps preferences between window openings in this session.
// Token/API configuration remains in its existing files.
func (a *App) loadSettings() {
	a.notificationsEnabled = true
	b, err := a.readFile(settingsFile, 512)
	if err != nil {
		return
	}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(strings.TrimRight(line, "\x00"))
		value, ok := strings.CutPrefix(line, "notifications=")
		if !ok {
			continue
		}
		switch strings.TrimSpace(value) {
		case "on":
			a.notificationsEnabled = true
			return
		case "off":
			a.notificationsEnabled = false
			return
		}
	}
}

func (a *App) toggleNotifications() {
	a.notificationsEnabled = !a.notificationsEnabled
	value := "on"
	if !a.notificationsEnabled {
		value = "off"
		a.notices = nil // muted events must not appear later when re-enabled
	}
	a.settingsNote = "Saved."
	if err := a.writeFile(settingsFile, []byte("notifications="+value+"\n")); err != nil {
		a.settingsNote = "Could not save settings."
	}
	a.repaint()
}

func (a *App) toggleSettings() {
	a.settingsOpen = !a.settingsOpen
	if a.settingsOpen {
		a.closeMenu()
		a.settingsNote = ""
	}
	a.repaint()
}

func (a *App) settingsKey(k *hosted.Key) {
	switch {
	case k.Escape:
		a.toggleSettings()
	case k.Enter, k.Space, k.IsChar && (k.Char == ' ' || k.Char == 'n' || k.Char == 'N'):
		if !k.Ctrl() && !k.Alt() {
			a.toggleNotifications()
		}
	}
}

func inside(x, y, left, top, width, height int) bool {
	return x >= left && x < left+width && y >= top && y < top+height
}

func (a *App) settingsButtonBox() (x, y, w, h int) {
	return 4, a.height - rowH - 8, max(min(chatsW-8, a.width-8), 0), rowH + 4
}

func (a *App) settingsButtonAt(x, y int) bool {
	l, t, w, h := a.settingsButtonBox()
	return inside(x, y, l, t, w, h)
}

func (a *App) settingsBox() (x, y, w, h int) {
	w, h = max(min(a.width-12, 360), 0), max(min(a.height-12, 132), 0)
	return (a.width - w) / 2, (a.height - h) / 2, w, h
}

func (a *App) settingsClick(x, y int, right bool) {
	if right {
		return
	}
	l, t, w, h := a.settingsBox()
	if inside(x, y, l+12, t+32, w-24, rowH+6) {
		a.toggleNotifications()
	} else if inside(x, y, l+w-84, t+h-28, 72, rowH+4) || a.settingsButtonAt(x, y) {
		a.toggleSettings()
	}
}

func (a *App) paintSettingsButton() {
	x, y, w, h := a.settingsButtonBox()
	c := &a.cv
	c.fill(x, y, w, h, darkGrey)
	c.fill(x+1, y+1, w-2, h-2, lightGrey)
	c.text(x+6, y+3, w-12, "Settings Ctrl+S", black)
}

func (a *App) paintSettings() {
	x, y, w, h := a.settingsBox()
	c := &a.cv
	c.fill(x+2, y+2, w, h, darkGrey)
	c.fill(x, y, w, h, black)
	c.fill(x+1, y+1, w-2, h-2, lightGrey)
	c.setClip(x+1, y+1, w-2, h-2)
	c.text(x+12, y+10, w-24, "Settings", blue)
	check := "[ ] Notifications"
	if a.notificationsEnabled {
		check = "[x] Notifications"
	}
	c.fill(x+10, y+30, w-20, rowH+10, white)
	c.text(x+12, y+34, w-24, check, black)
	c.text(x+12, y+58, w-24, "Clock bubbles and window alerts", darkGrey)
	c.text(x+12, y+76, w-24, "Space / N: toggle. Esc: close.", darkGrey)
	c.text(x+12, y+h-24, w-108, a.settingsNote, blue)
	c.fill(x+w-84, y+h-28, 72, rowH+4, darkGrey)
	c.fill(x+w-83, y+h-27, 70, rowH+2, white)
	c.text(x+w-78, y+h-25, 60, "Close Esc", black)
	c.clearClip()
}
