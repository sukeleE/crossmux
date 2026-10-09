#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint16_t recentOverlaySleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentOverlaySleepPos = 0;
  uint8_t recentOverlaySleepFill = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Most recent epoch timestamp the device trusted as valid. Used by the
  // Reading Analytics suite to bucket reading time into days when the live
  // clock is temporarily unavailable (RTC-less boots before NTP sync).
  uint32_t lastKnownValidTimestamp = 0;

  // Music Player: playback mode (MusicEngine::PlayMode ordinal) and volume 0-100.
  uint8_t musicMode = 1;      // default RepeatAll
  uint8_t musicVolume = 60;

  // Bound NetEase Cloud Music account (empty musicCookie = not bound).
  std::string musicCookie;    // MUSIC_U session token
  std::string musicCsrf;      // __csrf
  uint64_t musicUserId = 0;
  std::string musicNickname;
  std::string musicPhone;  // last phone used for SMS login (prefilled next time)

  // Last-played online selection, used to deep-restore the player on open.
  uint64_t musicLastPlaylistId = 0;   // last playlist (0 = none remembered)
  std::string musicLastPlaylistName;  // display only
  uint32_t musicLastTrackId = 0;      // last track in that playlist

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  bool loadFromFile();
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;
  bool isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void pushRecentOverlaySleep(uint16_t idx);

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
