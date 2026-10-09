#include "MusicEngine.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointState.h"
#include "CoverLoader.h"
#include "HttpByteStream.h"
#include "MusicCache.h"
#include "network/HttpDownloader.h"

namespace {

std::string joinPath(const std::string& parent, const std::string& name) {
  return parent == "/" ? "/" + name : parent + "/" + name;
}

constexpr const char* ONLINE_COVER_TMP = "/.online_cover_tmp";

}  // namespace

MusicEngine& MusicEngine::getInstance() {
  static MusicEngine instance;
  return instance;
}

MusicEngine::~MusicEngine() = default;

size_t MusicEngine::rescan() {
  tracks_.clear();
  scanDirectory(MUSIC_DIR, MAX_SCAN_DEPTH);
  // An online session owns index_/order_ while it runs.
  if (!onlineSession_) {
    index_ = 0;
    std::vector<size_t>().swap(order_);
    orderPos_ = 0;
  }
  return tracks_.size();
}

const std::string& MusicEngine::onlineTitle(const size_t index) const {
  static const std::string empty;
  return onlineSession_ && index < online_.size() ? online_[index].title : empty;
}

uint32_t MusicEngine::onlineId(const size_t index) const {
  return onlineSession_ && index < online_.size() ? online_[index].id : 0;
}

uint32_t MusicEngine::onlineDurationMs(const size_t index) const {
  return onlineSession_ && index < online_.size() ? online_[index].durationMs : 0;
}

const std::string& MusicEngine::trackPath(size_t index) const {
  static const std::string empty;
  return index < tracks_.size() ? tracks_[index] : empty;
}

void MusicEngine::scanDirectory(const std::string& path, int depthLeft) {
  if (tracks_.size() >= MAX_TRACKS) return;

  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) return;  // missing /music is simply empty
  dir.rewindDirectory();

  std::vector<std::string> filesHere;
  std::vector<std::string> subdirs;

  char nameBuf[256];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.getName(nameBuf, sizeof(nameBuf)) == 0) continue;
    if (nameBuf[0] == '.') continue;  // hidden, "." and ".."

    const bool isDir = entry.isDirectory();
    std::string full = joinPath(path, nameBuf);
    if (isDir) {
      if (depthLeft > 0) subdirs.push_back(std::move(full));
    } else if (FsHelpers::checkFileExtension(full, ".mp3") ||
               FsHelpers::checkFileExtension(full, ".wav")) {
      filesHere.push_back(std::move(full));
      if (filesHere.size() + tracks_.size() >= MAX_TRACKS) break;
    }
  }

  // Natural order within each directory; directories are visited in order, so
  // album folders stay grouped instead of interleaving via a global sort.
  FsHelpers::sortFileList(filesHere);
  FsHelpers::sortFileList(subdirs);

  for (const std::string& file : filesHere) {
    tracks_.push_back(file);
    if (tracks_.size() >= MAX_TRACKS) return;
  }
  for (const std::string& subdir : subdirs) {
    scanDirectory(subdir, depthLeft - 1);
    if (tracks_.size() >= MAX_TRACKS) return;
  }
}

void MusicEngine::closeFile() {
  if (file_) {
    file_->close();
    file_.reset();
  }
}

void MusicEngine::closeStream() {
  if (stream_) {
    stream_->close();
    stream_.reset();
  }
}

void MusicEngine::applyOnlineTags(const OnlineTrack& track) {
  tags_ = TrackTags();
  coverBmp_.clear();
  extrasReady_ = false;

  tags_.title = track.title;
  tags_.artist = track.artist;
  tags_.album = track.album;
}

