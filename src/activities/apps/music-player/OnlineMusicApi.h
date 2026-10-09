#pragma once

// NetEase Cloud Music public (no-login) endpoints used by the online player:
//   /api/search/get                        keyword search
//   /api/song/enhance/player/url           resolve the 128 kbps MP3 URL
//   /api/song/detail                       album cover URL
//   /api/song/lyric                        LRC lyrics
//
// All plain HTTP, no signature as of 2026-10 (verified on-device). Copyrighted
// label tracks are not served without an account: enrich() then reports false
// and the UI keeps the vinyl fallback.

#include <cstdint>
#include <string>
#include <vector>

#include "TrackTags.h"

struct OnlineTrack {
  uint32_t id = 0;
  std::string title;
  std::string artist;
  std::string album;
  uint32_t durationMs = 0;

  // Filled by OnlineMusicApi::resolvePlayUrl(); coverUrl by resolveCover().
  std::string playUrl;
  uint32_t size = 0;
  std::string coverUrl;
};

namespace OnlineMusicApi {

constexpr size_t MAX_RESULTS = 200;

// Search by song name or artist. Replaces out; false on network/parse failure.
bool search(const std::string& keyword, std::vector<OnlineTrack>& out);

// Resolve ONLY the play URL (+ size) for one result. This is the playback
// critical path: a single request so audio can start as fast as possible.
// False when the song cannot be played (no url in the response).
bool resolvePlayUrl(OnlineTrack& track);
// Resolve the cover URL (single request). Best effort, called AFTER audio has
// started so the extra round trip never delays playback.
bool resolveCover(OnlineTrack& track);

// Fetch lyrics (synced when available) and populate tags.synced /
// tags.unsyncedLyrics. Best effort: returns false leaving tags untouched.
// Cache-first: reads the cached lyric body, then network (writes cache).
bool loadLyrics(uint32_t id, TrackTags& tags);
// Parse one /api/song/lyric response body into tags (shared by the network
// and cache paths). False leaving tags unchanged when no timed lyric exists.
bool parseLyricBody(const std::string& jsonBody, TrackTags& tags);

// Percent-encode UTF-8 text for a query string.
std::string urlEncode(const std::string& text);

// --- Account binding: SMS-code login -----------------------------------------

// No-SMS encrypted-POST self-test against cloudsearch; diagnostic only.
bool probeWeapiChannel();

// Send a login SMS code to the given mainland number (plaintext GET). False on
// network failure or when the server answers code != 200 (rate limit, etc.).
bool sendSmsCode(const std::string& phone);

// Log in with a phone number + SMS code via the encrypted weapi channel. On
// success the session cookies are written to cookieOut/csrfOut. False on a
// wrong code (503), risk control, or network failure.
bool loginWithSmsCode(const std::string& phone, const std::string& captcha,
                      std::string& cookieOut, std::string& csrfOut);

// --- Account binding (QR login) ----------------------------------------------

// Install/remove the bound session used by every subsequent request. When a
// session is present the enrich endpoint also serves VIP/copyrighted tracks.
void setSession(const std::string& cookie, const std::string& csrf = "");
void clearSession();
// Read-only "Name=Value; ..." form of the installed session, for diagnostics.
std::string sessionCookieHeader();
bool hasSession();

struct QrLoginStart {
  std::string unikey;
  std::string qrUrl;  // URL the displayed QR must encode
};

// Request a fresh QR-login key. False on network failure.
bool startQrLogin(QrLoginStart& out);

enum class QrPollCode { Error, Waiting, Scanned, Confirmed, Expired };

// Poll the login state. On Confirmed the response's Set-Cookie MUSIC_U/__csrf
// are written to cookieOut/csrfOut (untouched on other outcomes).
QrPollCode pollQrLogin(const std::string& unikey, std::string& cookieOut,
                       std::string& csrfOut);

struct AccountInfo {
  uint64_t userId = 0;
  std::string nickname;
};

// Fetch the logged-in account. False when not logged in.
bool getAccount(AccountInfo& out);

struct PlaylistInfo {
  uint64_t id = 0;
  std::string name;
  uint32_t trackCount = 0;
};

// List the bound user's playlists (personal playlists included).
bool userPlaylists(uint64_t uid, std::vector<PlaylistInfo>& out);

// Fetch a playlist's tracks in order (covers and durations populated).
bool playlistTracks(uint64_t playlistId, std::vector<OnlineTrack>& out);

}  // namespace OnlineMusicApi
