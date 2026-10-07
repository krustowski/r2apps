#ifndef R2_SPOTIFY_HOST_H
#define R2_SPOTIFY_HOST_H
#include "../libgor2/memento/host.hpp"
// Spotify's operations and snapshot; the shared transport is in libgor2.
#define SPOTIFY_MAGIC 0x50533252u
#define SPOTIFY_VERSION 4u
#define SPOTIFY_NONE r2memento::NoBuffer
#define SPOTIFY_QUEUE r2memento::QueueSize
#define SPOTIFY_VISIBLE 10u

enum SpotifyOp { SpotifySelectPlaylist=1, SpotifySelectTrack, SpotifyPlayPause,
 SpotifyPrevious, SpotifyNext, SpotifyRefresh, SpotifyPlaylistPage,
 SpotifyTrackPage, SpotifyStop, SpotifyCollect, SpotifyVolumeDown, SpotifyVolumeUp, SpotifyMute, SpotifyOpenPlaylist };
using SpotifyCommand = r2memento::Command;
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
using SpotifyHostBlock = r2memento::Block<SpotifySnapshot>;
using SpotifyHost = r2memento::Host<SpotifySnapshot>;
static_assert(sizeof(SpotifySnapshot)==1804,"Spotify snapshot ABI");
static_assert(sizeof(SpotifyHostBlock)==4040,"Spotify shared block ABI");
#endif
