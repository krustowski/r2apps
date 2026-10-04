package r2net

// tcpPayload accepts the new suffix of an overlapping retransmission. TCP may
// resend a segment whose prefix we already acknowledged before a window filled.
func tcpPayload(expected, seq uint32, data []byte, space int) []byte {
	if int32(seq-expected) > 0 {
		return nil
	}
	if seq != expected {
		overlap := uint32(expected - seq)
		if overlap >= uint32(len(data)) {
			return nil
		}
		data = data[int(overlap):]
	}
	if space < 0 {
		space = 0
	}
	if len(data) > space {
		data = data[:space]
	}
	return data
}
func tcpWindowNeedsUpdate(advertised, available int) bool {
	return available > advertised && (advertised == 0 || available-advertised >= 1460)
}

// Hold a receive window's worth of disjoint segments behind a gap. Keeping
// these bytes avoids discarding the rest of a CDN burst after one lost frame.
type tcpSegment struct {
	seq  uint32
	data []byte
}
type tcpReorder struct {
	segments [16]tcpSegment
	bytes    int
}

func (q *tcpReorder) store(expected, seq uint32, data []byte, window int) {
	distance := int32(seq - expected)
	if distance <= 0 || int(distance) >= window {
		return
	}
	if len(data) > window-int(distance) {
		data = data[:window-int(distance)]
	}
	if len(data) == 0 || len(data) > window-q.bytes {
		return
	}
	free := -1
	for i, segment := range q.segments {
		if len(segment.data) == 0 {
			free = i
			continue
		}
		start := int32(segment.seq - expected)
		if distance < start+int32(len(segment.data)) && start < distance+int32(len(data)) {
			return
		}
	}
	if free >= 0 {
		q.segments[free] = tcpSegment{seq: seq, data: append([]byte(nil), data...)}
		q.bytes += len(data)
	}
}
func (q *tcpReorder) take(expected uint32) []byte {
	for i, segment := range q.segments {
		if len(segment.data) == 0 || int32(segment.seq-expected) > 0 {
			continue
		}
		q.segments[i] = tcpSegment{}
		q.bytes -= len(segment.data)
		if data := tcpPayload(expected, segment.seq, segment.data, len(segment.data)); len(data) > 0 {
			return data
		}
	}
	return nil
}
