/*
 *  audio.cpp — the file-playing half of the speaker API.
 */

#include "r2/audio.hpp"
#include "r2/libc.hpp"

namespace r2::audio {

bool play_midi(string_view path) {
    char buffer[64];
    if (path.size() >= sizeof(buffer))
        return false;

    memcpy(buffer, path.data(), path.size());
    buffer[path.size()] = '\0';

    return raw_syscall(Sys::PlayFile, 0x01, (int64_t)buffer) == 0;
}

} // namespace r2::audio
