#ifndef R2_SPOTIFY_HOST_H
#define R2_SPOTIFY_HOST_H
// Pointer-free ABI shared with go/spotify/protocol. Control words use aligned
// 32-bit atomics; snapshots are double-buffered with a reader lease.
#define SPOTIFY_MAGIC 0x50533252u
#define SPOTIFY_VERSION 3u
#define SPOTIFY_NONE 2u
#define SPOTIFY_QUEUE 32u
#define SPOTIFY_VISIBLE 10u

enum SpotifyOp { SpotifySelectPlaylist=1, SpotifySelectTrack, SpotifyPlayPause,
 SpotifyPrevious, SpotifyNext, SpotifyRefresh, SpotifyPlaylistPage,
 SpotifyTrackPage, SpotifyStop, SpotifyCollect, SpotifyVolumeDown, SpotifyVolumeUp, SpotifyMute, SpotifyOpenPlaylist };
struct SpotifyCommand { uint32_t op, value; };
struct SpotifyRow { uint32_t index; char text[72]; };
struct SpotifySnapshot {
 uint32_t playlistCount, trackCount;
 uint32_t playlistSelected, trackSelected;
 uint32_t playlistOffset, trackOffset;
 uint32_t playing, paused, positionMS, durationMS;
 uint32_t heapBytes, stackBytes, goroutines;
 uint32_t volume, muted;
 char status[128], nowPlaying[96];
 SpotifyRow playlists[SPOTIFY_VISIBLE], tracks[SPOTIFY_VISIBLE];
};
struct SpotifyHostBlock {
 uint32_t magic, version;
 uint32_t clientBeat, frame, front, exited, tail;
 uint32_t hostBeat, reading, quit, head;
 SpotifyCommand commands[SPOTIFY_QUEUE];
 SpotifySnapshot snapshots[2];
};
static_assert(sizeof(SpotifySnapshot)==1804,"Spotify snapshot ABI");
static_assert(sizeof(SpotifyHostBlock)==3908,"Spotify shared block ABI");
#endif
