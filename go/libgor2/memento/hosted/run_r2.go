//go:build r2

package hosted

import (
	"time"

	r2 "github.com/krustowski/rou2exOS-apps/go/libgor2"
)

// Run attaches to the window supplied by Memento and runs its hooks until
// either side closes it. The process must have been started with --host.
// All hooks use one goroutine; workers should send results over channels.
// Runtime output is routed to the window's error message until normal exit.
func Run(options Options, hooks Hooks) error {
	client, err := Attach(r2.Args(), options.Magic, r2.Ticks())
	if err != nil {
		return err
	}
	r2.SetConsoleSink(client.RuntimeOutput)
	w, err := newWindow(client, options, hooks)
	if err != nil {
		client.Fail(err.Error())
		r2.SetConsoleSink(nil)
		client.Close()
		return err
	}
	var reason ExitReason
	for {
		reason = w.step(r2.Ticks())
		if reason != Running {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if hooks.OnClose != nil {
		hooks.OnClose(w, reason)
	}
	// Keep the runtime sink active through cleanup. A panic must leave the
	// block attached until the runtime has finished reporting its own words.
	r2.SetConsoleSink(nil)
	client.Close()
	return nil
}
