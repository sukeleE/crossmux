#include "MusicPlayerActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <WiFi.h>
#include <esp_wifi.h>

#include "CoverLoader.h"
#include "CrossPointState.h"
#include "MusicCache.h"
#include "MusicEngine.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/musicTransport.h"
#include "fontIds.h"

namespace {

constexpr int LIST_FONT = UI_12_FONT_ID;
constexpr int SMALL_FONT = UI_10_FONT_ID;
constexpr int TITLE_FONT = NOTOSANS_18_FONT_ID;
constexpr int ARTIST_FONT = NOTOSANS_14_FONT_ID;
constexpr int LYRIC_FONT = UI_12_FONT_ID;

constexpr int COVER_SIZE = CoverLoader::TARGET_SIZE;  // 400

// Now-playing layout shared by the full render and the per-second local
// (windowed) progress refresh. 480-wide portrait content, 40px margins.
constexpr int kMargin = 40;
constexpr int kContentW = 480 - kMargin * 2;  // 400
constexpr int kBarH = 8;
// Number of local progress refreshes before one full repaint clears the
// analog residue that accumulates on the heavily-toggled time digits.
constexpr uint8_t LOCAL_REFRESH_CLEAN_EVERY = 60;
// Root menu: playback mode, track list, rescan, my playlists.
constexpr int ROOT_MENU_ROWS = 4;
constexpr uint32_t QR_POLL_INTERVAL_MS = 2000;
constexpr uint32_t VOLUME_HIDE_MS = 2200;
constexpr uint32_t VOLUME_THROTTLE_MS = 200;
constexpr uint32_t MODE_LONGPRESS_MS = 600;

const char* baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

std::string displayTitle(const std::string& path, const std::string& tagged) {
  if (!tagged.empty()) return tagged;
  std::string name = baseName(path);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name.resize(dot);
  return name;
}

void formatClock(char* dst, size_t dstSize, uint32_t ms) {
  const uint32_t totalSec = ms / 1000;
  snprintf(dst, dstSize, "%lu:%02lu", static_cast<unsigned long>(totalSec / 60),
           static_cast<unsigned long>(totalSec % 60));
}

// Plot a flat n*n 1-bpp glyph pixel by pixel. GfxRenderer::drawIcon hardcodes a
// (size-1-row) vertical flip for its Portrait-only callers; blitGlyph maps
// rows straight down and cols straight right.
void blitGlyph(const GfxRenderer& r, const uint8_t* bits, int size, int x, int y, bool ink) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const uint8_t byte = bits[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) {
        r.drawPixel(x + col, y + row, ink);
      }
    }
  }
}

// One-pixel midpoint circle. Used for the generic vinyl-record cover.
void strokeRing(const GfxRenderer& r, int cx, int cy, int radius, bool ink) {
  int x = radius;
  int y = 0;
  int err = 1 - radius;
  while (x >= y) {
    r.drawPixel(cx + x, cy + y, ink);
    r.drawPixel(cx - x, cy + y, ink);
    r.drawPixel(cx + x, cy - y, ink);
    r.drawPixel(cx - x, cy - y, ink);
    r.drawPixel(cx + y, cy + x, ink);
    r.drawPixel(cx - y, cy + x, ink);
    r.drawPixel(cx + y, cy - x, ink);
    r.drawPixel(cx - y, cy - x, ink);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

// Filled circle via horizontal spans.
void fillDisc(const GfxRenderer& r, int cx, int cy, int radius, bool ink) {
  for (int dy = -radius; dy <= radius; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) ++dx;
    r.drawLine(cx - dx, cy + dy, cx + dx, cy + dy, 1, ink);
  }
}

// Universal cover for tag-less tracks: a vinyl record on a black tile.
void drawGenericCover(const GfxRenderer& r, int x, int y, int size) {
  r.fillRoundedRect(x, y, size, size, 6, Black);

  const int cx = x + size / 2;
  const int cy = y + size / 2;

  // Grooves: thin white rings outside the centre label.
  const int outer = size / 2 - 14;
  for (int radius = 70; radius <= outer; radius += 18) {
    strokeRing(r, cx, cy, radius, false);
  }

  // Centre label with a note glyph.
  constexpr int kLabelR = 52;
  fillDisc(r, cx, cy, kLabelR, false);
  const int noteSize = MusicTransport::note_size;  // 40
  blitGlyph(r, MusicTransport::note, noteSize, cx - noteSize / 2, cy - noteSize / 2, true);
}

const char* modeName(const MusicEngine::PlayMode mode) {
  switch (mode) {
    case MusicEngine::PlayMode::Sequential:
      return tr(STR_MUSIC_PLAYER_MODE_SEQUENTIAL);
    case MusicEngine::PlayMode::RepeatAll:
      return tr(STR_MUSIC_PLAYER_MODE_REPEAT_ALL);
    case MusicEngine::PlayMode::RepeatOne:
      return tr(STR_MUSIC_PLAYER_MODE_REPEAT_ONE);
    case MusicEngine::PlayMode::Shuffle:
      return tr(STR_MUSIC_PLAYER_MODE_SHUFFLE);
  }
  return "";
}

}  // namespace

void MusicPlayerActivity::onEnter() {
  Activity::onEnter();

  // Reinstall the persisted NetEase session before any online request.
  LOG_INF("MP", "restore session cookie=%u csrf=%u phone=%u",
          static_cast<unsigned>(APP_STATE.musicCookie.size()),
          static_cast<unsigned>(APP_STATE.musicCsrf.size()),
          static_cast<unsigned>(APP_STATE.musicPhone.size()));
  if (!APP_STATE.musicCookie.empty()) {
    OnlineMusicApi::setSession(APP_STATE.musicCookie, APP_STATE.musicCsrf);
  }

  auto& engine = MusicEngine::getInstance();

  // Reset transient view state shared by every entry path.
  restorePending_ = false;
  restoredPaused_ = false;
  restoreAttempts_ = 0;
  volumeOverlay_ = false;
  volumePending_ = false;
  modeMenuOpen_ = false;
  menuLevel_ = 0;
  lastProgressSecond_ = UINT32_MAX;
  lastLyricIndex_ = -2;

  // Deep-restore: when a playlist is remembered and nothing is currently
  // playing, load that playlist straight to a paused now-playing screen,
  // skipping the local rescan. The track fetch completes in loopPlaylistTracks
  // (it needs WiFi up); never reach DNS before the network is ready.
  if (APP_STATE.musicLastPlaylistId != 0 && !engine.soundSuppressed()) {
    browsePlaylistId_ = APP_STATE.musicLastPlaylistId;
    browseMessage_ = APP_STATE.musicLastPlaylistName;
    browseTracks_.clear();
    std::vector<OnlineMusicApi::PlaylistInfo>().swap(playlists_);
    view_ = View::PlaylistTracks;
    browseLoading_ = true;
    restorePending_ = true;
    selected_ = 0;
    viewTop_ = 0;
    firstShow_ = true;
    lastShownIndex_ = SIZE_MAX;
    lastPersistedIndex_ = SIZE_MAX;
    requestUpdate();
    return;
  }

  // The now-playing screen is the landing view, whether or not a session is
  // running. An online session keeps its playlist/index; don't rescan over it.
  if (engine.soundSuppressed()) {
    selected_ = static_cast<int>(engine.currentIndex());
    if (!engine.isOnlineSession()) engine.rescan();
    lastPersistedIndex_ = engine.isOnlineSession() ? engine.currentIndex() : SIZE_MAX;
  } else {
    const size_t count = engine.rescan();
    selected_ = count > 0 ? 0 : -1;
    lastPersistedIndex_ = SIZE_MAX;
  }
  view_ = View::NowPlaying;
  viewTop_ = 0;
  firstShow_ = true;
  lastShownIndex_ = SIZE_MAX;
  requestUpdate();
}

void MusicPlayerActivity::loop() {
  const uint32_t now = millis();

  // Heartbeat: proves the activity is being driven and shows the exact loading
  // state while we debug the post-login hang.
  static uint32_t lastHbMs = 0;
  if (now - lastHbMs >= 1000) {
    lastHbMs = now;
    LOG_INF("MP", "hb view=%d phase=%u loading=%d wifi=%d",
            static_cast<int>(view_), browsePhase_, browseLoading_ ? 1 : 0,
            WiFi.status());
  }

  // Mode menu captures every key while open.
  if (modeMenuOpen_) {
    loopModeMenu();
    return;
  }

  if (view_ == View::NowPlaying) {
    loopNowPlaying(now);
  } else if (view_ == View::List) {
    loopList(now);
  } else if (view_ == View::LoginMethod) {
    loopLoginMethod();
  } else if (view_ == View::QrBind) {
    loopQrBind(now);
  } else if (view_ == View::Playlists) {
    loopPlaylists(now);
  } else {
    loopPlaylistTracks(now);
  }
}

