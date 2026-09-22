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
