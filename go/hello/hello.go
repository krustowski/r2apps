// example-print is the smallest useful Go program for rou2exOS: it says who it
// is, what it was given and what the machine thinks the time is, and leaves.
package main

import (
	"fmt"

	"github.com/krustowski/rou2exOS-apps/go/libgor2"
)

func main() {
	fmt.Printf("Hello from Go on r2!\n")

	if args := libgor2.Args(); len(args) > 1 {
		fmt.Printf("args: %v\n", args[1:])
	}

	var info libgor2.SysInfo
	if err := libgor2.ReadSysInfo(&info); err != nil {
		fmt.Printf("sysinfo: %v\n", err)
		return
	}

	fmt.Printf("host:    %s\n", trim(info.Name[:]))
	fmt.Printf("version: %s\n", trim(info.Version[:]))
	fmt.Printf("uptime:  %d s\n", info.Uptime)

	var now libgor2.RTC
	if err := libgor2.ReadRTC(&now); err != nil {
		fmt.Printf("rtc: %v\n", err)
		return
	}

	fmt.Printf("time:    %04d-%02d-%02d %02d:%02d:%02d\n",
		now.Year(), now.Month, now.Day, now.Hours, now.Minutes, now.Seconds)
}

// trim cuts a fixed-width kernel field down to the text in it.
func trim(b []byte) string {
	n := 0
	for n < len(b) && b[n] != 0 {
		n++
	}

	return string(b[:n])
}
