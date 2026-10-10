// Independent WireGuard peer; its test TUN models one router and one LAN host.
package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun"
	"io"
	"os"
	"os/signal"
)

type config struct {
	Private, Public, Preshared string
	Port                       int
}
type router struct {
	packets chan []byte
	events  chan tun.Event
	done    chan struct{}
}

func (*router) File() *os.File             { return nil }
func (*router) MTU() (int, error)          { return 1420, nil }
func (*router) Name() (string, error)      { return "test-router", nil }
func (*router) BatchSize() int             { return 1 }
func (r *router) Events() <-chan tun.Event { return r.events }
func (r *router) Close() error             { close(r.done); close(r.events); return nil }
func (r *router) Read(bufs [][]byte, sizes []int, offset int) (int, error) {
	select {
	case p := <-r.packets:
		sizes[0] = copy(bufs[0][offset:], p)
		return 1, nil
	case <-r.done:
		return 0, io.EOF
	}
}
func checksum(p []byte) uint16 {
	var sum uint32
	for len(p) >= 2 {
		sum += uint32(binary.BigEndian.Uint16(p))
		p = p[2:]
	}
	if len(p) > 0 {
		sum += uint32(p[0]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 65535) + (sum >> 16)
	}
	return ^uint16(sum)
}
func packet(src, dst []byte, icmp []byte) []byte {
	p := make([]byte, 20+len(icmp))
	p[0] = 0x45
	p[8] = 64
	p[9] = 1
	binary.BigEndian.PutUint16(p[2:4], uint16(len(p)))
	copy(p[12:16], src)
	copy(p[16:20], dst)
	copy(p[20:], icmp)
	p[22] = 0
	p[23] = 0
	binary.BigEndian.PutUint16(p[22:24], checksum(p[20:]))
	binary.BigEndian.PutUint16(p[10:12], checksum(p[:20]))
	return p
}
func (r *router) Write(bufs [][]byte, offset int) (int, error) {
	for _, buf := range bufs {
		p := buf[offset:]
		if len(p) < 28 || p[0] != 0x45 || p[9] != 1 || p[20] != 8 {
			continue
		}
		n := int(binary.BigEndian.Uint16(p[2:4]))
		if n > len(p) || n < 28 {
			continue
		}
		p = p[:n]
		if string(p[16:20]) != string([]byte{10, 4, 6, 68}) {
			continue
		}
		var answer []byte
		if p[8] == 1 {
			body := make([]byte, 36)
			body[0] = 11
			copy(body[8:], p[:28])
			answer = packet([]byte{10, 4, 6, 1}, p[12:16], body)
			fmt.Println("router: authenticated TTL=1, returning time exceeded")
		} else {
			body := append([]byte(nil), p[20:]...)
			body[0] = 0
			answer = packet([]byte{10, 4, 6, 68}, p[12:16], body)
			fmt.Println("router: authenticated echo, returning LAN host reply")
		}
		select {
		case r.packets <- answer:
		case <-r.done:
			return 0, io.EOF
		}
	}
	return len(bufs), nil
}
func main() {
	b, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	var c config
	if err = json.Unmarshal(b, &c); err != nil {
		panic(err)
	}
	r := &router{make(chan []byte, 32), make(chan tun.Event, 1), make(chan struct{})}
	dev := device.NewDevice(r, conn.NewDefaultBind(), device.NewLogger(device.LogLevelError, "router: "))
	defer dev.Close()
	uapi := fmt.Sprintf("private_key=%s\nlisten_port=%d\npublic_key=%s\npreshared_key=%s\nallowed_ip=10.3.255.253/32\n", c.Private, c.Port, c.Public, c.Preshared)
	if err = dev.IpcSet(uapi); err != nil {
		panic(err)
	}
	if err = dev.Up(); err != nil {
		panic(err)
	}
	fmt.Println("ROUTER READY")
	done := make(chan os.Signal, 1)
	signal.Notify(done, os.Interrupt)
	<-done
}