void MusicPlayerActivity::clampViewport(int visibleRows) {
  auto& engine = MusicEngine::getInstance();
  const int count = static_cast<int>(engine.trackCount());
  if (count <= 0) {
    selected_ = -1;
    viewTop_ = 0;
    return;
  }
  selected_ = std::clamp(selected_, 0, count - 1);
  const int maxTop = std::max(0, count - visibleRows);
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
  if (selected_ < viewTop_) viewTop_ = selected_;
  if (selected_ >= viewTop_ + visibleRows) viewTop_ = selected_ - visibleRows + 1;
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
}

void MusicPlayerActivity::loopList(uint32_t now) {
  (void)now;
  auto& engine = MusicEngine::getInstance();

  // Long-press Confirm: mode menu (before the short-press play handling).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, MODE_LONGPRESS_MS)) {
    openModeMenu();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // The list is a menu level: Back returns to the now-playing screen.
    selected_ = static_cast<int>(engine.currentIndex());
    view_ = View::NowPlaying;
    firstShow_ = true;
    lastRenderMs_ = 0;
    requestUpdate();
    return;
  }

  const int rowHeight = renderer.getLineHeight(LIST_FONT) + 12;
  // Geometry mirrors renderList(): header sits at the top of the landscape
  // safe area; the list fills the rest of the height.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = safe.y + safe.height - listTop;
  const int visibleRows = std::max(1, listHeight / rowHeight);

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selected_ > 0) --selected_;
    clampViewport(visibleRows);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selected_ + 1 < static_cast<int>(engine.trackCount())) ++selected_;
    clampViewport(visibleRows);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    viewTop_ = std::max(0, viewTop_ - visibleRows);
    selected_ = viewTop_;
    clampViewport(visibleRows);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    const int count = static_cast<int>(engine.trackCount());
    viewTop_ = std::clamp(viewTop_ + visibleRows, 0, std::max(0, count - visibleRows));
    selected_ = viewTop_;
    clampViewport(visibleRows);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (engine.playAt(static_cast<size_t>(selected_))) {
      view_ = View::NowPlaying;
      firstShow_ = true;
      lastRenderMs_ = 0;
      lastProgressSecond_ = UINT32_MAX;
      lastShownIndex_ = SIZE_MAX;
      lastLyricIndex_ = -2;
      volumeOverlay_ = false;
      volumePending_ = false;
      requestUpdate();
    }
  }
}

void MusicPlayerActivity::loopNowPlaying(uint32_t now) {
  auto& engine = MusicEngine::getInstance();

  // Lyrics/cover finished loading in the background: repaint to show them.
  if (engine.consumeExtrasReady()) requestUpdate();

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, MODE_LONGPRESS_MS)) {
    openModeMenu();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Now-playing is the player's root screen: return to the home page (where
    // the player is launched from). The engine keeps playing if a session is.
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (restoredPaused_) {
      // A merely-loaded queue has no audio task yet, so togglePause() would
      // no-op (HAL status unavailable). Resolve/open/start now via the same
      // blocking path the manual play uses, with a loading popup.
      GUI.drawPopup(renderer, tr(STR_MUSIC_PLAYER_OPENING));
      if (engine.startCurrent()) {
        restoredPaused_ = false;
        firstShow_ = true;
        lastRenderMs_ = 0;
        lastProgressSecond_ = UINT32_MAX;
        lastShownIndex_ = SIZE_MAX;
        lastLyricIndex_ = -2;
        requestUpdate();
      } else {
        GUI.drawPopup(renderer, tr(STR_MUSIC_PLAYER_PLAY_FAILED));
        delay(1600);
        requestUpdate();
      }
    } else {
      engine.togglePause();
      requestUpdate();
    }
  }

  // Wheel alone: skip tracks. Up = previous, Down = next. Read both edges
  // first; evaluating one inside the `if` condition consumes it, so
  // re-reading it for the direction would always see false.
  const bool wheelUp = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool wheelDown = mappedInput.wasReleased(MappedInputManager::Button::Down);
  if (wheelUp) engine.previous();
  if (wheelDown) engine.next();
  if (wheelUp || wheelDown) {
    // Skipping from the paused restored state starts playback.
    restoredPaused_ = false;
    requestUpdate();
  }

  // BOOT held turns the wheel into Left/Right: volume. The popup follows the
  // chord and auto-hides once it stops.
  const bool chordUp = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool chordDown = mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (chordUp || chordDown) {
    engine.adjustVolume(chordUp ? 3 : -3);
    volumeOverlay_ = true;
    lastVolumeEventMs_ = now;
    if (now - lastRenderMs_ >= VOLUME_THROTTLE_MS) {
      requestUpdate();
    } else {
      volumePending_ = true;
    }
  }
  if (volumePending_ && now - lastRenderMs_ >= VOLUME_THROTTLE_MS) {
    volumePending_ = false;
    requestUpdate();
  }
  if (volumeOverlay_ && now - lastVolumeEventMs_ >= VOLUME_HIDE_MS && now - lastRenderMs_ >= 300) {
    volumeOverlay_ = false;
    engine.persistVolume();  // one SD write when the wheel stops, not per tick
    requestUpdate();
  }

  // A Sequential pass that ended while another activity was on top leaves the
  // screen focused on the final track (no forced view switch).

  const HalAudioOutput::Status status = engine.status();

  // Track switch (manual skip or auto-advance): immediate repaint.
  if (engine.currentIndex() != lastShownIndex_) {
    lastLyricIndex_ = engine.tags().lineIndexAt(status.positionMs);
    // Remember the newly current online track (next/previous or auto-advance).
    if (engine.isOnlineSession() && engine.currentIndex() != lastPersistedIndex_) {
      const uint32_t newId = engine.onlineId(engine.currentIndex());
      if (newId != 0) {
        APP_STATE.musicLastTrackId = newId;
        APP_STATE.saveToFile();
      }
      lastPersistedIndex_ = engine.currentIndex();
    }
    LOG_INF("MP", "repaint reason=IDX cur=%u last=%u",
            static_cast<unsigned>(engine.currentIndex()),
            static_cast<unsigned>(lastShownIndex_));
    requestUpdate();
  }

  // Lyric line change: redraw ONLY the lyric region with the newly current
  // line instead of repainting the whole screen, so the displayed line
  // follows playback in real time. Latch immediately to avoid refires.
  const int lyricIndex = engine.tags().lineIndexAt(status.positionMs);
  if (lyricIndex != lastLyricIndex_) {
    const bool canWindow = view_ == View::NowPlaying && !modeMenuOpen_ &&
                           !firstShow_ && lyricTop_ >= 0 &&
                           !engine.tags().synced.empty();
    lastLyricIndex_ = lyricIndex;
    if (canWindow && status.active) {
      refreshLyricLocal();
    } else {
      requestUpdate();
    }
  }

  // Progress: at most one refresh per second. When the now-playing frame is
  // static (no menu/overlay, past its first full paint), do a true local
  // refresh of only the progress bar + current time instead of repainting the
  // whole screen; the cover and other static elements are never touched.
  const uint32_t currentSecond = status.positionMs / 1000;
  if (status.active && currentSecond != lastProgressSecond_ && now - lastRenderMs_ >= 1000) {
    const bool canWindow = view_ == View::NowPlaying && !modeMenuOpen_ && !volumeOverlay_ &&
                           !firstShow_ && barY_ >= 0;
    if (canWindow && ++localWindowCount_ < LOCAL_REFRESH_CLEAN_EVERY) {
      refreshProgressLocal();
      // Record the second we just showed, otherwise this branch refires on the
      // very next loop (the local path does not touch lastRenderMs_, so the
      // 1000 ms gate alone would not throttle it) -> a refresh storm.
      lastProgressSecond_ = currentSecond;
    } else {
      // First frame, an open overlay/menu, or the periodic ghost cleanup:
      // a HALF full-screen refresh actually clears the residue accumulated
      // by the per-second differential windows (another FAST would not).
      localWindowCount_ = 0;
      renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
      requestUpdate();
    }
  }
}

