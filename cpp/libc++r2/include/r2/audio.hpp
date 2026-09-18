#ifndef _R2CXX_AUDIO_HPP_
#define _R2CXX_AUDIO_HPP_

/*
 *  audio.hpp — the PC speaker.
 */

#include "string_view.hpp"
#include "syscall.hpp"

namespace r2::audio {

/*  Sounds `freq` Hz for `duration` (syscall 0x1a).  The kernel blocks for the
 *  duration, so this is not the way to score a game loop.  */
inline bool beep(uint16_t freq, uint16_t duration_ms) {
    return raw_syscall(Sys::PlayFreq, freq, duration_ms) == 0;
}

/*  Stops whatever the speaker is doing (syscall 0x1f).  */
inline void stop() { raw_syscall(Sys::PlayStop, 0, 0); }

/*  Plays a MIDI file from the filesystem (syscall 0x1b).  */
bool play_midi(string_view path);

/*  Note frequencies, for anything that wants to play a tune.  */
inline constexpr uint16_t NOTE_C4 = 262;
inline constexpr uint16_t NOTE_D4 = 294;
inline constexpr uint16_t NOTE_E4 = 330;
inline constexpr uint16_t NOTE_F4 = 349;
inline constexpr uint16_t NOTE_G4 = 392;
inline constexpr uint16_t NOTE_A4 = 440;
inline constexpr uint16_t NOTE_B4 = 494;
inline constexpr uint16_t NOTE_C5 = 523;

} // namespace r2::audio

#endif
