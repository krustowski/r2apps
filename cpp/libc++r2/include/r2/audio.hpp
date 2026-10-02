#ifndef _R2CXX_AUDIO_HPP_
#define _R2CXX_AUDIO_HPP_

/*
 *  audio.hpp — the PC speaker, and PCM out of the HD Audio controller.
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

/*
 *  PCM through the HD Audio controller (syscall 0x3f): 16-bit signed stereo,
 *  left then right, into a 128 KiB ring the kernel plays round and round
 *  (about 0.68 s at 48 kHz).  write() never waits, it takes what there is
 *  room for; a stream that runs dry plays silence.  Inline, so that a program
 *  built without SSE (-mgeneral-regs-only) compiles these with its own flags.
 *
 *      if (r2::audio::open(44100)) {
 *          size_t took = r2::audio::write(pcm, bytes);
 *          ...
 *          r2::audio::close();
 *      }
 */

/*  Starts the stream at `rate` Hz (8000, 11025, 16000, 22050, 24000, 32000,
 *  44100, 48000, 88200 or 96000), taking it over from whoever had it.  False
 *  without a controller, for another rate, or on a kernel older than the
 *  call.  */
inline bool open(uint32_t rate) noexcept { return raw_syscall(Sys::Audio, 0x01, (int64_t)rate) == 0; }

/*  Queues `bytes` of PCM; returns how many were taken (whole samples).  */
inline size_t write(const void *pcm, size_t bytes) noexcept {
    struct {
        uint64_t buffer, length;
    } req = {(uint64_t)(uintptr_t)pcm, (uint64_t)bytes};
    int64_t r = raw_syscall(Sys::Audio, 0x02, (int64_t)&req);
    return r > 0 && (uint64_t)r <= bytes ? (size_t)r : 0;
}

/*  Bytes queued and not yet played.  */
inline uint64_t queued() noexcept {
    int64_t r = raw_syscall(Sys::Audio, 0x03, 0);
    return r > 0 && r < (1 << 20) ? (uint64_t)r : 0;
}

inline void close() noexcept { raw_syscall(Sys::Audio, 0x04, 0); }

/*  Holds the stream where it is, and lets it go on.  */
inline void pause() noexcept { raw_syscall(Sys::Audio, 0x05, 0); }
inline void resume() noexcept { raw_syscall(Sys::Audio, 0x06, 0); }

} // namespace r2::audio

#endif
