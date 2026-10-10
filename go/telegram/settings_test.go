package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func settingsShortcut(a *App) {
	a.command(&hosted.Command{Op: hosted.OpKey, Flags: 1<<0 | 1<<2 | 1<<14, Value: 's'})
}

func TestNotificationSettingsDefaultAndLoad(t *testing.T) {
	for _, tc := range []struct {
		name, config string
		enabled      bool
	}{
		{"missing", "", true},
		{"empty", "\n\x00\x00", true},
		{"on", "notifications=on\n", true},
		{"off", "notifications=off\n", false},
		{"sector padded", "notifications=off\n" + strings.Repeat("\x00", 494), false},
		{"whitespace", " notifications= off \r\n", false},
		{"unknown", "notifications=perhaps\n", true},
		{"other key", "unknown=off\n", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a, f := newFake(t)
			if !a.notificationsEnabled {
				t.Fatal("not enabled by default")
			}
			if tc.config != "" {
				f.files[settingsFile] = []byte(tc.config)
			}
			a.start()
			if a.notificationsEnabled != tc.enabled {
				t.Fatalf("loaded enabled=%v", a.notificationsEnabled)
			}
		})
	}
}

func TestNotificationSettingsPersistAcrossWindows(t *testing.T) {
	a, f := chatting(t)
	settingsShortcut(a)
	a.key(&hosted.Key{Down: true, Space: true}, clipboard{})
	if a.notificationsEnabled || string(f.files[settingsFile]) != "notifications=off\n" {
		t.Fatalf("not saved off: enabled=%v, file=%q", a.notificationsEnabled, f.files[settingsFile])
	}
	// Another window reads the same filesystem before it starts polling.
	b, second := newFake(t)
	second.files = f.files
	b.start()
	if b.notificationsEnabled || b.setup || b.token != testToken {
		t.Fatal("preference lost or token setup disturbed")
	}
	settingsShortcut(b)
	b.key(keyChar('n'), clipboard{})
	if !b.notificationsEnabled || string(second.files[settingsFile]) != "notifications=on\n" {
		t.Fatal("not saved back on")
	}
	c, third := newFake(t)
	third.files = second.files
	c.start()
	if !c.notificationsEnabled {
		t.Fatal("on preference lost in third window")
	}
	// Token edits still go to their own existing file and do not change mute.
	if string(f.files[shippedToken]) != testToken+"\n" {
		t.Fatal("changed shipped token")
	}
	b.saveToken()
	if string(f.files[typedToken]) != testToken+"\n" || string(f.files[settingsFile]) != "notifications=on\n" {
		t.Fatal("settings and token files interfered")
	}
}

func TestMutedMessagesAndReactionsStillArrive(t *testing.T) {
	a, f := chatting(t)
	a.toggleNotifications()
	f.notices, f.attention = nil, 0
	// Polls continue during settings; both messages and reaction counts update.
	settingsShortcut(a)
	f.next(a)
	a.finished(ok(`[{"update_id":42,"message":{"message_id":8,"from":{"first_name":"Bob"},"chat":{"id":5},"text":"quiet"}},` +
		`{"update_id":43,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[],"new_reaction":[{"type":"emoji","emoji":"❤"}]}},` +
		`{"update_id":44,"message_reaction_count":{"chat":{"id":5},"message_id":8,"reactions":[{"type":{"type":"emoji","emoji":"👍"},"total_count":2}]}}]`))
	if len(f.notices) != 0 || f.attention != 0 || len(a.notices) != 0 {
		t.Fatal("muted update raised a notification")
	}
	if a.msgTotal != 2 || a.msgAt(1).text != "Bob: quiet" || reactLine(a.msgAt(0)) != "  <3" || reactLine(a.msgAt(1)) != "  +1 2" {
		t.Fatal("muting stopped message or reaction updates")
	}
	a.toggleNotifications()
	a.addMsg(0, false, "Bob", "audible again", 9, "")
	if len(f.notices) != 1 || f.attention != 1 {
		t.Fatal("notifications did not resume")
	}
}