// Out-of-band, once-per-second local refresh of only the progress bar and the
// current-time text. Runs on the Arduino main task while the render task may
// be idle; RenderLock serializes against a concurrent full render. Every
// pixel outside this region is left exactly as the entry full-render drew it,
// so the cover/title/transport never flash.
void MusicPlayerActivity::refreshProgressLocal() {
  auto& engine = MusicEngine::getInstance();
  const HalAudioOutput::Status s = engine.status();

  RenderLock lock;

  // Redraw the bar. drawRoundedRect strokes only the outline, so first erase
  // the whole slot white (also makes a backward seek shrink the fill cleanly),
  // then stroke the track, then paint the fill to the current position.
  renderer.fillRoundedRect(kMargin, barY_, kContentW, kBarH, kBarH / 2, White);
  renderer.drawRoundedRect(kMargin, barY_, kContentW, kBarH, 1, kBarH / 2, true);
  if (s.durationMs > 0 && s.positionMs <= s.durationMs) {
    const int fillW = std::clamp(
        (int)((uint64_t)(kContentW - kBarH) * s.positionMs / s.durationMs), 0, kContentW - kBarH);
    if (fillW > 0) {
      renderer.fillRoundedRect(kMargin, barY_, fillW + kBarH, kBarH, kBarH / 2, Black);
    }
  }

  // Current time: erase a fixed-width slot (wide enough for any m:ss value so
  // changing digits never leave residue), then draw the new time. The total
  // time on the right edge is outside this slot and is not touched.
  const int textH = renderer.getTextHeight(SMALL_FONT);
  const int eraseW = std::max(48, renderer.getTextWidth(SMALL_FONT, "59:59"));
  renderer.fillRect(kMargin, timesY_, eraseW, textH, false);
  char time[16];
  formatClock(time, sizeof(time), s.positionMs);
  renderer.drawText(SMALL_FONT, kMargin, timesY_, time);

  // Drive only the union logical rect covering the bar and the time row; the
  // renderer rotates it to the physical strip and byte-aligns it internally.
  renderer.displayWindow(kMargin, barY_, kContentW, timesY_ + textH - barY_);
}

// Out-of-band local refresh of ONLY the lyric region: erase the previous
// current line and draw the newly current line (bold, vertically centered).
// Everything else on screen is untouched, so line changes are instant and
// add no full-screen flicker.
void MusicPlayerActivity::refreshLyricLocal() {
  auto& engine = MusicEngine::getInstance();
  const auto& tags = engine.tags();
  const int current = tags.lineIndexAt(engine.status().positionMs);
  if (current < 0 || current >= static_cast<int>(tags.synced.size())) return;

  RenderLock lock;

  renderer.fillRect(kMargin, lyricTop_, kContentW, lyricBottom_ - lyricTop_, false);
  const std::string shown =
      renderer.truncatedText(LYRIC_FONT, tags.synced[current].text.c_str(), kContentW);
  renderer.drawText(LYRIC_FONT, kMargin, lyricLineY_, shown.c_str(), true,
                    EpdFontFamily::BOLD);

  renderer.displayWindow(kMargin, lyricTop_, kContentW, lyricBottom_ - lyricTop_);
}

// ---------------------------------------------------------------------------
// Mode menu overlay
// ---------------------------------------------------------------------------

void MusicPlayerActivity::openModeMenu() {
  modeMenuOpen_ = true;
  menuLevel_ = 0;
  modeMenuSel_ = 0;
  renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);  // less ghosting
  requestUpdate();
}

void MusicPlayerActivity::closeModeMenu() {
  modeMenuOpen_ = false;
  menuLevel_ = 0;
  renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
  requestUpdate();
}

