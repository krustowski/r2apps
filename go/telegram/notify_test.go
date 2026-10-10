package main

import (
	"strconv"
	"strings"
	"testing"

	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func TestIncomingNotices(t *testing.T) {
	a, f := chatting(t)
	if len(f.notices) != 1 || f.notices[0] != "Telegram: Alice Liddell - Alice: hi \x82 ? run ls now" {
		t.Fatalf("incoming message notice: %q", f.notices)
	}
	f.next(a)
	a.finished(ok(`[{"update_id":42,"message":{"message_id":8,"from":{"first_name":"Bob"},"chat":{"id":-2,"title":"Room"},"photo":[{"file_id":"pic","width":40}],"caption":"look"}},` +
		`{"update_id":43,"message":{"message_id":9,"from":{"first_name":"R2","is_bot":true,"username":"r2bot"},"chat":{"id":5},"text":"outgoing"}}]`))
	if len(f.notices) != 2 || f.notices[1] != "Telegram: Room - Bob: [photo] look" || f.attention != 2 {
		t.Fatalf("photo / outgoing notices %q, attention %d", f.notices, f.attention)
	}
	// Even if a retry brings an old update, it must not duplicate the bubble.
	a.request = rqPoll
	a.finished(ok(`[{"update_id":42,"message":{"message_id":8,"from":{"first_name":"Bob"},"chat":{"id":-2},"text":"duplicate"}}]`))
	if len(f.notices) != 2 || a.msgTotal != 3 {
		t.Fatalf("replayed update: notices %q, messages %d", f.notices, a.msgTotal)
	}
}

func TestReactionNotices(t *testing.T) {
	a, f := chatting(t)
	f.attention = 0 // the initial incoming message already raised attention
	update := func(body string) {
		t.Helper()
		a.request = rqPoll
		a.finished(ok(body))
		if f.attention != 0 {
			t.Fatal("reaction update marked the Telegram window for attention")
		}
	}
	update(`[{"update_id":42,"message_reaction":{"chat":{"id":5},"message_id":7,"user":{"first_name":"Bob"},"old_reaction":[],"new_reaction":[{"type":"emoji","emoji":"❤"}]}}]`)
	if len(f.notices) != 2 || !strings.Contains(f.notices[1], "Bob reacted <3 to Alice: hi") {
		t.Fatalf("reaction notice %q", f.notices)
	}
	update(`[{"update_id":43,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[{"type":"emoji","emoji":"❤"}],"new_reaction":[{"type":"emoji","emoji":"❤"}]}}]`)
	update(`[{"update_id":44,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[{"type":"emoji","emoji":"❤"}],"new_reaction":[]}}]`)
	update(`[{"update_id":45,"message_reaction":{"chat":{"id":5},"message_id":7,"user":{"username":"r2bot"},"old_reaction":[],"new_reaction":[{"type":"emoji","emoji":"🔥"}]}}]`)
	update(`[{"update_id":46,"message_reaction":{"chat":{"id":5},"message_id":999,"old_reaction":[],"new_reaction":[{"type":"emoji","emoji":"❤"}]}}]`)
	if len(f.notices) != 2 || f.attention != 0 {
		t.Fatalf("unchanged / removed / own / unknown reactions alerted: %q", f.notices)
	}
	// Count-only updates alert on increases, including when the total amount
	// stays the same but a different reaction is added.
	update(`[{"update_id":47,"message_reaction_count":{"chat":{"id":5},"message_id":7,"reactions":[{"type":{"type":"emoji","emoji":"❤"},"total_count":2}]}}]`)
	if len(f.notices) != 3 || !strings.Contains(f.notices[2], "Someone reacted <3") {
		t.Fatalf("anonymous reaction notice %q", f.notices)
	}
	update(`[{"update_id":48,"message_reaction_count":{"chat":{"id":5},"message_id":7,"reactions":[{"type":{"type":"emoji","emoji":"❤"},"total_count":2}]}}]`)
	update(`[{"update_id":49,"message_reaction_count":{"chat":{"id":5},"message_id":7,"reactions":[{"type":{"type":"emoji","emoji":"❤"},"total_count":1}]}}]`)
	if len(f.notices) != 3 {
		t.Fatalf("unchanged / decreased counts alerted: %q", f.notices)
	}
	update(`[{"update_id":50,"message_reaction_count":{"chat":{"id":5},"message_id":7,"reactions":[{"type":{"type":"emoji","emoji":"👍"},"total_count":1}]}}]`)
	if len(f.notices) != 4 || !strings.Contains(f.notices[3], "reacted +1") {
		t.Fatalf("changed reaction with the same total: %q", f.notices)
	}
	update(`[{"update_id":51,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[{"type":"emoji","emoji":"😂"}],"new_reaction":[{"type":"emoji","emoji":"😎"}]}}]`)
	if len(f.notices) != 5 || !strings.Contains(f.notices[4], "reacted *") {
		t.Fatalf("different unsupported reactions conflated: %q", f.notices)
	}
	update(`[{"update_id":52,"message_reaction":{"chat":{"id":5},"message_id":7,"old_reaction":[{"type":"emoji","emoji":"😎"}],"new_reaction":[{"type":"emoji","emoji":"\ud83d\ude0e"}]}}]`)
	if len(f.notices) != 5 {
		t.Fatalf("equivalent escaped emoji alerted: %q", f.notices)
	}
}

func TestNoticeBurstRetries(t *testing.T) {
	a, f := chatting(t)
	f.notices = nil
	available := 2
	a.notify = func(s string) bool {
		if available == 0 {
			return false
		}
		available--
		f.notices = append(f.notices, s)
		return true
	}
	for i := 0; i < 20; i++ {
		a.alert(0, "message "+strconv.Itoa(i))
	}
	if len(f.notices) != 2 || len(a.notices) != 18 {
		t.Fatalf("lost burst: sent %d pending %d", len(f.notices), len(a.notices))
	}
	// Flush even during a request or retry delay, without another API result.
	a.request, a.retryAt = rqPoll, 5000
	available = 30
	a.idle()
	if len(a.notices) != 0 || len(f.notices) != 20 {
		t.Fatalf("not retried: sent %d pending %d", len(f.notices), len(a.notices))
	}
	for i, s := range f.notices {
		if !strings.HasSuffix(s, "message "+strconv.Itoa(i)) {
			t.Fatalf("burst order at %d: %q", i, s)
		}
	}
	available = 0
	for i := 0; i < 40; i++ {
		a.alert(0, "flood "+strconv.Itoa(i))
	}
	if len(a.notices) != maxNotices || !strings.HasSuffix(a.notices[0], "flood 8") {
		t.Fatalf("unbounded / wrong flood backlog: %q", a.notices)
	}
	a.notify = nil
	a.notices = nil
	a.alert(0, "legacy host")
	if len(a.notices) != 0 {
		t.Fatal("queued for an older host")
	}
}

func TestNoticeText(t *testing.T) {
	if got := noticeText("  A\n\t" + string(code) + "B" + string(code) + " \x82\x00  "); got != "A B \x82" {
		t.Fatalf("notice formatting %q", got)
	}
	if got := noticeText(strings.Repeat("x", 200)); len(got) != hosted.NotificationTextCapacity-1 || !strings.HasSuffix(got, "...") {
		t.Fatalf("notice truncation %q", got)
	}
}