bool MusicEngine::playCachedOnlineTrack(OnlineTrack& track) {
  closeStream();
  closeFile();
  onlinePlayFromFile_ = false;

  file_ = std::unique_ptr<HalFile>(new HalFile());
  const std::string path = MusicCache::songFinalPath(track.id);
  if (!Storage.openFileForRead("Music", path, *file_)) {
    LOG_ERR("Music", "cached song open failed: %s", path.c_str());
    closeFile();
    return false;
  }

  applyOnlineTags(track);

  // Instant extras straight from the cache — no network on this path at all.
  MusicCache::readCoverBmp(track.id, coverBmp_);
  if (!tags_.hasLyrics()) MusicCache::readLyrics(track.id, tags_);

  onlinePlayFromFile_ = true;
  const bool ok = HalAudioOutput::playMp3(
      fileReadTrampoline, fileSeekTrampoline, fileSizeTrampoline, volume_);
  if (!ok) {
    onlinePlayFromFile_ = false;
    closeFile();
    return false;
  }
  LOG_INF("MusicCache", "cached song hit: id=%u '%s'", track.id,
          track.title.c_str());
  return true;
}

void MusicEngine::loadOnlineExtras(OnlineTrack& track) {
  // Lyrics: cached body first, then network (loadLyrics writes it back).
  if (!tags_.hasLyrics()) OnlineMusicApi::loadLyrics(track.id, tags_);

  // Cover: prefer the converted BMP already on the SD card. Only when it is
  // missing do we resolve the URL and fetch a small 300px version (the original
  // is often 1000px+, ~540 KB / ~10 s over this hotspot).
  if (coverBmp_.empty()) MusicCache::readCoverBmp(track.id, coverBmp_);
  if (coverBmp_.empty()) {
    if (track.coverUrl.empty()) OnlineMusicApi::resolveCover(track);
    if (!track.coverUrl.empty()) {
      const std::string sized =
          track.coverUrl +
          (track.coverUrl.find('?') == std::string::npos ? "?param=300y300"
                                                          : "&param=300y300");
      if (HttpDownloader::downloadToFile(sized, ONLINE_COVER_TMP) ==
          HttpDownloader::OK) {
        CoverLoader::fileToMonoBmp(ONLINE_COVER_TMP, false, coverBmp_);
        Storage.remove(ONLINE_COVER_TMP);
        if (!coverBmp_.empty()) MusicCache::writeCoverBmp(track.id, coverBmp_);
      }
    }
  }

  LOG_INF("Music", "Online extras: '%s' lrc=%d cover=%d", tags_.title.c_str(),
          tags_.hasLyrics(), static_cast<int>(!coverBmp_.empty()));
  extrasReady_ = true;
}

void MusicEngine::loadMetadata() {
  tags_ = TrackTags();
  coverBmp_.clear();
  tagBuffer_.clear();

  EmbeddedArt art;
  TrackTagIO::loadId3(*file_, tags_, tagBuffer_, art);
  TrackTagIO::loadSidecarLrc(tracks_[index_], tags_);

  if (art.data && CoverLoader::embeddedToMonoBmp(art, coverBmp_)) {
    // art aliases tagBuffer_; it is no longer needed once converted.
    std::vector<uint8_t>().swap(tagBuffer_);
  } else {
    std::vector<uint8_t>().swap(tagBuffer_);
    const std::string sibling = CoverLoader::findSibling(tracks_[index_]);
    if (!sibling.empty()) {
      const bool isPng = sibling.compare(sibling.size() - 4, 4, ".png") == 0;
      CoverLoader::fileToMonoBmp(sibling.c_str(), isPng, coverBmp_);
    }
  }

  LOG_INF("Music", "Tags: title='%s' artist='%s' lrc=%d cover=%d", tags_.title.c_str(), tags_.artist.c_str(),
          tags_.hasLyrics(), (int)!coverBmp_.empty());
}

