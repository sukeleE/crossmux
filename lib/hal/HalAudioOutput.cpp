#include "HalAudioOutput.h"

#if CROSSPOINT_CAP_SOUND_FEEDBACK

#include <AudioManager.h>
#include <BoardConfig.h>
#include <Logging.h>

#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
#include "Waveshare397Power.h"
#elif FREEINK_DEVICE_WAVESHARE_EPAPER_397_V2
#include "Waveshare397V2Power.h"
#endif

namespace HalAudioOutput {
namespace {

constexpr unsigned long AUDIO_RAIL_SETTLE_MS = 10;

AudioManager audio;
bool initialized = false;

bool setBoardPower(const bool enabled) {
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
  return Waveshare397Power::setAudioPower(enabled);
#elif FREEINK_DEVICE_WAVESHARE_EPAPER_397_V2
  return Waveshare397V2Power::setAudioPower(enabled);
#else
  (void)enabled;
  return true;
#endif
}

bool begin() {
  if (initialized) return true;
  if (!setBoardPower(true)) {
    LOG_ERR("SND", "Failed to enable board audio power");
    return false;
  }
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397 || FREEINK_DEVICE_WAVESHARE_EPAPER_397_V2
  delay(AUDIO_RAIL_SETTLE_MS);
#endif
  if (!audio.begin()) {
    LOG_ERR("SND", "Failed to initialize audio output");
    setBoardPower(false);
    return false;
  }
  initialized = true;
  return true;
}

}  // namespace

bool playPcm(const ReadCallback read, const SeekCallback seek, const uint32_t sampleRate, const uint8_t channels,
             const uint8_t volume) {
  if (!read || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::PcmSource source = {read, seek};
  if (audio.playPcm(source, sampleRate, channels)) return true;
  LOG_ERR("SND", "Failed to play feedback PCM");
  shutdown();
  return false;
}

bool playWav(const ReadCallback read, const SeekCallback seek, const uint8_t volume, const bool loop) {
  if (!read || !seek || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::WavSource source = {read, seek};
  if (audio.play(source, loop)) return true;
  LOG_ERR("SND", "Failed to play WAV");
  shutdown();
  return false;
}

bool playMp3(const ReadCallback read, const SeekCallback seek, const SizeCallback size,
             const uint8_t volume) {
  if (!read || !seek || !size || !begin()) return false;
  audio.setVolume(volume);
  AudioManager::Mp3Source source = {read, seek, size};
  if (audio.playMp3(source)) return true;
  LOG_ERR("SND", "Failed to play MP3");
  shutdown();
  return false;
}

bool pausePlayback() { return initialized && audio.pause(); }
bool resumePlayback() { return initialized && audio.resume(); }

bool getStatus(Status& out) {
  if (!initialized) return false;
  AudioManager::PlaybackStatus status;
  if (!audio.getPlaybackStatus(status)) return false;
  out.active = status.active;
  out.paused = status.paused;
  out.positionMs = status.positionMs;
  out.durationMs = status.durationMs;
  return true;
}

bool takeNaturalEnd() { return initialized && audio.takeNaturalEnd(); }

void stopPlayback() {
  if (initialized) audio.stop();
}

bool playbackRunning() {
  return initialized && audio.isPlaying();
}

bool restart(const uint8_t volume) {
  if (!initialized) return false;
  audio.setVolume(volume);
  return audio.requestRestart();
}

void setVolume(const uint8_t volume) {
  if (initialized) audio.setVolume(volume);
}

void shutdown() {
  if (!initialized) return;
  audio.powerDown();
  if (!setBoardPower(false)) LOG_ERR("SND", "Failed to disable board audio power");
  initialized = false;
}

}  // namespace HalAudioOutput

#endif
