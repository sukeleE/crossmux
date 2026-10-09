#pragma once

// Metadata for the currently playing track.
//
// Sources:
//  - ID3v2.2/2.3/2.4 tags in the MP3 file: TIT2/TPE1/TALB text frames, the
//    APIC embedded cover, and the USLT unsynced lyrics.
//  - A sibling .lrc file (same base name as the audio) for time-synced
//    lyrics. Without timestamps the lyrics are shown statically, like USLT.
//
// ID3 text is normalized to UTF-8 (Latin-1 / UTF-16 encodings converted).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class HalFile;

struct LrcLine {
  uint32_t timeMs = 0;
  std::string text;
};

struct TrackTags {
  std::string title;
  std::string artist;
  std::string album;

  // Synced lines from a .lrc file, sorted by time. Empty when no timed LRC.
  std::vector<LrcLine> synced;
  // Unsynced lyrics: USLT content, or an LRC without timestamps.
  std::string unsyncedLyrics;

  bool hasLyrics() const { return !synced.empty() || !unsyncedLyrics.empty(); }

  // Index of the synced line that should be highlighted at positionMs
  // (last line whose time is <= position). -1 when nothing applies yet.
  int lineIndexAt(uint32_t positionMs) const;
};

// Embedded cover extracted from an ID3v2 APIC frame. The pointer aliases the
// caller-owned tagBuffer passed to loadId3 and is valid while that lives.
struct EmbeddedArt {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool isPng = false;
};

namespace TrackTagIO {

// Parses the ID3v2 tag at the start of audioFile. The raw tag bytes are
// stored in tagBuffer (caller frees it after consuming the art); art is set
// only when an APIC picture exists. Missing tags leave everything default.
bool loadId3(HalFile& audioFile, TrackTags& tags, std::vector<uint8_t>& tagBuffer, EmbeddedArt& art);

// Loads and parses <audioPath without extension>.lrc if it exists.
void loadSidecarLrc(const std::string& audioPath, TrackTags& tags);

}  // namespace TrackTagIO
