#pragma once

#include <cstddef>
#include <cstdint>

namespace HalAudioOutput {

using ReadCallback = int (*)(uint8_t* dst, size_t length);
using SeekCallback = bool (*)(size_t position);
using SizeCallback = uint64_t (*)();

bool playPcm(ReadCallback read, SeekCallback seek, uint32_t sampleRate, uint8_t channels, uint8_t volume);

// MP3 stream (MPEG-1/2/2.5 layer III). seek is mandatory (header analysis)
// and size is used for the bitrate-based duration estimate.
bool playMp3(ReadCallback read, SeekCallback seek, SizeCallback size, uint8_t volume);

// Transport controls; pause mutes the DAC and drops the amp, resume continues
// from the same position. No-op (false) when nothing is playing.
bool pausePlayback();
bool resumePlayback();

struct Status {
  bool active = false;
  bool paused = false;
  uint32_t positionMs = 0;
  uint32_t durationMs = 0;  // 0 when unknown
};
bool getStatus(Status& out);

// One-shot natural-end flag (see AudioManager::takeNaturalEnd).
bool takeNaturalEnd();

// Plays a complete WAV file (16-bit PCM, 1-2 channels, 8-48 kHz; anything else
// is rejected). Both callbacks are required: the player parses the WAV header
// via seek before streaming. Returns immediately; playback runs in the audio
// task. loop=true repeats until stopPlayback(). The source behind the callbacks
// (e.g. an open SD file) must stay valid until playback ends or is stopped.
bool playWav(ReadCallback read, SeekCallback seek, uint8_t volume, bool loop = false);
void stopPlayback();
// True between a successful start and natural EOF / stopPlayback().
bool playbackRunning();

bool restart(uint8_t volume);
void setVolume(uint8_t volume);
void shutdown();

}  // namespace HalAudioOutput