func TestMuteDropsPendingNotices(t *testing.T) {
	a, f := chatting(t)
	blocking := a.notify
	a.notify = func(string) bool { return false }
	a.alert(0, "pending")
	if len(a.notices) != 1 {
		t.Fatal("no pending notice to mute")
	}
	a.toggleNotifications()
	if len(a.notices) != 0 {
		t.Fatal("pending notice retained on mute")
	}
	a.notices = []string{"stale"} // flushing also respects the switch
	a.notify = blocking
	f.notices = nil
	a.idle()
	if len(f.notices) != 0 || len(a.notices) != 0 {
		t.Fatal("flushed while muted")
	}
	a.toggleNotifications()
	a.flushNotices()
	if len(f.notices) != 0 {
		t.Fatal("old notices replayed when re-enabled")
	}
}

func TestSettingsMouseAndKeyboard(t *testing.T) {
	a, f := chatting(t)
	typeText(a, "draft")
	a.paint()
	x, y, w, h := a.settingsButtonBox()
	a.command(&hosted.Command{Op: hosted.OpMouseButton, X: int32(x + w/2), Y: int32(y + h/2), Extra: 1})
	if !a.settingsOpen {
		t.Fatal("Settings button did not open")
	}
	a.paint()
	snapshot(t, a, "settings-on")
	l, top, width, height := a.settingsBox()
	a.click(l+20, top+38, true)
	if !a.notificationsEnabled {
		t.Fatal("right click toggled option")
	}
	a.click(l+20, top+38, false)
	if a.notificationsEnabled || !a.settingsOpen {
		t.Fatal("checkbox did not toggle")
	}
	a.paint()
	snapshot(t, a, "settings-off")
	a.key(keyChar('z'), clipboard{})
	a.wheel(true)
	if string(a.input) != "draft" || a.scroll != 0 {
		t.Fatal("settings input leaked into chat")
	}
	a.click(l+width-50, top+height-20, false)
	if a.settingsOpen || f.closed {
		t.Fatal("Close button closed the window")
	}
	a.openMenu(0, true)
	settingsShortcut(a)
	if !a.settingsOpen || a.menuSeq >= 0 {
		t.Fatal("settings did not take over the message menu")
	}
	a.key(&hosted.Key{Down: true, Escape: true}, clipboard{})
	if a.settingsOpen || f.closed || string(a.input) != "draft" {
		t.Fatal("Esc did not just close settings")
	}
	settingsShortcut(a)
	settingsShortcut(a)
	if a.settingsOpen {
		t.Fatal("Ctrl+S did not close settings")
	}
}

func TestSettingsBeforeTokenAndSaveFailure(t *testing.T) {
	a, f := newFake(t)
	a.start()
	a.input = []byte("partial-token")
	settingsShortcut(a)
	a.key(&hosted.Key{Down: true, Enter: true}, clipboard{})
	if a.notificationsEnabled || string(a.input) != "partial-token" || !a.setup {
		t.Fatal("setup settings changed token")
	}
	a.key(&hosted.Key{Down: true, Escape: true}, clipboard{})
	if f.closed || a.settingsOpen {
		t.Fatal("setup Esc closed the window")
	}
	a.writeFile = func(string, []byte) error { return errors.New("RAM disk full") }
	settingsShortcut(a)
	a.key(keyChar('n'), clipboard{})
	if !a.notificationsEnabled || a.settingsNote != "Could not save settings." {
		t.Fatal("save failure was hidden or toggle not applied")
	}
	a.paint()
	snapshot(t, a, "settings-save-error")
	// Resize while settings is open; controls use the current geometry.
	a.command(&hosted.Command{Op: hosted.OpResize, X: 400, Y: 220})
	a.paint()
	l, top, _, _ := a.settingsBox()
	a.click(l+20, top+38, false)
	if a.notificationsEnabled {
		t.Fatal("resized checkbox did not toggle")
	}
	a.paint()
	snapshot(t, a, "settings-resized")
}
