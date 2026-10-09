#include "TrackTags.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

// --- ID3 text decoding ------------------------------------------------------

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back((char)cp);
  } else if (cp < 0x800) {
    out.push_back((char)(0xC0 | (cp >> 6)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char)(0xE0 | (cp >> 12)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (cp >> 18)));
    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

uint32_t decodeUtf16Unit(const uint8_t* p, bool bigEndian) {
  return bigEndian ? ((uint32_t)p[0] << 8) | p[1] : ((uint32_t)p[1] << 8) | p[0];
}

// Decodes an ID3 text field (enc: 0 Latin-1, 1 UTF-16 w/ BOM, 2 UTF-16BE,
// 3 UTF-8). terminates early at the field's own null terminator.
std::string decodeId3Text(uint8_t enc, const uint8_t* p, size_t len) {
  std::string out;

  if (enc == 3) {
    while (len > 0 && *p != 0) {
      out.push_back((char)*p);
      ++p;
      --len;
    }
    return out;
  }
  if (enc == 0) {
    while (len > 0 && *p != 0) {
      appendUtf8(out, *p);  // Latin-1 maps 1:1 onto Unicode 0..255
      ++p;
      --len;
    }
    return out;
  }

  // UTF-16 variants.
  bool bigEndian = (enc == 2);
  if (enc == 1 && len >= 2) {
    if (p[0] == 0xFF && p[1] == 0xFE) {
      bigEndian = false;
      p += 2;
      len -= 2;
    } else if (p[0] == 0xFE && p[1] == 0xFF) {
      bigEndian = true;
      p += 2;
      len -= 2;
    }
  }

  while (len >= 2) {
    const uint32_t unit = decodeUtf16Unit(p, bigEndian);
    p += 2;
    len -= 2;
    if (unit == 0) break;
    if (unit >= 0xD800 && unit <= 0xDBFF && len >= 2) {
      const uint32_t low = decodeUtf16Unit(p, bigEndian);
      p += 2;
      len -= 2;
      if (low >= 0xDC00 && low <= 0xDFFF) {
        appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
        continue;
      }
    }
    appendUtf8(out, unit);
  }
  return out;
}

// Offset of the null terminator for a description string inside an APIC/USLT
// frame (double null for UTF-16 fields). Returns the position right after it,
// or end of buffer when absent.
size_t skipDescription(const uint8_t* p, size_t len, uint8_t enc) {
  if (enc == 1 || enc == 2) {
    for (size_t i = 0; i + 1 < len; i += 2) {
      if (p[i] == 0 && p[i + 1] == 0) return i + 2;
    }
    return len;
  }
  for (size_t i = 0; i < len; ++i) {
    if (p[i] == 0) return i + 1;
  }
  return len;
}

// --- LRC --------------------------------------------------------------------

std::string trimCopy(std::string s) {
  auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
  while (!s.empty() && !notSpace((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && !notSpace((unsigned char)s.back())) s.pop_back();
  return s;
}

uint32_t parseLrcTime(const char* p, const char** endOut) {
  uint32_t minute = 0;
  while (isdigit((unsigned char)*p)) minute = minute * 10 + (*p++ - '0');
  if (*p != ':') {
    *endOut = nullptr;
    return 0;
  }
  ++p;
  uint32_t second = 0;
  while (isdigit((unsigned char)*p)) second = second * 10 + (*p++ - '0');
  uint32_t fraction = 0;
  uint32_t fracDigits = 0;
  if (*p == '.') {
    ++p;
    while (isdigit((unsigned char)*p) && fracDigits < 3) {
      fraction = fraction * 10 + (*p++ - '0');
      ++fracDigits;
    }
  }
  while (fracDigits < 3) {
    fraction *= 10;
    ++fracDigits;
  }
  *endOut = p;
  return minute * 60000 + second * 1000 + fraction;
}

}  // namespace

int TrackTags::lineIndexAt(uint32_t positionMs) const {
  if (synced.empty()) return -1;
  // Binary search for the last line with time <= position.
  int lo = 0, hi = (int)synced.size() - 1, ans = -1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (synced[mid].timeMs <= positionMs) {
      ans = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return ans;
}

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------

namespace TrackTagIO {

bool loadId3(HalFile& file, TrackTags& tags, std::vector<uint8_t>& buffer, EmbeddedArt& art) {
  uint8_t header[10];
  file.seekSet(0);  // harmless if already at 0
  if (file.read(header, sizeof(header)) != (int)sizeof(header)) return false;
  if (memcmp(header, "ID3", 3) != 0) return true;  // no tag: not an error

  const uint8_t major = header[3];
  if (major < 2 || major > 4) return true;

  const uint32_t tagSize = ((uint32_t)(header[6] & 0x7f) << 21) |
                           ((uint32_t)(header[7] & 0x7f) << 14) |
                           ((uint32_t)(header[8] & 0x7f) << 7) | (uint32_t)(header[9] & 0x7f);
  // Cap the read: covers with a sane ceiling; larger exotic tags are skipped.
  const uint32_t readSize = std::min<uint32_t>(tagSize, 1500000);

  buffer.resize((size_t)readSize + 10);
  memcpy(buffer.data(), header, 10);
  const int got = file.read(buffer.data() + 10, readSize);
  if (got < 0) return false;
  buffer.resize((size_t)got + 10);

  size_t pos = 10;

  // Skip an extended header if the flag is set.
  if (major == 4 && (header[5] & 0x40) && pos + 4 <= buffer.size()) {
    const uint32_t extSize = ((uint32_t)(buffer[pos] & 0x7f) << 21) |
                             ((uint32_t)(buffer[pos + 1] & 0x7f) << 14) |
                             ((uint32_t)(buffer[pos + 2] & 0x7f) << 7) | (buffer[pos + 3] & 0x7f);
    pos += extSize;
  } else if (major == 3 && (header[5] & 0x40) && pos + 4 <= buffer.size()) {
    const uint32_t extSize = (uint32_t)buffer[pos] << 24 | (uint32_t)buffer[pos + 1] << 16 |
                             (uint32_t)buffer[pos + 2] << 8 | buffer[pos + 3];
    pos += extSize;
  }

  const bool v22 = major == 2;
  while (pos < buffer.size()) {
    size_t idLen = v22 ? 3 : 4;
    if (pos + idLen >= buffer.size()) break;

    // Padding starts at the first null id.
    bool idAllZero = true;
    for (size_t i = 0; i < idLen; ++i) {
      if (buffer[pos + i] != 0) idAllZero = false;
    }
    if (idAllZero) break;

    uint32_t frameSize;
    size_t dataPos;
    if (v22) {
      if (pos + 6 > buffer.size()) break;
      frameSize = (uint32_t)buffer[pos + 3] << 16 | (uint32_t)buffer[pos + 4] << 8 | buffer[pos + 5];
      dataPos = pos + 6;
    } else {
      if (pos + 10 > buffer.size()) break;
      const uint8_t* sp = buffer.data() + pos + 4;
      if (major == 4) {
        frameSize = ((uint32_t)(sp[0] & 0x7f) << 21) | ((uint32_t)(sp[1] & 0x7f) << 14) |
                    ((uint32_t)(sp[2] & 0x7f) << 7) | (sp[3] & 0x7f);
      } else {
        frameSize = (uint32_t)sp[0] << 24 | (uint32_t)sp[1] << 16 | (uint32_t)sp[2] << 8 | sp[3];
        // Some 2.3 tools wrote syncsafe sizes; detect an impossible value and
        // reinterpret.
        if (frameSize > buffer.size() - (pos + 10)) {
          const uint32_t sync = ((uint32_t)(sp[0] & 0x7f) << 21) | ((uint32_t)(sp[1] & 0x7f) << 14) |
                                ((uint32_t)(sp[2] & 0x7f) << 7) | (sp[3] & 0x7f);
          if (sync <= buffer.size()) frameSize = sync;
        }
      }
      dataPos = pos + 10;
    }

    if (dataPos > buffer.size() || frameSize > buffer.size() - dataPos) break;

    const uint8_t* frame = buffer.data() + dataPos;
    const size_t frameLen = frameSize;
    char id[5] = {};
    memcpy(id, buffer.data() + pos, idLen);

    // Text frames (T* except "TXX").
    if (!v22 && id[0] == 'T' && frameLen >= 1) {
      const uint8_t enc = frame[0];
      std::string text = decodeId3Text(enc, frame + 1, frameLen - 1);
      if (strcmp(id, "TIT2") == 0) {
        tags.title = std::move(text);
      } else if (strcmp(id, "TPE1") == 0) {
        tags.artist = std::move(text);
      } else if (strcmp(id, "TALB") == 0) {
        tags.album = std::move(text);
      }
    }

    // USLT unsynced lyrics.
    if (!v22 && strcmp(id, "USLT") == 0 && frameLen >= 4) {
      const uint8_t enc = frame[0];
      // frame[1..3] = language; then the description, then the lyric text.
      const size_t descEnd = skipDescription(frame + 4, frameLen - 4, enc);
      if (4 + descEnd < frameLen) {
        tags.unsyncedLyrics = decodeId3Text(enc, frame + 4 + descEnd, frameLen - 4 - descEnd);
      }
    }

    // APIC embedded picture.
    if ((!v22 && strcmp(id, "APIC") == 0 && frameLen >= 2) ||
        (v22 && strcmp(id, "PIC") == 0 && frameLen >= 2)) {
      const uint8_t enc = frame[0];
      size_t p = 1;
      bool isPng = false;
      if (v22) {
        // v2.2 PIC: 3-byte image format ("JPEG"/"PNG") then picture type.
        if (p + 4 <= frameLen) {
          isPng = memcmp(frame + p, "PNG", 3) == 0;
          p += 3;
        }
      } else {
        // MIME type, latin-1, null terminated.
        size_t mimeEnd = p;
        while (mimeEnd < frameLen && frame[mimeEnd] != 0) ++mimeEnd;
        const std::string mime((const char*)frame + p, mimeEnd - p);
        isPng = mime.find("png") != std::string::npos;
        p = mimeEnd + 1;
      }
      if (p < frameLen) {
        ++p;  // picture type byte
        const size_t descEnd = skipDescription(frame + p, frameLen - p, enc);
        p += descEnd;
        if (p < frameLen && art.data == nullptr) {
          art.data = frame + p;
          art.size = frameLen - p;
          art.isPng = isPng;
        }
      }
    }

    pos = dataPos + frameSize;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Sidecar LRC
// ---------------------------------------------------------------------------

void loadSidecarLrc(const std::string& audioPath, TrackTags& tags) {
  const size_t slash = audioPath.find_last_of('/');
  const size_t dot = audioPath.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return;

  const std::string lrcPath = audioPath.substr(0, dot) + ".lrc";
  if (!Storage.exists(lrcPath.c_str())) return;

  HalFile f;
  if (!Storage.openFileForRead("Music", lrcPath.c_str(), f)) return;

  const uint64_t size64 = f.fileSize64();
  if (size64 > 300000) return;  // absurd LRC; ignore
  std::vector<char> raw((size_t)size64 + 1, '\0');
  const int got = f.read(raw.data(), (size_t)size64);
  if (got <= 0) return;
  raw.resize((size_t)got + 1);

  std::vector<std::string> plainLines;

  char* save = nullptr;
  for (char* line = strtok_r(raw.data(), "\n", &save); line != nullptr;
       line = strtok_r(nullptr, "\n", &save)) {
    const char* p = line;
    std::vector<uint32_t> times;
    // Every [tag] at line start; only time tags produce entries.
    while (*p == '[') {
      const char* after = nullptr;
      const uint32_t t = parseLrcTime(p + 1, &after);
      if (after == nullptr) break;
      if (*after == ']') {
        times.push_back(t);
        p = after + 1;
      } else {
        break;  // non-time tag like [ti:..]; stop scanning
      }
    }
    const std::string text = trimCopy(p);
    if (!times.empty()) {
      for (uint32_t t : times) tags.synced.push_back({t, text});
    } else if (!text.empty() && text[0] != '[') {
      plainLines.push_back(text);
    }
  }

  std::sort(tags.synced.begin(), tags.synced.end(),
            [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });

  if (tags.synced.empty() && tags.unsyncedLyrics.empty() && !plainLines.empty()) {
    for (size_t i = 0; i < plainLines.size(); ++i) {
      if (i) tags.unsyncedLyrics.push_back('\n');
      tags.unsyncedLyrics += plainLines[i];
    }
  }
}

}  // namespace TrackTagIO
