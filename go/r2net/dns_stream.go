package r2net

import (
	"encoding/binary"
	"errors"
	"io"
)

// DNS over TCP uses a two-byte length before each message. ReadFull matters:
// a TCP read can stop anywhere, including between the two length bytes.
func dnsStreamExchange(conn io.ReadWriter, query, answer []byte) (int, error) {
	if len(query) > 65535 {
		return 0, errors.New("DNS query exceeds wire length")
	}
	packet := make([]byte, len(query)+2)
	binary.BigEndian.PutUint16(packet, uint16(len(query)))
	copy(packet[2:], query)
	for len(packet) > 0 {
		n, err := conn.Write(packet)
		if err != nil {
			return 0, err
		}
		if n <= 0 || n > len(packet) {
			return 0, io.ErrShortWrite
		}
		packet = packet[n:]
	}
	var prefix [2]byte
	if _, err := io.ReadFull(conn, prefix[:]); err != nil {
		return 0, err
	}
	n := int(binary.BigEndian.Uint16(prefix[:]))
	if n < 12 || n > len(answer) {
		return 0, errors.New("invalid or excessive DNS response length")
	}
	_, err := io.ReadFull(conn, answer[:n])
	return n, err
}