void MusicPlayerActivity::loopModeMenu() {
  using B = MappedInputManager::Button;
  auto& engine = MusicEngine::getInstance();

  const int rowCount = menuLevel_ == 0 ? ROOT_MENU_ROWS : 4;

  // Up/Down move the cursor (also the BOOT-chord Left/Right on this board).
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Left)) {
    modeMenuSel_ = (modeMenuSel_ + rowCount - 1) % rowCount;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Right)) {
    modeMenuSel_ = (modeMenuSel_ + 1) % rowCount;
    requestUpdate();
  }

  if (mappedInput.wasReleased(B::Confirm)) {
    if (menuLevel_ == 0) {
      if (modeMenuSel_ == 0) {
        // Open the playback-mode submenu.
        menuLevel_ = 1;
        modeMenuSel_ = static_cast<int>(engine.playMode());
        requestUpdate();
      } else if (modeMenuSel_ == 1) {
        // Open the local track list as a menu level.
        modeMenuOpen_ = false;
        menuLevel_ = 0;
        renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
        selected_ = static_cast<int>(engine.currentIndex());
        view_ = View::List;
        firstShow_ = true;
        requestUpdate();
      } else if (modeMenuSel_ == 2) {
        // Rescan the library, then show the list.
        const size_t count = engine.rescan();
        modeMenuOpen_ = false;
        menuLevel_ = 0;
        view_ = View::List;
        selected_ = count > 0 ? 0 : -1;
        viewTop_ = 0;
        firstShow_ = true;
        renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
        requestUpdate();
      } else {
        // My playlists. The persisted cookie is authoritative: if the in-memory
        // jar was lost (a reset path, heap hiccup) restore it here instead of
        // forcing the user through login again. Only a clean unbind clears it.
        modeMenuOpen_ = false;
        menuLevel_ = 0;
        renderer.requestNextRefresh(HalDisplay::HALF_REFRESH);
        if (!OnlineMusicApi::hasSession() && !APP_STATE.musicCookie.empty()) {
          OnlineMusicApi::setSession(APP_STATE.musicCookie, APP_STATE.musicCsrf);
        }
        if (OnlineMusicApi::hasSession()) {
          // Gate on WiFi even with a session. This path used to call
          // loadPlaylists() directly, but after a fresh reboot WiFi/tcpip may
          // be uninitialized and the first DNS lookup asserts on a NULL tcpip
          // lock (xQueueSemaphoreTake) -> crash. Bring the network up first.
          if (WiFi.status() == WL_CONNECTED) {
            loadPlaylists();
          } else {
            auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
            if (!wifi) {
              LOG_ERR("OnlineMusic", "OOM: WifiSelectionActivity");
              return;
            }
            startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
              if (!result.isCancelled && WiFi.status() == WL_CONNECTED) loadPlaylists();
            });
          }
        } else {
          startBinding();
        }
      }
    } else {
      // Pick the mode and return to the root (accessory now reflects it).
      engine.setPlayMode(static_cast<MusicEngine::PlayMode>(modeMenuSel_));
      menuLevel_ = 0;
      modeMenuSel_ = 0;
      requestUpdate();
    }
    return;
  }

  // Back: one level up from the submenu, dismiss from the root.
  if (mappedInput.wasReleased(B::Back)) {
    if (menuLevel_ == 0) {
      closeModeMenu();
    } else {
      menuLevel_ = 0;
      modeMenuSel_ = 0;
      requestUpdate();
    }
    return;
  }

  // Another long-press: close the whole menu.
  if (mappedInput.wasLongPressed(B::Confirm, MODE_LONGPRESS_MS)) {
    closeModeMenu();
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void MusicPlayerActivity::render(RenderLock&&) {
  LOG_INF("MP", "render view=%d menu=%d lvl=%d", static_cast<int>(view_),
          static_cast<int>(modeMenuOpen_), static_cast<int>(menuLevel_));
  renderer.clearScreen();

  if (view_ == View::NowPlaying) {
    // Full-bleed now-playing screen; the standard app header is dropped to
    // make room for cover + lyrics.
    renderNowPlaying();
  } else if (view_ == View::List) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    GUI.drawHeader(renderer,
                   Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                   tr(STR_MUSIC_PLAYER_TRACK_LIST));
    renderList();
  } else if (view_ == View::LoginMethod) {
    renderLoginMethod();
  } else if (view_ == View::QrBind) {
    renderQrBind();
  } else if (view_ == View::Playlists) {
    renderPlaylists();
  } else {
    renderPlaylistTracks();
  }

  if (modeMenuOpen_) renderModeMenu();

  renderer.displayBuffer(firstShow_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstShow_ = false;
  lastRenderMs_ = millis();
}

void MusicPlayerActivity::renderList() {
  auto& engine = MusicEngine::getInstance();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const int listX = safe.x + metrics.contentSidePadding;
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listWidth = safe.width - metrics.contentSidePadding * 2;
  const int listHeight = safe.y + safe.height - listTop;

  if (engine.trackCount() == 0) {
    const auto lines = renderer.wrappedText(LIST_FONT, tr(STR_MUSIC_PLAYER_EMPTY), listWidth, 3);
    const int lineHeight = renderer.getLineHeight(LIST_FONT);
    int y = listTop + std::max(0, (listHeight - static_cast<int>(lines.size()) * lineHeight) / 2);
    for (const std::string& line : lines) {
      renderer.drawCenteredText(LIST_FONT, y, line.c_str());
      y += lineHeight;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int rowHeight = renderer.getLineHeight(LIST_FONT) + 12;
  const int visibleRows = std::max(1, listHeight / rowHeight);
  clampViewport(visibleRows);

  for (int row = 0; row < visibleRows; ++row) {
    const int index = viewTop_ + row;
    if (index < 0 || index >= static_cast<int>(engine.trackCount())) break;

    const int y = listTop + row * rowHeight;
    const bool selected = index == selected_;
    if (selected) {
      renderer.fillRoundedRect(listX, y + 3, listWidth, rowHeight - 6, 6, Black);
    }
    const std::string label = renderer.truncatedText(LIST_FONT, baseName(engine.trackPath(index)),
                                                     listWidth - 16);
    const int textY = y + (rowHeight - renderer.getTextHeight(LIST_FONT)) / 2;
    renderer.drawText(LIST_FONT, listX + 8, textY, label.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_MUSIC_PLAYER_PLAY), nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MusicPlayerActivity::renderNowPlaying() {
  auto& engine = MusicEngine::getInstance();

  // No tracks at all: nothing meaningful to show.
  if (engine.activeCount() == 0) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
    renderer.drawCenteredText(LIST_FONT, safe.height / 2 - 10,
                              tr(STR_MUSIC_PLAYER_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  // No button hints on this screen: use the full 480x800 panel.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
  // Some render-path strip math emits a few hundred pixels off-screen each
  // frame (invisible, but it logged a flood); clip the whole screen so they
  // are dropped silently.
  const GfxRenderer::ClipScope fullScreenClip(renderer, safe.x, safe.y,
                                              safe.width, safe.height);

  // Spotify mobile layout, 480 wide: large centered cover on top, then
  // title/artist, transport row, progress, and lyrics filling the bottom.
  // kMargin / kContentW come from the file's anonymous namespace so the
  // per-second local refresh shares identical geometry.
  const size_t index = engine.currentIndex();
  const TrackTags& tags = engine.tags();
  const HalAudioOutput::Status status = engine.status();
  // A restored, never-started queue is shown paused; before the audio layer has
  // a status, take the duration from the track itself (position stays 0).
  const bool showPaused = restoredPaused_ || status.paused;
  const uint32_t durMs =
      status.durationMs > 0 ? status.durationMs : (restoredPaused_ ? engine.onlineDurationMs(index) : 0);

  // --- Album cover -----------------------------------------------------------
  const int coverSize = std::min(COVER_SIZE, safe.width - kMargin * 2);
  const int coverX = (safe.width - coverSize) / 2;
  constexpr int coverY = 8;

  bool drewCover = false;
  if (engine.hasCover()) {
    const std::vector<uint8_t>& bmpBytes = engine.coverBmp();
    Bitmap cover(bmpBytes.data(), bmpBytes.size());
    if (cover.parseHeaders() == BmpReaderError::Ok && cover.getWidth() == COVER_SIZE &&
        cover.getHeight() == COVER_SIZE) {
      renderer.drawBitmap1Bit(cover, coverX, coverY, coverSize, coverSize);
      drewCover = true;
    }
  }
  if (!drewCover) {
    drawGenericCover(renderer, coverX, coverY, coverSize);
  }

  // --- Title / artist --------------------------------------------------------
  int y = coverY + coverSize + 12;

  const std::string rawTitle = engine.isOnlineSession()
                                   ? engine.onlineTitle(index)
                                   : displayTitle(engine.trackPath(index), tags.title);
  const std::string titleShown = renderer.truncatedText(TITLE_FONT, rawTitle.c_str(), kContentW);
  renderer.drawText(TITLE_FONT, kMargin, y, titleShown.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(TITLE_FONT) + 2;

  if (!tags.artist.empty() || !tags.album.empty()) {
    std::string sub = tags.artist;
    if (!tags.album.empty()) {
      sub += " · ";
      sub += tags.album;
    }
    const std::string shown = renderer.truncatedText(ARTIST_FONT, sub.c_str(), kContentW);
    renderer.drawText(ARTIST_FONT, kMargin, y, shown.c_str());
    y += renderer.getLineHeight(ARTIST_FONT);
  }

  // --- Transport row ---------------------------------------------------------
  constexpr int kRowH = 60;
  const int rowTop = y + 12;

  constexpr int kGlyph = 40;
  constexpr int kCircle = 60;
  const int totalW = kGlyph * 4 + kCircle;
  const int gap = std::max(0, (kContentW - totalW) / 4);

  const int xShuffle = kMargin;
  const int xPrev = xShuffle + kGlyph + gap;
  const int xPlay = xPrev + kGlyph + gap;
  const int xNext = xPlay + kCircle + gap;
  const int xRepeat = xNext + kGlyph + gap;

  const int glyphY = rowTop + (kRowH - kGlyph) / 2;

  // Shuffle toggle.
  const bool shuffleOn = engine.playMode() == MusicEngine::PlayMode::Shuffle;
  if (shuffleOn) {
    renderer.fillRoundedRect(xShuffle, glyphY, kGlyph, kGlyph, 8, Black);
    blitGlyph(renderer, MusicTransport::shuffle, kGlyph, xShuffle, glyphY, false);
  } else {
    blitGlyph(renderer, MusicTransport::shuffle, kGlyph, xShuffle, glyphY, true);
  }

  // Previous track.
  blitGlyph(renderer, MusicTransport::prev, kGlyph, xPrev, glyphY, true);

  // Play/pause circle (state shown by the glyph, so no separate paused line).
  renderer.fillRoundedRect(xPlay, rowTop, kCircle, kCircle, kCircle / 2, Black);
  const int playGlyph = MusicTransport::play_size;  // 36
  const int playXY = rowTop + (kCircle - playGlyph) / 2;
  blitGlyph(renderer,
            showPaused ? MusicTransport::play : MusicTransport::pause, playGlyph, xPlay + playXY,
            rowTop + playXY, false);

  // Next track.
  blitGlyph(renderer, MusicTransport::next, kGlyph, xNext, glyphY, true);

  // Repeat toggle: repeat glyph, repeat-one glyph when RepeatOne.
  const auto mode = engine.playMode();
  const bool repeatOn = mode == MusicEngine::PlayMode::RepeatAll ||
                        mode == MusicEngine::PlayMode::RepeatOne;
  const uint8_t* repeatBits =
      mode == MusicEngine::PlayMode::RepeatOne ? MusicTransport::repeatOne : MusicTransport::repeat;
  if (repeatOn) {
    renderer.fillRoundedRect(xRepeat, glyphY, kGlyph, kGlyph, 8, Black);
    blitGlyph(renderer, repeatBits, kGlyph, xRepeat, glyphY, false);
  } else {
    blitGlyph(renderer, repeatBits, kGlyph, xRepeat, glyphY, true);
  }

  // --- Progress bar + times --------------------------------------------------
  const int barY = rowTop + kRowH + 10;
  renderer.drawRoundedRect(kMargin, barY, kContentW, kBarH, 1, kBarH / 2, true);
  if (durMs > 0 && status.positionMs <= durMs) {
    const int fillW = std::clamp(
        (int)((uint64_t)(kContentW - kBarH) * status.positionMs / durMs), 0, kContentW - kBarH);
    if (fillW > 0) {
      renderer.fillRoundedRect(kMargin, barY, fillW + kBarH, kBarH, kBarH / 2, Black);
    }
  }

  const int timesY = barY + kBarH + 6;
  // Cache geometry for the per-second local refresh of just this region.
  barY_ = barY;
  timesY_ = timesY;
  char time[16];
  formatClock(time, sizeof(time), status.positionMs);
  renderer.drawText(SMALL_FONT, kMargin, timesY, time);
  if (durMs > 0) {
    char total[16];
    formatClock(total, sizeof(total), durMs);
    const int w = renderer.getTextWidth(SMALL_FONT, total);
    renderer.drawText(SMALL_FONT, kMargin + kContentW - w, timesY, total);
  }

  // --- Lower area: lyrics first, otherwise up-next queue ----------------------
  const int lowerTop = timesY + renderer.getLineHeight(SMALL_FONT) + 12;
  // Keep the last 20px clear for the tiny chord hint.
  const int lowerBottom = safe.y + safe.height - 20;
  const int lineStep = renderer.getLineHeight(LYRIC_FONT) + 6;

  // Cache the lower-region geometry for the real-time local lyric refresh;
  // the refresh erases exactly this region before drawing the new line.
  lyricTop_ = lowerTop;
  lyricBottom_ = lowerBottom;

  if (!tags.synced.empty()) {
    // Only the line currently playing -- no context lines, no scrolling.
    // Bold, vertically centered in the lower region.
    const int current = tags.lineIndexAt(status.positionMs);
    const int lyricH = renderer.getLineHeight(LYRIC_FONT);
    lyricLineY_ = lowerTop + std::max(0, (lowerBottom - lowerTop - lyricH) / 2);
    if (current >= 0 && current < static_cast<int>(tags.synced.size())) {
      const std::string shown =
          renderer.truncatedText(LYRIC_FONT, tags.synced[current].text.c_str(), kContentW);
      renderer.drawText(LYRIC_FONT, kMargin, lyricLineY_, shown.c_str(), true,
                        EpdFontFamily::BOLD);
    }
  } else if (!tags.unsyncedLyrics.empty()) {
    const int maxLines = std::max(1, (lowerBottom - lowerTop) / lineStep);
    const auto lines = renderer.wrappedText(LYRIC_FONT, tags.unsyncedLyrics.c_str(), kContentW, maxLines);
    int lineY = lowerTop;
    for (const std::string& line : lines) {
      if (lineY + renderer.getLineHeight(LYRIC_FONT) > lowerBottom) break;
      renderer.drawText(LYRIC_FONT, kMargin, lineY, line.c_str());
      lineY += lineStep;
    }
  } else {
    renderer.drawText(SMALL_FONT, kMargin, lowerTop, tr(STR_MUSIC_PLAYER_UP_NEXT), true,
                      EpdFontFamily::BOLD);

    const auto upcoming = engine.upcomingIndices(3);
    int slot = 0;
    for (size_t trackIndex : upcoming) {
      const int rowY = lowerTop + renderer.getLineHeight(SMALL_FONT) + 8 + slot * lineStep;
      if (rowY + renderer.getLineHeight(LYRIC_FONT) > lowerBottom) break;
      const std::string entryLabel =
          engine.isOnlineSession()
              ? engine.onlineTitle(trackIndex)
              : displayTitle(engine.trackPath(trackIndex), "");
      const std::string shown =
          renderer.truncatedText(LYRIC_FONT, entryLabel.c_str(), kContentW);
      renderer.drawText(LYRIC_FONT, kMargin, rowY, shown.c_str());
      ++slot;
    }
  }

  // --- Volume popup (last, so it sits over everything) -----------------------
  if (volumeOverlay_) {
    constexpr int bw = 300;
    constexpr int bh = 92;
    const int bx = (safe.width - bw) / 2;
    const int by = (safe.height - bh) / 2;

    renderer.fillRoundedRect(bx, by, bw, bh, 10, Black);

    char label[24];
    snprintf(label, sizeof(label), "%s %d%%", tr(STR_MUSIC_PLAYER_VOLUME), engine.volume());
    renderer.drawCenteredText(LYRIC_FONT, by + 12, label, false, EpdFontFamily::BOLD);

    const int barX = bx + 24;
    const int innerW = bw - 48;
    const int vBarY = by + 50;
    renderer.drawRoundedRect(barX, vBarY, innerW, 12, 1, 6, false);
    const int fillW = std::clamp((int)((uint64_t)(innerW - 12) * engine.volume() / 100), 0,
                                 innerW - 12);
    if (fillW > 0) {
      renderer.fillRoundedRect(barX, vBarY, fillW + 12, 12, 6, White);
    }
  }

  // Tiny static hint: the volume gesture is now BOOT+wheel.
  renderer.drawCenteredText(SMALL_FONT, safe.y + safe.height - 18,
                            tr(STR_MUSIC_PLAYER_MENU_HINT_MODE));

  lastShownIndex_ = index;
  lastProgressSecond_ = status.positionMs / 1000;
  lastLyricIndex_ = tags.lineIndexAt(status.positionMs);
  if (!volumeOverlay_) volumePending_ = false;
}

void MusicPlayerActivity::renderModeMenu() {
  // Hints are drawn below the card on both views, so center in the hint-safe area.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  constexpr int cardW = 420;
  constexpr int kRowX = 16;
  const int rowW = cardW - kRowX * 2;
  const int cx = safe.x + (safe.width - cardW) / 2;

  int cardH = 0;
  int pitch = 40;
  int boxH = 34;
  const char* title = nullptr;
  int rowCount = 0;

  if (menuLevel_ == 0) {
    pitch = 44;
    boxH = 38;
    rowCount = ROOT_MENU_ROWS;
    cardH = 50 + rowCount * pitch + 10;
    title = tr(STR_MUSIC_PLAYER_MENU_TITLE);
  } else {
    rowCount = 4;
    cardH = 50 + rowCount * pitch + 14;  // 224
    title = tr(STR_MUSIC_PLAYER_MODE_TITLE);
  }

  const int cy = safe.y + (safe.height - cardH) / 2;

  // Opaque card (B/W panel cannot dim the background); white fill then border.
  renderer.fillRoundedRect(cx, cy, cardW, cardH, 8, White);
  renderer.drawRoundedRect(cx, cy, cardW, cardH, 2, 8, true);

  renderer.drawText(TITLE_FONT, cx + 20, cy + 14, title, true, EpdFontFamily::BOLD);

  const int rowsTop = cy + 50;
  const int rowX = cx + kRowX;

  for (int i = 0; i < rowCount; ++i) {
    const int ry = rowsTop + i * pitch;
    const bool cursor = i == modeMenuSel_;

    if (cursor) {
      renderer.fillRoundedRect(rowX, ry, rowW, boxH, 6, Black);
    }

    const int textY = ry + (boxH - renderer.getTextHeight(LIST_FONT)) / 2;
    const bool whiteText = cursor;

    if (menuLevel_ == 0) {
      const char* label = nullptr;
      if (i == 0) label = tr(STR_MUSIC_PLAYER_MODE_TITLE);
      else if (i == 1) label = tr(STR_MUSIC_PLAYER_TRACK_LIST);
      else if (i == 2) label = tr(STR_MUSIC_PLAYER_RESCAN);
      else label = tr(STR_MUSIC_PLAYER_MY_PLAYLISTS);
      renderer.drawText(LIST_FONT, rowX + 12, textY, label, !whiteText);

      // Every row carries a chevron; row 0 also shows the active mode name.
      {
        const int chevSize = MusicTransport::chevron_size;  // 28
        const int chevX = rowX + rowW - chevSize - 8;
        const int chevY = ry + (boxH - chevSize) / 2;
        blitGlyph(renderer, MusicTransport::chevron, chevSize, chevX, chevY, !whiteText);

        if (i == 0) {
          const char* mode = modeName(MusicEngine::getInstance().playMode());
          const int modeW = renderer.getTextWidth(SMALL_FONT, mode);
          renderer.drawText(SMALL_FONT, chevX - 8 - modeW,
                            ry + (boxH - renderer.getTextHeight(SMALL_FONT)) / 2, mode,
                            !whiteText);
        }
      }
    } else {
      renderer.drawText(LIST_FONT, rowX + 12, textY,
                        modeName(static_cast<MusicEngine::PlayMode>(i)), !cursor,
                        EpdFontFamily::REGULAR);

      // Check on the active mode (independent of the cursor highlight).
      if (i == static_cast<int>(MusicEngine::getInstance().playMode())) {
        const int cs = MusicTransport::check_size;  // 28
        blitGlyph(renderer, MusicTransport::check, cs, rowX + rowW - cs - 8,
                  ry + (boxH - cs) / 2, cursor ? false : true);
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MusicPlayerActivity::renderLoginMethod() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  constexpr int cardW = 420;
  constexpr int kRowX = 16;
  const int rowW = cardW - kRowX * 2;
  const int cx = safe.x + (safe.width - cardW) / 2;

  constexpr int rowCount = 2;
  constexpr int pitch = 56;
  constexpr int boxH = 48;
  constexpr int cardH = 50 + rowCount * pitch + 14;
  const int cy = safe.y + (safe.height - cardH) / 2;

  // Opaque white card with a black border.
  renderer.fillRoundedRect(cx, cy, cardW, cardH, 8, White);
  renderer.drawRoundedRect(cx, cy, cardW, cardH, 2, 8, true);
  renderer.drawText(TITLE_FONT, cx + 20, cy + 14,
                    tr(STR_MUSIC_PLAYER_LOGIN_CHOICE), true, EpdFontFamily::BOLD);

  const int rowsTop = cy + 50;
  const int rowX = cx + kRowX;
  const char* rowLabels[rowCount] = {
      tr(STR_MUSIC_PLAYER_LOGIN_SMS),
      tr(STR_MUSIC_PLAYER_LOGIN_QR),
  };
  for (int i = 0; i < rowCount; ++i) {
    const int ry = rowsTop + i * pitch;
    const bool cursor = i == loginMethodSel_;
    if (cursor) renderer.fillRoundedRect(rowX, ry, rowW, boxH, 6, Black);
    const int textY = ry + (boxH - renderer.getTextHeight(LIST_FONT)) / 2;
    renderer.drawText(LIST_FONT, rowX + 12, textY, rowLabels[i], !cursor);
  }

  // Optional error line just under the card.
  if (!loginMessage_.empty()) {
    const int msgW = renderer.getTextWidth(LIST_FONT, loginMessage_.c_str());
    renderer.drawText(LIST_FONT, safe.x + (safe.width - msgW) / 2,
                      cy + cardH + 20, loginMessage_.c_str(), true);
  }

  const auto maps = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
  GUI.drawButtonHints(renderer, maps.btn1, maps.btn2, maps.btn3, maps.btn4);
}

// ---------------------------------------------------------------------------
// Account binding & playlist browsing
// ---------------------------------------------------------------------------

void MusicPlayerActivity::startBinding() {
  if (WiFi.status() != WL_CONNECTED) {
    auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true);
    if (!wifi) {
      LOG_ERR("OnlineMusic", "OOM: WifiSelectionActivity");
      return;
    }
    startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
      if (result.isCancelled || WiFi.status() != WL_CONNECTED) return;
      enterLoginMethod();
    });
    return;
  }
  enterLoginMethod();
}

void MusicPlayerActivity::enterLoginMethod() {
  loginMethodSel_ = 0;
  loginMessage_.clear();
  view_ = View::LoginMethod;
  firstShow_ = true;
  requestUpdate();
}

void MusicPlayerActivity::loopLoginMethod() {
  using B = MappedInputManager::Button;

  if (mappedInput.wasReleased(B::Back)) {
    view_ = View::NowPlaying;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  // Only two rows: wheel Up/Down (also BOOT-chord Left/Right) toggles.
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Left)) {
    loginMethodSel_ = (loginMethodSel_ + 1) % 2;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Right)) {
    loginMethodSel_ = (loginMethodSel_ + 1) % 2;
    requestUpdate();
  }

  if (mappedInput.wasReleased(B::Confirm)) {
    loginMessage_.clear();
    if (loginMethodSel_ == 0) {
      launchPhoneKeyboard();  // SMS-code login
    } else {
      bindState_ = BindState::Starting;
      view_ = View::QrBind;
      firstShow_ = true;
      requestUpdate();
    }
  }
}

void MusicPlayerActivity::launchPhoneKeyboard() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const std::string phone = std::get<KeyboardResult>(result.data).text;

        // Validate 11 digits starting with 1.
        bool valid = phone.size() == 11 && phone[0] == '1';
        for (char c : phone) {
          if (c < '0' || c > '9') valid = false;
        }
        if (!valid) {
          loginMessage_ = tr(STR_MUSIC_PLAYER_PHONE_BAD);
          view_ = View::LoginMethod;
          firstShow_ = true;
          requestUpdate();
          return;
        }

        smsPhone_ = phone;
        // Remember the number (prefilled next time) even if the flow is aborted.
        APP_STATE.musicPhone = phone;
        APP_STATE.saveToFile();

        // No-SMS encrypted-POST self-test: verifies the weapi channel returns
        // a body before we spend a text. Runs before the SMS send by design.
        const bool channelOk = OnlineMusicApi::probeWeapiChannel();
        LOG_INF("MP", "weapi channel probe -> %s", channelOk ? "OK" : "STALL");

        // Blocking send; usually returns within a second or two.
        if (OnlineMusicApi::sendSmsCode(phone)) {
          launchCaptchaKeyboard();
        } else {
          loginMessage_ = tr(STR_MUSIC_PLAYER_SMS_SEND_FAIL);
          view_ = View::LoginMethod;
          firstShow_ = true;
          requestUpdate();
        }
      },
      // Prefill the remembered number; Confirm keeps it, wheel edits if needed.
      tr(STR_MUSIC_PLAYER_PHONE_TITLE), APP_STATE.musicPhone, 11, InputType::Text);
}

void MusicPlayerActivity::launchCaptchaKeyboard() {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const std::string captcha = std::get<KeyboardResult>(result.data).text;

        std::string cookie, csrf;
        if (OnlineMusicApi::loginWithSmsCode(smsPhone_, captcha, cookie, csrf)) {
          onBindConfirmed(cookie, csrf);
        } else {
          loginMessage_ = tr(STR_MUSIC_PLAYER_CAPTCHA_WRONG);
          view_ = View::LoginMethod;
          firstShow_ = true;
          requestUpdate();
        }
      },
      tr(STR_MUSIC_PLAYER_CAPTCHA_TITLE), "", 6, InputType::Text);
}

void MusicPlayerActivity::onBindConfirmed(const std::string& cookie, const std::string& csrf) {
  LOG_INF("MP", "bind confirmed, cookie %u bytes", static_cast<unsigned>(cookie.size()));
  OnlineMusicApi::setSession(cookie, csrf);

  // Persist the session token BEFORE any further network/render work, so a
  // failure or abort while fetching the profile or playlists can never drop the
  // login. Re-entering a phone number is exactly the friction we are removing.
  APP_STATE.musicCookie = cookie;
  APP_STATE.musicCsrf = csrf;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("MP", "session cookie save FAILED");
  } else {
    LOG_INF("MP", "session cookie saved (%u bytes)",
            static_cast<unsigned>(APP_STATE.musicCookie.size()));
  }

  // Account/profile and playlists are fetched by the staged loader (which also
  // paints its progress), not here where they would block without feedback.
  loadPlaylists();
}

