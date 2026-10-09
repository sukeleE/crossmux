#include "CoverLoader.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Print.h>

#include <cctype>
#include <algorithm>

#include "JpegToBmpConverter.h"
#include "PngToBmpConverter.h"

namespace CoverLoader {
namespace {

constexpr const char* TEMP_ART_PATH = "/.coverart_tmp";

// Print target that collects bytes in a vector.
class MemoryPrint : public Print {
 public:
  explicit MemoryPrint(size_t reserveBytes) { buffer_.reserve(reserveBytes); }

  size_t write(uint8_t b) override {
    buffer_.push_back(b);
    return 1;
  }

  size_t write(const uint8_t* buf, size_t size) override {
    buffer_.insert(buffer_.end(), buf, buf + size);
    return size;
  }

  std::vector<uint8_t>& buffer() { return buffer_; }

 private:
  std::vector<uint8_t> buffer_;
};

bool convertOpenFile(bool isPng, HalFile& file, std::vector<uint8_t>& out) {
  MemoryPrint sink(TARGET_SIZE * TARGET_SIZE / 8 + 256);
  bool ok;
  if (isPng) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(file, sink, TARGET_SIZE, TARGET_SIZE);
  } else {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(file, sink, TARGET_SIZE, TARGET_SIZE);
  }
  if (!ok || sink.buffer().empty()) {
    LOG_ERR("Music", "Cover conversion failed (png=%d)", isPng);
    return false;
  }
  out = std::move(sink.buffer());
  return true;
}

}  // namespace

std::string findSibling(const std::string& audioPath) {
  const size_t slash = audioPath.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? "/" : audioPath.substr(0, slash + 1);

  static const char* const kNames[] = {
      "cover.jpg", "cover.png", "cover.jpeg", "folder.jpg", "folder.png",
      "album.jpg", "album.png", "front.jpg", "front.png", "AlbumArt.jpg",
  };
  for (const char* name : kNames) {
    const std::string candidate = dir + name;
    if (Storage.exists(candidate.c_str())) return candidate;
  }
  return "";
}

bool fileToMonoBmp(const char* path, bool isPng, std::vector<uint8_t>& out) {
  HalFile file;
  if (!Storage.openFileForRead("MusicCover", path, file)) {
    LOG_ERR("Music", "Cannot open cover file: %s", path);
    return false;
  }
  return convertOpenFile(isPng, file, out);
}

bool embeddedToMonoBmp(const EmbeddedArt& art, std::vector<uint8_t>& out) {
  if (!art.data || art.size == 0) return false;

  {
    HalFile tmp;
    if (!Storage.openFileForWrite("MusicCover", TEMP_ART_PATH, tmp)) {
      LOG_ERR("Music", "Cannot create temp cover file");
      return false;
    }
    size_t written = 0;
    while (written < art.size) {
      const size_t chunk = std::min<size_t>(art.size - written, 4096);
      const size_t rc = tmp.write(art.data + written, chunk);
      if (rc == 0) {
        LOG_ERR("Music", "Temp cover write failed");
        tmp.flush();
        Storage.remove(TEMP_ART_PATH);
        return false;
      }
      written += rc;
    }
    tmp.flush();
  }

  HalFile file;
  if (!Storage.openFileForRead("MusicCover", TEMP_ART_PATH, file)) {
    Storage.remove(TEMP_ART_PATH);
    return false;
  }

  const bool ok = convertOpenFile(art.isPng, file, out);
  Storage.remove(TEMP_ART_PATH);
  return ok;
}

}  // namespace CoverLoader