bool MusicEngine::startCurrent() {
  const size_t count = activeCount();
  if (count == 0 || index_ >= count) return false;

  // Replace anything currently running. stopPlayback() joins the audio task,
  // so no callback can touch the old source after this returns.
  HalAudioOutput::stopPlayback();

  bool ok = false;
  if (onlineSession_) {
    OnlineTrack& track = online_[index_];

    // (1) Complete cached file: instant start, fully offline capable.
    if (MusicCache::hasCompleteSong(track) && playCachedOnlineTrack(track)) {
      ok = true;
    } else {
      // (2) Network path. Resolve the play URL on the critical path.
      onlinePlayFromFile_ = false;
      if (track.playUrl.empty() &&
          !OnlineMusicApi::resolvePlayUrl(track)) {
        LOG_ERR("Music", "Track not playable: %s", track.title.c_str());
        closeStream();
        sessionActive_ = true;  // keep the session; user can skip
        return false;
      }
      // Evict BEFORE opening, so enableTee follows open() with no delay and no
      // producer bytes are missed. Best effort: failure just skips the tee.
      const uint64_t need = track.size > 0 ? track.size : 4ULL * 1024 * 1024;
      const bool teeAllowed = MusicCache::enforceBudget(need);
      if (!stream_) stream_.reset(new HttpByteStream());
      if (!stream_->open(track.playUrl)) {
        LOG_ERR("Music", "Stream open failed: %s", track.playUrl.c_str());
        closeStream();
        sessionActive_ = true;
        return false;
      }

      // Instant tags so the now-playing frame has title/artist, then arm the
      // tee BEFORE playMp3; audio starts immediately afterward.
      applyOnlineTags(track);
      if (teeAllowed) {
        const uint32_t id = track.id;
        const uint64_t size = stream_->fileSize();
        const std::string url = track.playUrl;
        stream_->setTeeCommitCallback(
            [id, size, url](bool success) {
              if (success) MusicCache::onSongCommitted(id, size, url);
            });
        stream_->enableTee(MusicCache::songPartPath(id),
                           MusicCache::songFinalPath(id), id, url);
        stream_->setTeeHeaderWindow(true);
      }
      ok = HalAudioOutput::playMp3(fileReadTrampoline, fileSeekTrampoline,
                                   fileSizeTrampoline, volume_);
      if (!ok) {
        closeStream();
      } else {
        if (teeAllowed) stream_->setTeeHeaderWindow(false);
        // Slow lyrics/cover fetch (and any network gap-fill) runs only AFTER
        // the music is playing, buffered by the read-ahead ring.
        loadOnlineExtras(track);
      }
    }
  } else {
    closeFile();

    file_ = std::unique_ptr<HalFile>(new HalFile());
    if (!Storage.openFileForRead("Music", tracks_[index_].c_str(), *file_)) {
      LOG_ERR("Music", "Failed to open: %s", tracks_[index_].c_str());
      closeFile();
      sessionActive_ = true;  // keep the session; user can skip the bad file
      return false;
    }

    loadMetadata();

    if (FsHelpers::checkFileExtension(tracks_[index_], ".mp3")) {
      ok = HalAudioOutput::playMp3(fileReadTrampoline, fileSeekTrampoline, fileSizeTrampoline,
                                   volume_);
    } else {
      ok = HalAudioOutput::playWav(fileReadTrampoline, fileSeekTrampoline, volume_, false);
    }
    if (!ok) closeFile();
  }

  sessionActive_ = true;
  return ok;
}

bool MusicEngine::reopenCurrent() {
  const size_t count = activeCount();
  if (count == 0 || index_ >= count) return false;

  // Same teardown as startCurrent, but metadata reload is skipped: tags_ and
  // coverBmp_ remain valid for the same track.
  HalAudioOutput::stopPlayback();

  bool ok = false;
  if (onlineSession_) {
    OnlineTrack& track = online_[index_];
    // A complete cache is preferred (e.g. the file finished downloading after
    // the stream-based start); otherwise restart the HTTP stream at byte 0.
    if (MusicCache::hasCompleteSong(track)) {
      if (!playCachedOnlineTrack(track)) {
        sessionActive_ = true;
        return false;
      }
      ok = true;
    } else {
      onlinePlayFromFile_ = false;
      if (!stream_ || !stream_->seekSet(0)) {
        closeStream();
        sessionActive_ = true;
        return false;
      }
      ok = HalAudioOutput::playMp3(fileReadTrampoline, fileSeekTrampoline,
                                   fileSizeTrampoline, volume_);
      if (!ok) closeStream();
    }
  } else {
    closeFile();

    file_ = std::unique_ptr<HalFile>(new HalFile());
    if (!Storage.openFileForRead("Music", tracks_[index_].c_str(), *file_)) {
      LOG_ERR("Music", "Failed to reopen: %s", tracks_[index_].c_str());
      closeFile();
      sessionActive_ = true;
      return false;
    }

    if (FsHelpers::checkFileExtension(tracks_[index_], ".mp3")) {
      ok = HalAudioOutput::playMp3(fileReadTrampoline, fileSeekTrampoline, fileSizeTrampoline,
                                   volume_);
    } else {
      ok = HalAudioOutput::playWav(fileReadTrampoline, fileSeekTrampoline, volume_, false);
    }
    if (!ok) closeFile();
  }

  sessionActive_ = true;
  return ok;
}