void MusicPlayerActivity::loadPlaylists() {
  // The hotspot link tuning (no modem sleep + lock to 11b/g) is applied by the
  // staged loader's first step, prepareHotspotLink(), which paints its status
  // and explicitly reconnects after the live protocol change. Doing that here
  // silently dropped the association right after login and nothing recovered.
  view_ = View::Playlists;
  browseLoading_ = true;
  browseMessage_.clear();
  browseStage_.clear();
  browsePhase_ = 0;
  browseUid_ = 0;
  selected_ =0;
  viewTop_ = 0;
  firstShow_ = true;
  requestUpdate();
}

bool MusicPlayerActivity::prepareHotspotLink() {
  if (linkPrepared_) {
    // Tuning already applied once this run; just confirm we are associated.
    return WiFi.status() == WL_CONNECTED;
  }
  // Online music streams continuously, so keep the radio awake for the whole
  // session. With modem sleep on, playlist/track transfers on this hotspot
  // stall mid-body forever (only a few KB in), the same failure OPDS downloads
  // already guard against in HttpDownloader.
  WiFi.setSleep(false);
#if !defined(SIMULATOR)
  // Phone hotspots negotiate 802.11n A-MPDU block-ack sessions with the ESP32
  // that stall RX after a few KB (esp-idf #7204 and many duplicates). 11b/g
  // has no A-MPDU; throughput loss is irrelevant for a few-KB JSON API.
  //
  // Changing the protocol on an already-associated STA drops the association,
  // so reconnect explicitly and wait for it to come back before returning.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
  WiFi.reconnect();
  const uint32_t start = millis();
  while (millis() - start < 15000) {
    if (WiFi.status() == WL_CONNECTED) {
      linkPrepared_ = true;
      return true;
    }
    delay(100);
  }
  return false;
#else
  linkPrepared_ = true;
  return true;
#endif
}

