//go:build r2

// telegram.elf is the Telegram client Memento's Telegram window shows.
// Memento starts it with `--host 0x<address>`, passes the window's input in
// and shows the frames it draws (see package hosted); everything else ---
// the Bot API over HTTPS, pictures, GIFs --- happens here.
package main

import (
	"fmt"
	"time"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
	"github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func main() {
	host, err := hosted.Attach(r2.Args(), magic, r2.Ticks())
	if err != nil {
		fmt.Println("telegram.elf is started by Memento's Telegram window.")
		return
	}
	startLog(host)
	w := newWorker(host.PortBase())
	go w.run()

	quit := false
	a := newApp()
	a.colours = host.Colours()
	a.now = r2.Ticks
	a.readFile = readFile
	a.writeFile = func(path string, data []byte) error { return r2.WriteFile(path, data) }
	a.user = systemUser
	a.submit = func(j job) { w.jobs <- j }
	a.cancel = w.cancel
	a.copyText = host.Copy
	a.attention = host.Attention
	a.closeWin = func() { quit = true }
	a.log = logLine
	if mw, mh := host.MaxSize(); mw < a.width || mh < a.height {
		a.width, a.height = min(mw, a.width), min(mh, a.height)
	}
	a.start()

	var cmd hosted.Command
	for !quit && host.Poll(r2.Ticks()) == hosted.Running {
		for i := 0; i < 32 && host.Next(&cmd); i++ {
			if cmd.Op == hosted.OpResize && !host.Fits(int(cmd.X), int(cmd.Y)) {
				continue
			}
			a.command(&cmd)
		}
		select {
		case r := <-w.results:
			a.finished(r)
		default:
		}
		a.idle()
		if a.dirty {
			a.paint()
			if !host.Publish(a.cv.px, a.cv.w, a.cv.h, "Telegram") {
				a.dirty = true // Memento is still reading the other one
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	close(w.done)
	host.Close()
}

// readFile reads at most limit bytes of a file, a piece at a time: the
// room for a big one is only taken as it turns out to be big.
func readFile(path string, limit int) ([]byte, error) {
	const piece = 64 * 1024
	var out []byte
	for len(out) < limit {
		n := min(limit-len(out), piece)
		if cap(out)-len(out) < n {
			grown := make([]byte, len(out), min(limit, max(2*cap(out), len(out)+n)))
			copy(grown, out)
			out = grown
		}
		got, err := r2.ReadFileAt(path, out[len(out):len(out)+n], uint64(len(out)))
		if err != nil {
			if len(out) == 0 {
				return nil, err
			}
			break
		}
		out = out[:len(out)+got]
		if got < n {
			break
		}
	}
	return out, nil
}

// systemUser is who is at the keyboard, as the kernel has it: the login
// Memento set, or "" when the kernel would not say.
func systemUser() string {
	var info r2.SysInfo
	for tries := 0; tries < 3; tries++ { // busy now and then: the config lock
		if r2.ReadSysInfo(&info) == nil {
			n := 0
			for n < len(info.User) && info.User[n] > ' ' && info.User[n] < 0x7f {
				n++
			}
			return string(info.User[:n])
		}
	}
	return ""
}
