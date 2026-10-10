package r2net

// tcpPeerMSS reads SYN options, including padding and safely ignoring malformed
// tails. IPv4 TCP without an MSS option uses 536 bytes.
func tcpPeerMSS(options []byte) int {
	for len(options) > 0 {
		kind := options[0]
		if kind == 0 {
			break
		}
		if kind == 1 {
			options = options[1:]
			continue
		}
		if len(options) < 2 {
			break
		}
		n := int(options[1])
		if n < 2 || n > len(options) {
			break
		}
		if kind == 2 && n == 4 {
			mss := int(options[2])<<8 | int(options[3])
			if mss > 0 {
				return mss
			}
		}
		options = options[n:]
	}
	return defaultPeerMSS
}