void MusicPlayerActivity::unbindAccount() {
  OnlineMusicApi::clearSession();
  APP_STATE.musicCookie.clear();
  APP_STATE.musicCsrf.clear();
  APP_STATE.musicUserId = 0;
  APP_STATE.musicNickname.clear();
  // Forget the last-played selection too.
  APP_STATE.musicLastPlaylistId = 0;
  APP_STATE.musicLastPlaylistName.clear();
  APP_STATE.musicLastTrackId = 0;
  APP_STATE.saveToFile();

  view_ = View::NowPlaying;
  firstShow_ = true;
  requestUpdate();
}

void MusicPlayerActivity::loopQrBind(const uint32_t now) {
  using B = MappedInputManager::Button;

  if (mappedInput.wasReleased(B::Back)) {
    view_ = View::NowPlaying;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  if (bindState_ == BindState::Expired || bindState_ == BindState::Failed) {
    if (mappedInput.wasReleased(B::Confirm)) {
      bindState_ = BindState::Starting;
      firstShow_ = true;
      requestUpdate();
    }
    return;
  }

  if (bindState_ == BindState::Starting) {
    OnlineMusicApi::QrLoginStart start;
    BindState next = BindState::Failed;
    if (OnlineMusicApi::startQrLogin(start)) {
      qrKey_ = start.unikey;
      if (qrEncodeLow(start.qrUrl, qrMatrix_)) {
        next = BindState::Waiting;
        qrLastPollMs_ = now;
      }
    }
    bindState_ = next;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  if (now - qrLastPollMs_ < QR_POLL_INTERVAL_MS) return;
  qrLastPollMs_ = now;

  std::string cookie, csrf;
  const OnlineMusicApi::QrPollCode code = OnlineMusicApi::pollQrLogin(qrKey_, cookie, csrf);
  switch (code) {
    case OnlineMusicApi::QrPollCode::Waiting:
      return;  // no visual change
    case OnlineMusicApi::QrPollCode::Scanned:
      bindState_ = BindState::Scanned;
      break;
    case OnlineMusicApi::QrPollCode::Confirmed:
      onBindConfirmed(cookie, csrf);
      return;
    case OnlineMusicApi::QrPollCode::Expired:
      bindState_ = BindState::Expired;
      break;
    case OnlineMusicApi::QrPollCode::Error:
      bindState_ = BindState::Failed;
      break;
  }
  firstShow_ = true;
  requestUpdate();
}

void MusicPlayerActivity::loopPlaylists(const uint32_t now) {
  (void)now;
  using B = MappedInputManager::Button;

  if (browseLoading_) {
    // Staged across loop ticks: announce each stage (so the e-paper shows what
    // the device is doing) and only then run the blocking network call.
    // Connectivity itself is established in phase 1 (prepareHotspotLink), so a
    // not-yet-ready stack is recovered there rather than failed immediately.
    auto failLoad = [this]() {
      browseLoading_ = false;
      browseMessage_ = tr(STR_MUSIC_PLAYER_QR_FAILED);
      browseStage_.clear();
      browsePhase_ = 0;
      firstShow_ = true;
      requestUpdate();
    };

    if (browsePhase_ == 0) {
      playlists_.clear();
      browseUid_ = 0;
      browseStage_ = tr(STR_MUSIC_PLAYER_STAGE_LINK);
      browsePhase_ = 1;
      firstShow_ = true;
      requestUpdate();
      return;  // paint the stage before blocking
    }

    if (browsePhase_ == 1) {
      // Apply no-sleep + 11b/g and wait for the association to come back.
      if (!prepareHotspotLink()) {
        // No link: browse the cached playlist list when this device was bound
        // before, so the user can still open playlists fully offline.
        const uint64_t uid = APP_STATE.musicUserId;
        if (uid != 0 &&
            MusicCache::readUserPlaylists(uid, playlists_, false) &&
            !playlists_.empty()) {
          browseUid_ = uid;
          browseLoading_ = false;
          browseMessage_ = "";
          browseStage_.clear();
          browsePhase_ = 0;
          selected_ = 0;
          viewTop_ = 0;
          firstShow_ = true;
          requestUpdate();
          return;
        }
        failLoad();
        return;
      }
      browseUid_ = APP_STATE.musicUserId;
      if (browseUid_ != 0) {
        browseStage_ = tr(STR_MUSIC_PLAYER_STAGE_PLAYLISTS);
        browsePhase_ = 3;
      } else {
        browseStage_ = tr(STR_MUSIC_PLAYER_STAGE_ACCOUNT);
        browsePhase_ = 2;
      }
      firstShow_ = true;
      requestUpdate();
      return;  // paint before the next blocking call
    }

    if (browsePhase_ == 2) {
      OnlineMusicApi::AccountInfo account;
      if (OnlineMusicApi::getAccount(account)) {
        browseUid_ = account.userId;
        APP_STATE.musicUserId = browseUid_;
        APP_STATE.musicNickname = account.nickname;
        APP_STATE.saveToFile();
      }
      if (browseUid_ == 0) {
        failLoad();
        return;
      }
      browseStage_ = tr(STR_MUSIC_PLAYER_STAGE_PLAYLISTS);
      browsePhase_ = 3;
      firstShow_ = true;
      requestUpdate();
      return;  // paint before loading playlists
    }

    // browsePhase_ == 3
    const bool ok =
        browseUid_ != 0 && OnlineMusicApi::userPlaylists(browseUid_, playlists_);
    browseLoading_ = false;
    browseMessage_ = ok ? "" : tr(STR_MUSIC_PLAYER_QR_FAILED);
    browseStage_.clear();
    browsePhase_ = 0;
    selected_ = 0;
    viewTop_ = 0;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(B::Back)) {
    view_ = View::NowPlaying;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  const int rowHeight = renderer.getLineHeight(LIST_FONT) + renderer.getLineHeight(SMALL_FONT) + 12;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = safe.y + safe.height - listTop;
  const int visibleRows = std::max(1, listHeight / rowHeight);

  // Last row is the logout action.
  const int count = static_cast<int>(playlists_.size()) + 1;

  if (mappedInput.wasReleased(B::Up)) {
    if (selected_ > 0) --selected_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Down)) {
    if (selected_ + 1 < count) ++selected_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Left)) {
    viewTop_ = std::max(0, viewTop_ - visibleRows);
    selected_ = viewTop_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Right)) {
    viewTop_ = std::clamp(viewTop_ + visibleRows, 0, std::max(0, count - visibleRows));
    selected_ = viewTop_;
    requestUpdate();
  }

  if (mappedInput.wasReleased(B::Confirm)) {
    if (selected_ < static_cast<int>(playlists_.size())) {
      browsePlaylistId_ = playlists_[selected_].id;
      browseMessage_ = playlists_[selected_].name;
      browseTracks_.clear();
      view_ = View::PlaylistTracks;
      browseLoading_ = true;
      selected_ = 0;
      viewTop_ = 0;
      firstShow_ = true;
      // Remember this as the last-opened playlist.
      APP_STATE.musicLastPlaylistId = browsePlaylistId_;
      APP_STATE.musicLastPlaylistName = browseMessage_;
      APP_STATE.saveToFile();
      requestUpdate();
    } else {
      unbindAccount();
    }
  }

  selected_ = std::clamp(selected_, 0, count - 1);
  const int maxTop = std::max(0, count - visibleRows);
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
  if (selected_ < viewTop_) viewTop_ = selected_;
  if (selected_ >= viewTop_ + visibleRows) viewTop_ = selected_ - visibleRows + 1;
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
}

