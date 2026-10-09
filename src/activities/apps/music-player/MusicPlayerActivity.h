#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/apps/music-player/OnlineMusicApi.h"
#include "activities/apps/music-player/QrCode.h"

class MusicPlayerActivity final : public Activity {
 public:
  MusicPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MusicPlayer", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t {
    NowPlaying,
    List,            // local track list (opened from the long-press menu)
    LoginMethod,     // choose SMS-code or QR login (shown when unbound)
    QrBind,          // QR-code account binding
    Playlists,       // bound user's playlist list
    PlaylistTracks,  // one playlist's tracks
  };
  // QR binding screen state.
  enum class BindState : uint8_t { Starting, Waiting, Scanned, Expired, Failed };

  void loopList(uint32_t now);
  void loopNowPlaying(uint32_t now);
  void loopModeMenu();
  void loopLoginMethod();
  void loopQrBind(uint32_t now);
  void loopPlaylists(uint32_t now);
  void loopPlaylistTracks(uint32_t now);

  // True local refresh of only the progress bar + current time (no full
  // repaint), used once per second while the now-playing view is static.
  void refreshProgressLocal();
  // True local refresh of only the lyric region with the currently playing
  // line, used the moment the lyric line changes.
  void refreshLyricLocal();

  void openModeMenu();
  void closeModeMenu();

  void renderList();
  void renderNowPlaying();
  void renderModeMenu();
  void renderLoginMethod();
  void renderQrBind();
  void renderPlaylists();
  void renderPlaylistTracks();

  // Account binding flow.
  void startBinding();
  void enterLoginMethod();
  // SMS-code login: two keyboard steps (phone number, then the SMS code).
  void launchPhoneKeyboard();
  void launchCaptchaKeyboard();
  // On a successful login (SMS or QR 803): persist session + open playlists.
  void onBindConfirmed(const std::string& cookie, const std::string& csrf);
  void loadPlaylists();
  // Apply the once-per-run phone-hotspot link tuning (no modem sleep, lock to
  // 11b/g) and explicitly re-establish the association the live protocol
  // change drops. False if the link does not come back.
  bool prepareHotspotLink();
  void unbindAccount();

  void clampViewport(int visibleRows);

  View view_ = View::NowPlaying;
  int selected_ = 0;
  int viewTop_ = 0;
  bool firstShow_ = true;

  uint32_t lastRenderMs_ = 0;
  // Progress render throttling (at most once per second).
  uint32_t lastProgressSecond_ = UINT32_MAX;
  // Geometry of the bar/time region, cached from the full render for the local
  // refresh; -1 until renderNowPlaying has run.
  int barY_ = -1;
  int timesY_ = -1;
  // Geometry of the lyric region, cached from the full render for the local
  // line-change refresh; -1 until renderNowPlaying has run.
  int lyricTop_ = -1;
  int lyricBottom_ = -1;
  int lyricLineY_ = -1;
  // Counts local refreshes since the last full repaint (periodic ghost clean).
  uint8_t localWindowCount_ = 0;
  size_t lastShownIndex_ = SIZE_MAX;
  // Highlighted lyric line on the last rendered frame.
  int lastLyricIndex_ = -2;

  // Volume popup: visible only while the wheel is being turned, auto-hides
  // shortly after it stops.
  bool volumeOverlay_ = false;
  bool volumePending_ = false;  // wheel tick waiting for the throttle window
  uint32_t lastVolumeEventMs_ = 0;

  // Settings menu overlay (long-press Confirm) drawn above whichever view is
  // active. Level 0 = root settings, level 1 = playback-mode submenu.
  bool modeMenuOpen_ = false;
  uint8_t menuLevel_ = 0;
  int modeMenuSel_ = 0;

  // Login-method chooser + SMS-code flow.
  int loginMethodSel_ = 0;
  std::string loginMessage_;  // non-empty when an error line is shown
  std::string smsPhone_;

  // QR binding.
  BindState bindState_ = BindState::Starting;
  std::string qrKey_;
  QrCode qrMatrix_;
  uint32_t qrLastPollMs_ = 0;

  // Playlist browsing.
  bool browseLoading_ = false;
  std::string browseMessage_;
  std::string browseStage_;  // detailed "what it is doing" line while loading
  uint8_t browsePhase_ = 0;  // staged-loading step (see loopPlaylists)
  uint64_t browseUid_ = 0;   // uid carried across staged ticks
  bool linkPrepared_ = false;  // one-time hotspot link tuning applied
  uint64_t browsePlaylistId_ = 0;
  std::vector<OnlineMusicApi::PlaylistInfo> playlists_;
  std::vector<OnlineTrack> browseTracks_;

  // Deep-restore of the remembered playlist when the player is opened.
  bool restorePending_ = false;  // currently loading the remembered playlist
  bool restoredPaused_ = false;  // queue loaded but audio not started (paused look)
  uint8_t restoreAttempts_ = 0;  // playlist-fetch tries before giving up
  // Whether the offline (WiFi not connected) cache restore has been attempted.
  bool restoreOfflineTried_ = false;
  // Last online index whose track id was persisted (covers next/prev/auto).
  size_t lastPersistedIndex_ = SIZE_MAX;
};