void MusicEngine::buildShuffleOrder(const size_t n, size_t firstTrack) {
  order_.resize(n);
  for (size_t i = 0; i < n; ++i) order_[i] = i;

  // Fisher-Yates; esp_random() is the ESP-IDF hardware RNG (no seeding needed).
  // Modulo bias is below 0.04% for n <= MAX_TRACKS.
  for (size_t i = n; i > 1; --i) {
    const size_t j = esp_random() % i;
    std::swap(order_[i - 1], order_[j]);
  }

  // Put the picked track first so playAt starts where the user selected.
  for (size_t i = 0; i < n; ++i) {
    if (order_[i] == firstTrack) {
      std::swap(order_[i], order_[0]);
      break;
    }
  }
  orderPos_ = 0;
}

bool MusicEngine::playAt(size_t index) {
  if (index >= tracks_.size()) return false;
  if (onlineSession_) {
    // Leaving the online session back to local playback.
    HalAudioOutput::stopPlayback();
    closeStream();
    onlineSession_ = false;
    onlinePlayFromFile_ = false;
    std::vector<OnlineTrack>().swap(online_);
    std::vector<size_t>().swap(order_);
    orderPos_ = 0;
  }
  index_ = index;
  if (mode_ == PlayMode::Shuffle) buildShuffleOrder(tracks_.size(), index);
  return startCurrent();
}

bool MusicEngine::playOnline(std::vector<OnlineTrack> results, const size_t startIndex) {
  if (results.empty() || startIndex >= results.size()) return false;

  HalAudioOutput::stopPlayback();
  closeFile();
  closeStream();

  online_ = std::move(results);
  onlineSession_ = true;
  onlinePlayFromFile_ = false;
  index_ = startIndex;
  std::vector<size_t>().swap(order_);
  orderPos_ = 0;
  if (mode_ == PlayMode::Shuffle) buildShuffleOrder(online_.size(), startIndex);
  return startCurrent();
}

bool MusicEngine::loadOnline(std::vector<OnlineTrack> tracks, const size_t startIndex) {
  if (tracks.empty() || startIndex >= tracks.size()) return false;

  closeFile();
  closeStream();

  online_ = std::move(tracks);
  onlineSession_ = true;
  onlinePlayFromFile_ = false;
  index_ = startIndex;
  std::vector<size_t>().swap(order_);
  orderPos_ = 0;
  if (mode_ == PlayMode::Shuffle) buildShuffleOrder(online_.size(), startIndex);

  // Populate only the static display metadata (pure struct copy); the play URL
  // is not resolved and no stream/audio is touched. sessionActive_ stays false,
  // so update() idles and the audio trampolines can never fire.
  const OnlineTrack& track = online_[index_];
  tags_ = TrackTags();
  tags_.title = track.title;
  tags_.artist = track.artist;
  tags_.album = track.album;
  coverBmp_.clear();
  return true;
}

