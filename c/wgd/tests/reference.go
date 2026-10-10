// Official wireguard-go peer with its userspace IP stack; no host TUN or root needed.
// Build within a checkout of WireGuard/wireguard-go, using this file as the package.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/netip"
	"os"
	"time"

	"golang.org/x/net/icmp"
	"golang.org/x/net/ipv4"
	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

type config struct{ Private, Public, Preshared, Endpoint string }

func main() {
	if len(os.Args) != 2 {
		panic("usage: reference TEST-KEYS.JSON")
	}
	b, err := os.ReadFile(os.Args[1])
	must(err)
	var cfg config
	must(json.Unmarshal(b, &cfg))
	tun, stack, err := netstack.CreateNetTUN([]netip.Addr{netip.MustParseAddr("10.77.0.2")}, nil, 1420)
	must(err)
	dev := device.NewDevice(tun, conn.NewDefaultBind(), device.NewLogger(device.LogLevelError, "reference: "))
	defer dev.Close()
	uapi := fmt.Sprintf("private_key=%s\npublic_key=%s\npreshared_key=%s\nallowed_ip=10.77.0.1/32\nendpoint=%s\n", cfg.Private, cfg.Public, cfg.Preshared, cfg.Endpoint)
	must(dev.IpcSet(uapi))
	must(dev.Up())
	for i, size := range []int{0, 8, 250, 1008, 1392} {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		c, err := stack.DialContext(ctx, "ping4", "10.77.0.1")
		must(err)
		must(c.SetDeadline(time.Now().Add(10 * time.Second)))
		payload := bytes.Repeat([]byte{byte(i + 1)}, size)
		request, err := (&icmp.Message{Type: ipv4.ICMPTypeEcho, Body: &icmp.Echo{Seq: i + 1, Data: payload}}).Marshal(nil)
		must(err)
		_, err = c.Write(request)
		must(err)
		buf := make([]byte, len(request))
		n, err := c.Read(buf)
		must(err)
		reply, err := icmp.ParseMessage(1, buf[:n])
		must(err)
		echo, ok := reply.Body.(*icmp.Echo)
		if !ok || reply.Type != ipv4.ICMPTypeEchoReply || echo.Seq != i+1 || !bytes.Equal(echo.Data, payload) {
			panic("ping payload mismatch")
		}
		c.Close()
		cancel()
	}
	fmt.Println("PASS: official wireguard-go handshake and five encrypted IPv4 pings")
	client := http.Client{Timeout: 20 * time.Second, Transport: &http.Transport{DialContext: stack.DialContext, DisableKeepAlives: true}}
	for _, path := range []string{"/", "/messages", "/"} {
		resp, err := client.Get("http://10.77.0.1:8080" + path)
		must(err)
		body, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		must(err)
		if resp.StatusCode != 200 {
			panic(fmt.Sprintf("HTTP status %d", resp.StatusCode))
		}
		if path == "/" && (len(body) < 1024 || !bytes.Contains(body, []byte("</html>"))) {
			panic("incomplete chat page")
		}
		fmt.Printf("PASS: encrypted TCP to existing chat HTTP service %s, status %d, %d bytes\n", path, resp.StatusCode, len(body))
	}
	if os.Getenv("WGD_LONG_TEST") == "1" {
		time.Sleep(125 * time.Second)
		resp, err := client.Get("http://10.77.0.1:8080/messages")
		must(err)
		resp.Body.Close()
		if resp.StatusCode != 200 {
			panic("HTTP after rekey failed")
		}
		fmt.Println("PASS: encrypted TCP after 125-second session rekey")
	}
}
func must(err error) {
	if err != nil {
		panic(err)
	}
}