void MusicPlayerActivity::loopPlaylistTracks(const uint32_t now) {
  (void)now;
  using B = MappedInputManager::Button;

  if (browseLoading_) {
    if (restorePending_) {
      auto& engine = MusicEngine::getInstance();

      // Load the queue (positioned at the remembered track) and switch to the
      // paused now-playing view. Returns false leaving tracks untouched.
      auto finishRestore = [&](std::vector<OnlineTrack>& tracks) -> bool {
        if (tracks.empty()) return false;
        // Position at the remembered track, falling back to the first.
        size_t idx = 0;
        const uint32_t wanted = APP_STATE.musicLastTrackId;
        if (wanted != 0) {
          auto it = std::find_if(tracks.begin(), tracks.end(),
                                 [wanted](const OnlineTrack& t) { return t.id == wanted; });
          if (it != tracks.end()) idx = static_cast<size_t>(it - tracks.begin());
        }
        if (!engine.loadOnline(std::move(tracks), idx)) return false;
        browseLoading_ = false;
        restorePending_ = false;
        restoredPaused_ = true;
        selected_ = static_cast<int>(idx);
        view_ = View::NowPlaying;
        viewTop_ = 0;
        firstShow_ = true;
        lastRenderMs_ = 0;
        lastProgressSecond_ = UINT32_MAX;
        lastShownIndex_ = SIZE_MAX;
        lastLyricIndex_ = -2;
        lastPersistedIndex_ = idx;
        requestUpdate();
        return true;
      };

      // Offline boot: never reach DNS before the STA netif/tcpip is up (NULL
      // tcpip lock crash). Restore straight from the on-SD cache once, then
      // keep waiting for WiFi in case the playlist is not cached yet.
      if (WiFi.status() != WL_CONNECTED) {
        if (!restoreOfflineTried_) {
          restoreOfflineTried_ = true;
          std::vector<OnlineTrack> cached;
          if (MusicCache::readPlaylist(browsePlaylistId_, cached, false) &&
              finishRestore(cached)) {
            return;
          }
        }
        return;  // no repaint: this does not refresh-storm
      }
      WiFi.setSleep(false);  // modem-sleep stalls streaming transfers

      // playlistTracks() itself falls back to the stale cached list offline.
      browseTracks_.clear();
      if (OnlineMusicApi::playlistTracks(browsePlaylistId_, browseTracks_) &&
          finishRestore(browseTracks_)) {
        return;
      }

      // Fetch failed (often the large-body stall): retry a few times, then fall
      // back to the default local landing so the player never gets stuck here.
      if (++restoreAttempts_ < 4) {
        delay(300);
        return;
      }
      restorePending_ = false;
      restoredPaused_ = false;
      browseLoading_ = false;
      const size_t count = engine.rescan();
      selected_ = count > 0 ? 0 : -1;
      view_ = View::NowPlaying;
      viewTop_ = 0;
      firstShow_ = true;
      lastRenderMs_ = 0;
      requestUpdate();
      return;
    }

    browseTracks_.clear();
    const bool ok = OnlineMusicApi::playlistTracks(browsePlaylistId_, browseTracks_);
    browseLoading_ = false;
    if (!ok) browseMessage_ = tr(STR_MUSIC_PLAYER_QR_FAILED);
    selected_ = 0;
    viewTop_ = 0;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(B::Back)) {
    // Return to the playlist list with the opened playlist highlighted.
    auto it = std::find_if(playlists_.begin(), playlists_.end(),
                           [this](const OnlineMusicApi::PlaylistInfo& p) { return p.id == browsePlaylistId_; });
    selected_ = static_cast<int>(it - playlists_.begin());
    viewTop_ = 0;
    view_ = View::Playlists;
    firstShow_ = true;
    requestUpdate();
    return;
  }

  auto& engine = MusicEngine::getInstance();
  const int rowHeight = renderer.getLineHeight(LIST_FONT) + renderer.getLineHeight(SMALL_FONT) + 16;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = safe.y + safe.height - listTop;
  const int visibleRows = std::max(1, listHeight / rowHeight);
  const int count = static_cast<int>(browseTracks_.size());

  if (mappedInput.wasReleased(B::Up)) {
    if (selected_ > 0) --selected_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Down)) {
    if (selected_ + 1 < count) ++selected_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Left)) {
    viewTop_ = std::max(0, viewTop_ - visibleRows);
    selected_ = viewTop_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Right)) {
    viewTop_ = std::clamp(viewTop_ + visibleRows, 0, std::max(0, count - visibleRows));
    selected_ = viewTop_;
    requestUpdate();
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    // playOnline resolves the track URL, opens the HTTP stream, fetches cover
    // and lyrics and analyses the MP3 header all synchronously on this task,
    // which blocks repainting for several seconds. Show a popup first so the
    // user sees it is loading rather than frozen; drawPopup pushes to the
    // panel immediately. On failure show why and then restore the list.
    GUI.drawPopup(renderer, tr(STR_MUSIC_PLAYER_OPENING));
    if (engine.playOnline(browseTracks_, static_cast<size_t>(selected_))) {
      // Remember the track now playing in this playlist.
      APP_STATE.musicLastTrackId = browseTracks_[selected_].id;
      APP_STATE.saveToFile();
      lastPersistedIndex_ = static_cast<size_t>(selected_);
      view_ = View::NowPlaying;
      firstShow_ = true;
      lastRenderMs_ = 0;
      lastProgressSecond_ = UINT32_MAX;
      lastShownIndex_ = SIZE_MAX;
      lastLyricIndex_ = -2;
      requestUpdate();
    } else {
      GUI.drawPopup(renderer, tr(STR_MUSIC_PLAYER_PLAY_FAILED));
      delay(1600);
      requestUpdate();
    }
  }

  selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
  const int maxTop = std::max(0, count - visibleRows);
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
  if (selected_ < viewTop_) viewTop_ = selected_;
  if (selected_ >= viewTop_ + visibleRows) viewTop_ = selected_ - visibleRows + 1;
  viewTop_ = std::clamp(viewTop_, 0, maxTop);
}