void MusicEngine::next() {
  const size_t count = activeCount();
  if (count == 0) return;

  if (mode_ == PlayMode::Shuffle) {
    if (order_.empty()) {
      buildShuffleOrder(count, index_);
    } else if (orderPos_ + 1 < order_.size()) {
      index_ = order_[++orderPos_];
    } else {
      // Pass finished: a fresh permutation whose first track differs from the
      // one just played.
      const size_t finished = index_;
      buildShuffleOrder(count, 0);
      if (order_.size() > 1 && order_[0] == finished) {
        std::swap(order_[0], order_[esp_random() % (order_.size() - 1) + 1]);
      }
      index_ = order_[0];
    }
    startCurrent();
    return;
  }

  if (index_ + 1 < count) {
    ++index_;
  } else if (mode_ == PlayMode::Sequential) {
    endSession();  // manual Next past the last track also ends
    return;
  } else {
    index_ = 0;
  }
  startCurrent();
}

void MusicEngine::previous() {
  const size_t count = activeCount();
  if (count == 0) return;

  // Spotify gesture: restart the current track after 3s, otherwise go back.
  HalAudioOutput::Status current;
  if (HalAudioOutput::getStatus(current) && current.positionMs > 3000) {
    reopenCurrent();
    return;
  }

  if (mode_ == PlayMode::Shuffle) {
    if (orderPos_ > 0 && !order_.empty()) {
      index_ = order_[--orderPos_];
      startCurrent();
    } else {
      reopenCurrent();  // first in the permutation: replay it
    }
    return;
  }

  if (index_ == 0) {
    if (mode_ == PlayMode::Sequential) {
      reopenCurrent();
    } else {
      index_ = count - 1;  // RepeatAll/RepeatOne wrap to the last track
      startCurrent();
    }
    return;
  }

  --index_;
  startCurrent();
}

void MusicEngine::togglePause() {
  HalAudioOutput::Status current;
  if (!HalAudioOutput::getStatus(current)) return;
  if (current.paused) {
    HalAudioOutput::resumePlayback();
  } else {
    HalAudioOutput::pausePlayback();
  }
}

void MusicEngine::adjustVolume(int delta) {
  const int next = std::max(0, std::min(100, (int)volume_ + delta));
  volume_ = (uint8_t)next;
  HalAudioOutput::setVolume(volume_);
}

HalAudioOutput::Status MusicEngine::status() const {
  HalAudioOutput::Status current;
  HalAudioOutput::getStatus(current);
  return current;
}

void MusicEngine::setPlayMode(const PlayMode mode) {
  mode_ = mode;

  // Entering Shuffle mid-session: build an order starting with the playing
  // track without interrupting it. Leaving Shuffle resumes linear order from
  // index_.
  if (mode == PlayMode::Shuffle && order_.empty()) {
    buildShuffleOrder(activeCount(), index_);
  } else if (mode != PlayMode::Shuffle) {
    std::vector<size_t>().swap(order_);
    orderPos_ = 0;
  }

  APP_STATE.musicMode = static_cast<uint8_t>(mode_);
  APP_STATE.saveToFile();
}

void MusicEngine::restoreSettings(const uint8_t mode, const uint8_t volume) {
  mode_ = mode <= 3 ? static_cast<PlayMode>(mode) : PlayMode::RepeatAll;
  volume_ = std::min(volume, static_cast<uint8_t>(100));
}

void MusicEngine::persistVolume() {
  APP_STATE.musicVolume = volume_;
  APP_STATE.saveToFile();
}

std::vector<size_t> MusicEngine::upcomingIndices(size_t maxCount) const {
  std::vector<size_t> out;
  const size_t count = activeCount();

  if (mode_ == PlayMode::Shuffle) {
    // Only the remainder of this pass is predicted; no guessing the next.
    for (size_t k = orderPos_ + 1; k < order_.size() && out.size() < maxCount; ++k) {
      out.push_back(order_[k]);
    }
    return out;
  }

  const bool wraps = (mode_ != PlayMode::Sequential);
  for (size_t k = 1; k <= maxCount; ++k) {
    const size_t linear = index_ + k;
    if (linear < count) {
      out.push_back(linear);
    } else if (!wraps) {
      break;
    } else {
      const size_t wrapped = linear % count;
      if (wrapped == index_ || out.size() >= maxCount) break;  // completed a pass
      out.push_back(wrapped);
    }
  }
  return out;
}

