package model

import "testing"

func TestResumeOnlyTheInterruptedSong(t *testing.T) {
	saved := Track{ID: "song", File: "/mnt/tar/song.wav", DurationMS: 10000}
	for _, tc := range []struct {
		track      Track
		mark, want uint32
	}{
		{saved, 5000, 5000},
		{saved, 10000, 0},
		{saved, 11000, 0},
		{Track{ID: "other", File: saved.File}, 5000, 0},
		{Track{ID: saved.ID, File: "/mnt/tar/other.wav"}, 5000, 0},
		{Track{ID: saved.ID, File: saved.File}, 10000, 0},
	} {
		if got := ResumePosition(saved, tc.mark, tc.track); got != tc.want {
			t.Fatalf("resume position %d, want %d", got, tc.want)
		}
	}
	if ResumePosition(Track{}, 1000, Track{}) != 0 {
		t.Fatal("unavailable tracks shared a resume position")
	}
}
