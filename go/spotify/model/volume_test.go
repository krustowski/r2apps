package model

import "testing"

func TestVolumeSignedSamples(t *testing.T) {
	for _, c := range []struct {
		volume uint32
		want   []int16
	}{
		{100, []int16{-32768, -1000, 0, 1000, 32767}},
		{50, []int16{-16384, -500, 0, 500, 16383}},
		{0, []int16{0, 0, 0, 0, 0}},
		{200, []int16{-32768, -1000, 0, 1000, 32767}},
	} {
		samples := []int16{-32768, -1000, 0, 1000, 32767}
		pcm := make([]byte, len(samples)*2)
		for i, v := range samples {
			pcm[i*2], pcm[i*2+1] = byte(v), byte(uint16(v)>>8)
		}
		ApplyVolume(pcm, c.volume)
		for i, want := range c.want {
			got := int16(uint16(pcm[i*2]) | uint16(pcm[i*2+1])<<8)
			if got != want {
				t.Fatalf("volume %d sample %d: got %d want %d", c.volume, i, got, want)
			}
		}
	}
}
