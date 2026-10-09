#pragma once

// On-SD content cache for the online NetEase player.
//
// Everything is keyed by the stable NetEase id (or an MD5 of the query for
// search) under the existing hidden app root:
//
//   /.crosspoint/music-cache/
//     songs/manifest.txt         FIFO ledger ("CM1" then <id>,<size>,<cachedAt>)
//     songs/<id>.mp3             complete song
//     songs/<id>.part            in-progress tee (swept at boot)
//     songs/<id>.meta.json       {"v":1,"s":size,"u":playUrl,"t":cachedAt}
//     covers/<id>.bmp            converted 1-bit cover (skips JPEG decode)
//     lyrics/<id>.json           raw /api/song/lyric response
//     playlists/<plid>.json      track list + commit time
//     users/<uid>.json           playlist list + commit time
//     search/<md5hex>.json       search results + commit time
//
// Reads follow: fresh cache -> network (which writes cache) -> stale cache
// (TTL ignored, used offline). Songs are permanent until the 8 GiB budget is
// exceeded, at which point the oldest entries are evicted (FIFO). All cache
// work is best effort and must never block playback.

#include <cstdint>
#include <string>
#include <vector>

#include "OnlineMusicApi.h"  // OnlineTrack, OnlineMusicApi::PlaylistInfo
#include "TrackTags.h"

namespace MusicCache {

constexpr uint64_t SONG_BUDGET_BYTES = 8ULL * 1024 * 1024 * 1024;  // 8 GiB
constexpr uint64_t FREE_SPACE_MARGIN = 1024 * 1024;
constexpr uint32_t TTL_USER_PLAYLISTS_S = 6 * 3600;
constexpr uint32_t TTL_PLAYLIST_TRACKS_S = 24 * 3600;
constexpr uint32_t TTL_SEARCH_S = 3600;
constexpr uint8_t CACHE_FORMAT_VERSION = 1;

// Ensure the directory tree, load the ledger, sweep stale .part files and
// reconcile. Idempotent; call once after Storage.begin() succeeds.
bool begin();

// --- paths ---
std::string songFinalPath(uint32_t id);
std::string songPartPath(uint32_t id);

// --- songs / budget ---
// True iff the final song + meta exist, meta.s == the file size (and ==
// track.size when that is non-zero).
bool hasCompleteSong(const OnlineTrack& track);
// Evict oldest songs until a future file of `need` fits both the 8 GiB budget
// and the free space on the card. False when space cannot be guaranteed (the
// caller then streams without tee).
bool enforceBudget(uint64_t need);
// Record a fully tee'd song: write its meta, upsert the ledger, persist it.
// Runs on the producer task. When size == 0 the final file is stat'd.
void onSongCommitted(uint32_t id, uint64_t size, const std::string& url);

// --- lyrics (raw API JSON body) ---
bool writeLyrics(uint32_t id, const std::string& rawJsonBody);
bool readLyrics(uint32_t id, TrackTags& tags);  // parsed via parseLyricBody

// --- covers (converted BMP) ---
bool writeCoverBmp(uint32_t id, const std::vector<uint8_t>& bmp);
bool readCoverBmp(uint32_t id, std::vector<uint8_t>& bmp);

// --- playlist track lists ---
bool writePlaylist(uint64_t playlistId, const std::vector<OnlineTrack>& tracks);
bool readPlaylist(uint64_t playlistId, std::vector<OnlineTrack>& out, bool requireFresh);

// --- per-uid playlist lists ---
bool writeUserPlaylists(uint64_t uid,
                        const std::vector<OnlineMusicApi::PlaylistInfo>& playlists);
bool readUserPlaylists(uint64_t uid,
                       std::vector<OnlineMusicApi::PlaylistInfo>& out, bool requireFresh);

// --- search results ---
bool writeSearch(const std::string& keyword, const std::vector<OnlineTrack>& tracks);
bool readSearch(const std::string& keyword, std::vector<OnlineTrack>& out, bool requireFresh);

// --- management UI ---
uint64_t totalBytes();  // recursive sum of file sizes
bool clearAll();        // remove the cache tree and recreate it empty
}  // namespace MusicCache
