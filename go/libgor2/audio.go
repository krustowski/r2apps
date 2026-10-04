package libgor2

import "unsafe"

// PlayFreq sounds the PC speaker at freq Hz for ms milliseconds and blocks
// until it is done (syscall 0x1a).  The kernel accepts 20 Hz to 20 kHz.
func PlayFreq(freq, ms uint16) error {
	return err(Syscall(ScPlayFreq, uintptr(freq), uintptr(ms)))
}

// PlayMIDI plays a Standard MIDI File through the speaker (syscall 0x1b).
func PlayMIDI(name string) error {
	b := cstring(name)

	return err(Syscall(ScPlayFile, 0x01, ptr(unsafe.Pointer(&b[0]))))
}

// StopSpeaker silences the speaker (syscall 0x1f).
func StopSpeaker() error {
	return err(Syscall(ScPlayStop, 0, 0))
}

// AudioOpen opens HD Audio output at rate Hz (syscall 0x3f, op 1).
// Samples are signed 16-bit little-endian stereo. Opening takes over the
// machine's single output stream. ENotImplemented means no controller.
func AudioOpen(rate uint32) error {
	return err(Syscall(ScAudio, 1, uintptr(rate)))
}

// AudioWrite queues complete stereo frames without waiting. It returns a
// short count when the 128 KiB kernel ring fills; retry the remainder later.
func AudioWrite(pcm []byte) (int, error) {
	if len(pcm)%4 != 0 {
		return 0, EInvalidInput
	}
	if len(pcm) == 0 {
		return 0, nil
	}
	req := [2]uint64{uint64(ptr(unsafe.Pointer(&pcm[0]))), uint64(len(pcm))}
	n := Syscall(ScAudio, 2, ptr(unsafe.Pointer(&req)))
	if n > uintptr(len(pcm)) || n%4 != 0 {
		return 0, EInvalidInput
	}
	return int(n), nil
}

// AudioQueued reports PCM bytes waiting to play.
func AudioQueued() uint64 { return uint64(Syscall(ScAudio, 3, 0)) }

// AudioClose releases this process's output stream.
func AudioClose() error { return err(Syscall(ScAudio, 4, 0)) }

// AudioPause holds the queued audio and playback position.
func AudioPause() error { return err(Syscall(ScAudio, 5, 0)) }

// AudioResume continues a paused stream.
func AudioResume() error { return err(Syscall(ScAudio, 6, 0)) }