void MusicEngine::endSession() {
  HalAudioOutput::stopPlayback();
  closeFile();
  closeStream();
  if (onlineSession_) {
    onlineSession_ = false;
    onlinePlayFromFile_ = false;
    std::vector<OnlineTrack>().swap(online_);
  }
  sessionActive_ = false;  // index_ stays as the list highlight
  LOG_INF("Music", "Sequential pass finished at index %u", static_cast<unsigned>(index_));
}

void MusicEngine::update() {
  if (!sessionActive_) return;
  if (!HalAudioOutput::takeNaturalEnd()) return;

  closeFile();
  if (activeCount() == 0) {
    sessionActive_ = false;
    return;
  }

  switch (mode_) {
    case PlayMode::RepeatOne:
      reopenCurrent();
      break;

    case PlayMode::Shuffle:
      if (order_.empty()) {
        buildShuffleOrder(activeCount(), index_);
        index_ = order_[0];
      } else if (orderPos_ + 1 < order_.size()) {
        index_ = order_[++orderPos_];
      } else {
        const size_t finished = index_;
        buildShuffleOrder(activeCount(), 0);
        if (order_.size() > 1 && order_[0] == finished) {
          std::swap(order_[0], order_[esp_random() % (order_.size() - 1) + 1]);
        }
        index_ = order_[0];
      }
      startCurrent();
      break;

    case PlayMode::Sequential:
      if (index_ + 1 >= activeCount()) {
        endSession();
      } else {
        ++index_;
        startCurrent();
      }
      break;

    case PlayMode::RepeatAll:
      index_ = (index_ + 1) % activeCount();
      startCurrent();
      break;
  }
}

void MusicEngine::stopForUsbDrive() {
  HalAudioOutput::stopPlayback();
  closeFile();
  closeStream();
  tags_ = TrackTags();
  std::vector<uint8_t>().swap(tagBuffer_);
  std::vector<uint8_t>().swap(coverBmp_);
  std::vector<size_t>().swap(order_);
  orderPos_ = 0;
  onlineSession_ = false;
  onlinePlayFromFile_ = false;
  std::vector<OnlineTrack>().swap(online_);
  sessionActive_ = false;
}

// ---------------------------------------------------------------------------
// Audio-task trampolines — SdFat requires every access serialized with the
// main task; StorageLock takes the recursive storage mutex for the call.
// ---------------------------------------------------------------------------

int MusicEngine::fileReadTrampoline(uint8_t* dst, size_t len) {
  MusicEngine& engine = getInstance();
  if (engine.onlineSession_ && !engine.onlinePlayFromFile_) {
    if (!engine.stream_) return -1;
    return engine.stream_->read(dst, len);
  }
  HalStorage::StorageLock lock;
  if (!engine.file_ || !engine.file_->isOpen()) return -1;
  return engine.file_->read(dst, len);
}

bool MusicEngine::fileSeekTrampoline(size_t pos) {
  MusicEngine& engine = getInstance();
  if (engine.onlineSession_ && !engine.onlinePlayFromFile_) {
    if (!engine.stream_) return false;
    return engine.stream_->seekSet(pos);
  }
  HalStorage::StorageLock lock;
  if (!engine.file_ || !engine.file_->isOpen()) return false;
  return engine.file_->seekSet(pos);
}

uint64_t MusicEngine::fileSizeTrampoline() {
  MusicEngine& engine = getInstance();
  if (engine.onlineSession_ && !engine.onlinePlayFromFile_) {
    if (!engine.stream_) return 0;
    return engine.stream_->fileSize();
  }
  HalStorage::StorageLock lock;
  if (!engine.file_ || !engine.file_->isOpen()) return 0;
  return engine.file_->fileSize64();
}
