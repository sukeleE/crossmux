#pragma once

// Music engine: playlist + transport control for the Music Player app.
//
// A Meyers singleton independent of any Activity, so playback survives leaving
// the app (reading/Home keeps the music going) and re-entering restores
// control. All playlist/transport methods run on the Arduino main task; the
// audio task touches the open file only through the static trampolines (each
// call wrapped in HalStorage::StorageLock, since SdFat is not thread-safe).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "HalAudioOutput.h"
#include "OnlineMusicApi.h"
#include "TrackTags.h"

class HalFile;
class HttpByteStream;

class MusicEngine {
 public:
  // Playlist traversal mode. A single 4-value enum (rather than a shuffle flag
  // x repeat-state pair) keeps update()/next()/previous() on one switch and the
  // mode menu rows mapped 1:1.
  enum class PlayMode : uint8_t {
    Sequential = 0,  // play through the list, then stop
    RepeatAll = 1,   // wrap around forever (default)
    RepeatOne = 2,   // repeat the current track
    Shuffle = 3,     // random permutation, reshuffled each pass
  };

  static MusicEngine& getInstance();
  ~MusicEngine();

  // Recursively scan /music (depth- and count-capped). Replaces the playlist;
  // does not stop a running session (callers rescan before starting one).
  // Returns the number of tracks found.
  size_t rescan();

  size_t trackCount() const { return tracks_.size(); }
  const std::string& trackPath(size_t index) const;
  size_t currentIndex() const { return index_; }

  bool playAt(size_t index);
  // Start an online session: playlist = results, start at startIndex. Local
  // playlist is untouched for when the session ends.
  bool playOnline(std::vector<OnlineTrack> results, size_t startIndex);
  // Load an online queue and position at startIndex WITHOUT resolving the URL,
  // opening the stream, or powering audio (no network, no sound). The player
  // renders the track paused and startCurrent() begins playback on first play.
  bool loadOnline(std::vector<OnlineTrack> tracks, size_t startIndex);
  bool isOnlineSession() const { return onlineSession_; }
  // Size of the currently active playlist (local or online).
  size_t activeCount() const { return onlineSession_ ? online_.size() : tracks_.size(); }
  // Display title of an online entry (returns "" outside an online session).
  const std::string& onlineTitle(size_t index) const;
  // Id / duration of an online entry (0 outside an online session / bad index).
  uint32_t onlineId(size_t index) const;
  uint32_t onlineDurationMs(size_t index) const;
  // Start playback of the current track (local or online). Used by the player
  // to bring up a merely-loaded queue on the first user play.
  bool startCurrent();
  void next();
  void previous();
  void togglePause();
  void adjustVolume(int delta);
  uint8_t volume() const { return volume_; }

  PlayMode playMode() const { return mode_; }
  // Change mode; entering Shuffle mid-session builds an order starting with the
  // current track. Persists immediately (menu picks are infrequent).
  void setPlayMode(PlayMode mode);
  // Called from setup() before audio is powered: memory state only.
  void restoreSettings(uint8_t mode, uint8_t volume);
  // Persist the current volume once (volume overlay hide), not every wheel tick.
  void persistVolume();
  // Physical indices of the next few tracks under the current mode (fewer when
  // nothing remains; empty at the end of a Sequential list).
  std::vector<size_t> upcomingIndices(size_t maxCount) const;

  // Snapshot from the audio layer (idle values when nothing is playing).
  HalAudioOutput::Status status() const;

  // Metadata of the current track (empty until a track starts).
  const TrackTags& tags() const { return tags_; }
  // 1-bit BMP bytes of the current track's cover, if any.
  const std::vector<uint8_t>& coverBmp() const { return coverBmp_; }
  bool hasCover() const { return !coverBmp_.empty(); }
  // Returns true once after an online track's lyrics/cover finish loading in
  // the background; the player calls it each loop and repaints when set.
  bool consumeExtrasReady() {
    if (!extrasReady_) return false;
    extrasReady_ = false;
    return true;
  }

  // True while a playback session (including the short gap between tracks) is
  // active. Key-click feedback is suppressed so it cannot preempt the music.
  bool soundSuppressed() const { return sessionActive_; }

  // Main-loop tick: auto-advance when the current track ends naturally.
  void update();

  // Full teardown before USB Drive exclusively owns the SD card.
  void stopForUsbDrive();

 private:
  MusicEngine() = default;
  MusicEngine(const MusicEngine&) = delete;
  MusicEngine& operator=(const MusicEngine&) = delete;

  bool reopenCurrent();
  void closeFile();
  void closeStream();
  void loadMetadata();
  // Play an online track straight from its fully cached file (no network).
  bool playCachedOnlineTrack(OnlineTrack& track);
  // Instant display tags for an online track (title/artist/album only).
  void applyOnlineTags(const OnlineTrack& track);
  // Lyrics + cover for an online track (network; best effort), run AFTER the
  // audio task has started so it never delays playback.
  void loadOnlineExtras(OnlineTrack& track);
  void scanDirectory(const std::string& path, int depthLeft);
  // Fisher-Yates permutation of [0,n) with firstTrack at position 0.
  void buildShuffleOrder(size_t n, size_t firstTrack);
  // Clean end of a Sequential pass: stop playback, keep index_ as highlight.
  void endSession();

  // Audio-task entry points; local-file calls take the storage lock, network
  // calls go straight to the HttpByteStream.
  static int fileReadTrampoline(uint8_t* dst, size_t len);
  static bool fileSeekTrampoline(size_t pos);
  static uint64_t fileSizeTrampoline();

  static constexpr int MAX_SCAN_DEPTH = 8;
  static constexpr size_t MAX_TRACKS = 2000;
  static constexpr char MUSIC_DIR[] = "/music";

  std::vector<std::string> tracks_;
  size_t index_ = 0;
  // Shuffle permutation of physical indices; orderPos_ is the current slot.
  std::vector<size_t> order_;
  size_t orderPos_ = 0;
  std::unique_ptr<HalFile> file_;
  // Online session state.
  std::vector<OnlineTrack> online_;
  bool onlineSession_ = false;
  // True while the online track plays from the on-SD cache file instead of the
  // HTTP stream (trampolines then route to file_ exactly like local playback).
  bool onlinePlayFromFile_ = false;
  std::unique_ptr<HttpByteStream> stream_;

  uint8_t volume_ = 60;
  PlayMode mode_ = PlayMode::RepeatAll;
  bool sessionActive_ = false;
  TrackTags tags_;
  std::vector<uint8_t> tagBuffer_;
  std::vector<uint8_t> coverBmp_;
  // Latched when loadOnlineExtras finishes; consumed by the player loop.
  bool extrasReady_ = false;
};
