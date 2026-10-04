package model

// ApplyVolume attenuates PCM16 little-endian samples in place. Apply once per
// fresh source chunk, before queuing it; retrying a short write must not rescale.
func ApplyVolume(pcm []byte, percent uint32) {
	if percent > 100 {
		percent = 100
	}
	if percent == 100 {
		return
	}
	for i := 0; i+1 < len(pcm); i += 2 {
		sample := int32(int16(uint16(pcm[i]) | uint16(pcm[i+1])<<8))
		scaled := uint16(int16(sample * int32(percent) / 100))
		pcm[i], pcm[i+1] = byte(scaled), byte(scaled>>8)
	}
}