// ---------------------------------------------------------------------------
// Binding / playlist rendering
// ---------------------------------------------------------------------------

void MusicPlayerActivity::renderQrBind() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer,
                 Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_MUSIC_PLAYER_QR_BIND));

  // Pick the largest integer scale that fits the QR (with quiet zone) across.
  const int modules = qrMatrix_.size;
  int scale = 1;
  while ((modules + 8) * (scale + 1) <= safe.width - 20) ++scale;
  const int qrPx = (modules + 8) * scale;
  const int qrX = safe.x + (safe.width - qrPx) / 2;
  const int qrY = safe.y + metrics.topPadding + metrics.headerHeight +
                  metrics.verticalSpacing + 16;

  if (bindState_ != BindState::Starting && modules > 0) {
    for (int my = 0; my < modules; ++my) {
      for (int mx = 0; mx < modules; ++mx) {
        if (!qrMatrix_.get(mx, my)) continue;
        renderer.fillRect(qrX + (mx + 4) * scale, qrY + (my + 4) * scale, scale, scale, Black);
      }
    }
  }

  const char* status = tr(STR_MUSIC_PLAYER_PLAYLIST_LOADING);
  switch (bindState_) {
    case BindState::Waiting:
      status = tr(STR_MUSIC_PLAYER_QR_SCAN_HINT);
      break;
    case BindState::Scanned:
      status = tr(STR_MUSIC_PLAYER_QR_SCANNED);
      break;
    case BindState::Expired:
      status = tr(STR_MUSIC_PLAYER_QR_EXPIRED);
      break;
    case BindState::Failed:
      status = tr(STR_MUSIC_PLAYER_QR_FAILED);
      break;
    case BindState::Starting:
      break;
  }
  renderer.drawCenteredText(LIST_FONT, qrY + qrPx + 24, status, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MusicPlayerActivity::renderPlaylists() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer,
                 Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_MUSIC_PLAYER_MY_PLAYLISTS));

  const int listX = safe.x + metrics.contentSidePadding;
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listWidth = safe.width - metrics.contentSidePadding * 2;
  const int listHeight = safe.y + safe.height - listTop;

  if (browseLoading_) {
    const char* stage = browseStage_.empty()
                            ? tr(STR_MUSIC_PLAYER_PLAYLIST_LOADING)
                            : browseStage_.c_str();
    renderer.drawCenteredText(LIST_FONT, listTop + listHeight / 2, stage);
    return;
  }
  if (playlists_.empty()) {
    renderer.drawCenteredText(LIST_FONT, listTop + listHeight / 2, browseMessage_.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int smallH = renderer.getLineHeight(SMALL_FONT);
  const int rowHeight = renderer.getLineHeight(LIST_FONT) + smallH + 12;
  const int visibleRows = std::max(1, listHeight / rowHeight);

  const int playlistCount = static_cast<int>(playlists_.size());
  for (int row = 0; row < visibleRows; ++row) {
    const int index = viewTop_ + row;
    if (index < 0 || index > playlistCount) break;

    const int y = listTop + row * rowHeight;
    const bool selected = index == selected_;
    if (selected) {
      renderer.fillRoundedRect(listX, y + 3, listWidth, rowHeight - 6, 6, Black);
    }

    const int textY = y + 6;
    if (index < playlistCount) {
      const OnlineMusicApi::PlaylistInfo& pl = playlists_[index];

      const std::string name =
          renderer.truncatedText(LIST_FONT, pl.name.c_str(), listWidth - 16);
      renderer.drawText(LIST_FONT, listX + 8, textY, name.c_str(), !selected,
                        EpdFontFamily::BOLD);

      char sub[24];
      snprintf(sub, sizeof(sub), "%u %s", pl.trackCount, tr(STR_MUSIC_PLAYER_TRACK_COUNT));
      renderer.drawText(SMALL_FONT, listX + 8,
                        textY + renderer.getLineHeight(LIST_FONT) + 2, sub, !selected);
    } else {
      renderer.drawText(LIST_FONT, listX + 8, textY + 4,
                        tr(STR_MUSIC_PLAYER_LOGOUT), !selected);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MusicPlayerActivity::renderPlaylistTracks() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer,
                 Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 browseMessage_.c_str());

  const int listX = safe.x + metrics.contentSidePadding;
  const int listTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listWidth = safe.width - metrics.contentSidePadding * 2;
  const int listHeight = safe.y + safe.height - listTop;

  if (browseLoading_) {
    renderer.drawCenteredText(LIST_FONT, listTop + listHeight / 2,
                              tr(STR_MUSIC_PLAYER_PLAYLIST_LOADING));
    return;
  }
  if (browseTracks_.empty()) {
    renderer.drawCenteredText(LIST_FONT, listTop + listHeight / 2, browseMessage_.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int rowHeight = renderer.getLineHeight(LIST_FONT) + renderer.getLineHeight(SMALL_FONT) + 16;
  const int visibleRows = std::max(1, listHeight / rowHeight);

  for (int row = 0; row < visibleRows; ++row) {
    const int index = viewTop_ + row;
    if (index < 0 || index >= static_cast<int>(browseTracks_.size())) break;

    const OnlineTrack& item = browseTracks_[index];
    const int y = listTop + row * rowHeight;
    const bool selected = index == selected_;
    if (selected) {
      renderer.fillRoundedRect(listX, y + 3, listWidth, rowHeight - 6, 6, Black);
    }

    const std::string title = renderer.truncatedText(LIST_FONT, item.title.c_str(), listWidth - 16);
    renderer.drawText(LIST_FONT, listX + 8, y + 6, title.c_str(), !selected, EpdFontFamily::BOLD);

    std::string sub = item.artist;
    if (!item.album.empty()) {
      sub += " · ";
      sub += item.album;
    }
    const std::string subShown = renderer.truncatedText(SMALL_FONT, sub.c_str(), listWidth - 16);
    renderer.drawText(SMALL_FONT, listX + 8, y + 6 + renderer.getLineHeight(LIST_FONT) + 4,
                      subShown.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_MUSIC_PLAYER_PLAY), nullptr,
                                            nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
