#include "MusicCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include "mbedtls/md5.h"
#endif

namespace {

constexpr const char* ROOT = "/.crosspoint/music-cache";
constexpr const char* DIR_SONGS = "/.crosspoint/music-cache/songs";
constexpr const char* DIR_COVERS = "/.crosspoint/music-cache/covers";
constexpr const char* DIR_LYRICS = "/.crosspoint/music-cache/lyrics";
constexpr const char* DIR_PLAYLISTS = "/.crosspoint/music-cache/playlists";
constexpr const char* DIR_USERS = "/.crosspoint/music-cache/users";
constexpr const char* DIR_SEARCH = "/.crosspoint/music-cache/search";
constexpr const char* MANIFEST_PATH = "/.crosspoint/music-cache/songs/manifest.txt";

std::string join2(const std::string& dir, const std::string& name) {
  return dir + "/" + name;
}

uint32_t nowSec() { return static_cast<uint32_t>(time(nullptr)); }

// Replace control bytes (tab/newline/etc.) with spaces so a field stays on one
// line/column. UTF-8 bytes (>=0x80) are preserved untouched.
std::string cleanField(std::string s) {
  for (char& c : s) {
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  }
  return s;
}

// --- PSRAM std::allocator for the resident manifest vector -------------------
#if !defined(SIMULATOR)
template <class T>
class PsramAllocator {
 public:
  using value_type = T;
  PsramAllocator() = default;
  template <class U>
  PsramAllocator(const PsramAllocator<U>&) {}
  T* allocate(std::size_t n) {
    if (n == 0) return nullptr;
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    return static_cast<T*>(p);  // null on total exhaustion (treated as OOM)
  }
  void deallocate(T* p, std::size_t) noexcept {
    if (p) heap_caps_free(p);
  }
};
template <class T, class U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) {
  return true;
}
template <class T, class U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) {
  return false;
}
#else
template <class T>
using PsramAllocator = std::allocator<T>;
#endif

struct ManifestEntry {
  uint32_t id;
  uint32_t size;
  uint32_t at;
};

std::vector<ManifestEntry, PsramAllocator<ManifestEntry>>& entries() {
  static std::vector<ManifestEntry, PsramAllocator<ManifestEntry>> v;
  return v;
}
std::mutex g_manMutex;

int findEntry(const std::vector<ManifestEntry, PsramAllocator<ManifestEntry>>& v,
              const uint32_t id) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].id == id) return static_cast<int>(i);
  return -1;
}

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

uint32_t leadingId(const std::string& name) {
  return static_cast<uint32_t>(std::strtoul(name.c_str(), nullptr, 10));
}

// --- atomic text/binary writes ----------------------------------------------
bool writeBytesAtomic(const std::string& path, const uint8_t* data, const size_t len) {
  const std::string tmp = path + ".tmp";
  Storage.remove(tmp.c_str());
  {
    HalFile f;
    if (!Storage.openFileForWrite("MUSICC", tmp, f)) return false;
    const size_t w = f.write(data, len);
    f.flush();
    if (w != len) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

bool writeTextAtomic(const std::string& path, const std::string& text) {
  return writeBytesAtomic(path, reinterpret_cast<const uint8_t*>(text.data()),
                          text.size());
}

std::string readText(const std::string& path) {
  if (!Storage.exists(path.c_str())) return std::string();
  String s = Storage.readFile(path.c_str());
  return std::string(s.c_str(), s.length());
}

uint64_t fileSize(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("MUSICC", path, f)) return 0;
  return f.fileSize64();
}

// --- tab-separated field splitter (mutates line) ----------------------------
int splitFields(char* s, char* out[], const int maxSlots) {
  int n = 0;
  out[n++] = s;
  for (; *s; ++s) {
    if (*s == '\t') {
      *s = '\0';
      if (n < maxSlots) out[n++] = s + 1;
    }
  }
  return n;
}

// Iterate lines of text, invoking fn(line-without-CR). Empty/blank lines skip.
template <class Fn>
void forEachLine(const std::string& text, Fn fn) {
  size_t start = 0;
  while (start <= text.size()) {
    size_t eol = text.find('\n', start);
    std::string line = text.substr(start, eol == std::string::npos
                                               ? std::string::npos
                                               : eol - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) fn(line);
    if (eol == std::string::npos) break;
    start = eol + 1;
  }
}

// A stored list is fresh iff the clock is valid (>=2024) and it was written
// within ttl. requireFresh=false always accepts (offline/stale fallback).
bool isFresh(const uint32_t storedAt, const uint32_t ttl, const bool requireFresh) {
  if (!requireFresh) return true;
  const uint32_t now = nowSec();
  if (now < 1700000000u) return false;  // clock unsynced/back-dated
  return storedAt > 0 && static_cast<int64_t>(now - storedAt) < static_cast<int64_t>(ttl);
}

std::string md5Hex(const std::string& key) {
  unsigned char d[16];
  char hex[33] = {};
#if !defined(SIMULATOR)
  if (mbedtls_md5(reinterpret_cast<const uint8_t*>(key.data()), key.size(), d) != 0)
    return std::string();
#else
  // Simulator has no mbedtls; FNV-1a is deterministic and sufficient here.
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : key) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  for (int i = 0; i < 8; ++i) d[i] = (h >> (i * 8)) & 0xFF;
#endif
  for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
  return std::string(hex, 32);
}

std::string trimCopy(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// --- manifest load/persist ---------------------------------------------------
void loadManifest(std::vector<ManifestEntry, PsramAllocator<ManifestEntry>>& v) {
  const std::string text = readText(MANIFEST_PATH);
  if (text.empty()) return;
  bool first = true;
  forEachLine(text, [&](const std::string& line) {
    if (first) {
      first = false;
      return;  // "CM1" header
    }
    unsigned id = 0, size = 0, at = 0;
    if (std::sscanf(line.c_str(), "%u,%u,%u", &id, &size, &at) == 3)
      v.push_back({static_cast<uint32_t>(id), static_cast<uint32_t>(size),
                   static_cast<uint32_t>(at)});
  });
}

bool persistManifestLocked() {
  std::string text = "CM1\n";
  for (const ManifestEntry& e : entries()) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u,%u,%u\n", e.id, e.size, e.at);
    text += buf;
  }
  return writeTextAtomic(MANIFEST_PATH, text);
}

// Sweep *.part / *.tmp left by a crashed/reset producer. Returns true on delete.
bool sweepStaging() {
  bool changed = false;
  auto dir = Storage.open(DIR_SONGS);
  if (!dir || !dir.isDirectory()) return false;
  char name[128];
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.getName(name, sizeof(name)) == 0) continue;
    const std::string n(name);
    if (f.isDirectory() || !(endsWith(n, ".part") || endsWith(n, ".tmp"))) continue;
    f.close();
    if (Storage.remove(join2(DIR_SONGS, n).c_str())) changed = true;
  }
  return changed;
}

// Collect ids of *.mp3 and *.meta files in the songs dir.
void collectSongIds(std::vector<uint32_t>& mp3, std::vector<uint32_t>& metas) {
  auto dir = Storage.open(DIR_SONGS);
  if (!dir || !dir.isDirectory()) return;
  char name[128];
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.getName(name, sizeof(name)) == 0 || f.isDirectory()) continue;
    const std::string n(name);
    if (endsWith(n, ".mp3")) mp3.push_back(leadingId(n));
    else if (endsWith(n, ".meta")) metas.push_back(leadingId(n));
  }
  std::sort(mp3.begin(), mp3.end());
  mp3.erase(std::unique(mp3.begin(), mp3.end()), mp3.end());
  std::sort(metas.begin(), metas.end());
  metas.erase(std::unique(metas.begin(), metas.end()), metas.end());
}

// Read the two-line song meta: "<size>\n<url>". False on missing/unreadable.
bool readSongMeta(const uint32_t id, uint64_t& size, std::string& url) {
  const std::string text = readText(join2(DIR_SONGS, std::to_string(id) + ".meta"));
  if (text.empty()) return false;
  size_t eol = text.find('\n');
  std::string s1 = text.substr(0, eol);
  if (!s1.empty() && s1.back() == '\r') s1.pop_back();
  size = std::strtoull(s1.c_str(), nullptr, 10);
  url.clear();
  if (eol != std::string::npos) {
    url = text.substr(eol + 1);
    while (!url.empty() && (url.back() == '\r' || url.back() == '\n')) url.pop_back();
  }
  return size > 0;
}

}  // namespace

namespace MusicCache {

std::string songFinalPath(const uint32_t id) {
  return join2(DIR_SONGS, std::to_string(id) + ".mp3");
}
std::string songPartPath(const uint32_t id) {
  return join2(DIR_SONGS, std::to_string(id) + ".part");
}
static std::string songMetaPath(const uint32_t id) {
  return join2(DIR_SONGS, std::to_string(id) + ".meta");
}

bool begin() {
  const char* dirs[] = {ROOT,       DIR_SONGS, DIR_COVERS, DIR_LYRICS,
                        DIR_PLAYLISTS, DIR_USERS, DIR_SEARCH};
  for (const char* d : dirs) {
    if (!Storage.ensureDirectoryExists(d)) {
      LOG_ERR("MusicCache", "mkdir failed: %s", d);
      return false;
    }
  }

  auto& v = entries();
  v.clear();
  loadManifest(v);

  bool changed = sweepStaging();

  std::vector<uint32_t> mp3, metas;
  collectSongIds(mp3, metas);

  // Validate each mp3 against its meta; build the set of good pairs.
  std::vector<uint32_t> valid;
  for (uint32_t id : mp3) {
    if (!std::binary_search(metas.begin(), metas.end(), id)) {
      Storage.remove(songFinalPath(id).c_str());  // crash before meta
      changed = true;
      continue;
    }
    uint64_t s = 0;
    std::string url;
    const bool gotMeta = readSongMeta(id, s, url);
    if (!gotMeta || s != fileSize(songFinalPath(id))) {
      Storage.remove(songFinalPath(id).c_str());
      Storage.remove(songMetaPath(id).c_str());
      changed = true;
      continue;
    }
    valid.push_back(id);
  }
  // Meta without an mp3 -> orphaned meta.
  for (uint32_t id : metas) {
    if (!std::binary_search(mp3.begin(), mp3.end(), id)) {
      Storage.remove(songMetaPath(id).c_str());
      changed = true;
    }
  }
  std::sort(valid.begin(), valid.end());

  // Drop manifest entries that no longer point at a good pair.
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const ManifestEntry& e) {
                           return !std::binary_search(valid.begin(), valid.end(), e.id);
                         }),
          v.end());
  // Adopt good pairs absent from the ledger (crash before manifest write).
  for (uint32_t id : valid) {
    if (findEntry(v, id) < 0) {
      uint64_t s = 0;
      std::string url;
      readSongMeta(id, s, url);
      v.push_back({id, static_cast<uint32_t>(s), nowSec()});
      changed = true;
    }
  }
  // Keep the ledger in FIFO order (oldest cached first), stable on ties.
  std::stable_sort(v.begin(), v.end(),
                   [](const ManifestEntry& a, const ManifestEntry& b) { return a.at < b.at; });

  if (changed) persistManifestLocked();

  LOG_INF("MusicCache", "ready: %u songs", static_cast<unsigned>(v.size()));
  return true;
}

bool hasCompleteSong(const OnlineTrack& track) {
  const std::string mp = songFinalPath(track.id);
  if (!Storage.exists(mp.c_str())) return false;
  uint64_t s = 0;
  std::string url;
  if (!readSongMeta(track.id, s, url)) return false;
  if (s != fileSize(mp)) return false;
  if (track.size != 0 && static_cast<uint64_t>(track.size) != s) return false;
  return true;
}

bool enforceBudget(const uint64_t need) {
  std::lock_guard<std::mutex> lock(g_manMutex);
  auto& v = entries();

  uint64_t used = 0;
  for (const ManifestEntry& e : v) used += e.size;

  bool evicted = false;
  auto evictFront = [&] {
    const uint32_t id = v.front().id;
    used -= v.front().size;
    v.erase(v.begin());
    Storage.remove(songFinalPath(id).c_str());
    Storage.remove(songMetaPath(id).c_str());
    evicted = true;
  };

  while (!v.empty() && used + need > SONG_BUDGET_BYTES) evictFront();

  uint64_t total = 0, freeB = 0;
  bool haveSpace = Storage.getSpace(total, freeB);
  if (!haveSpace) freeB = 0;
  while (!v.empty() && freeB < need + FREE_SPACE_MARGIN) {
    evictFront();
    if (Storage.getSpace(total, freeB)) {
    } else {
      freeB = 0;
    }
  }

  if (used + need > SONG_BUDGET_BYTES) {
    if (evicted) persistManifestLocked();
    return false;  // single song larger than the whole budget
  }
  if (haveSpace && freeB < need + FREE_SPACE_MARGIN) {
    if (evicted) persistManifestLocked();
    return false;
  }
  if (evicted) persistManifestLocked();
  return true;
}

void onSongCommitted(const uint32_t id, uint64_t size, const std::string& url) {
  std::lock_guard<std::mutex> lock(g_manMutex);
  auto& v = entries();
  if (size == 0) size = fileSize(songFinalPath(id));
  if (size == 0) return;

  // Two-line meta: size then URL.
  const std::string meta = std::to_string(size) + "\n" + url + "\n";
  if (!writeTextAtomic(songMetaPath(id), meta)) return;

  const int existing = findEntry(v, id);
  if (existing >= 0) v.erase(v.begin() + existing);
  v.push_back({id, static_cast<uint32_t>(size), nowSec()});
  persistManifestLocked();
  LOG_INF("MusicCache", "song committed id=%u size=%u", id,
          static_cast<unsigned>(size));
}

// --- lyrics ------------------------------------------------------------------
bool writeLyrics(const uint32_t id, const std::string& raw) {
  return writeTextAtomic(join2(DIR_LYRICS, std::to_string(id) + ".json"), raw);
}
bool readLyrics(const uint32_t id, TrackTags& tags) {
  const std::string body = readText(join2(DIR_LYRICS, std::to_string(id) + ".json"));
  if (body.empty()) return false;
  return OnlineMusicApi::parseLyricBody(body, tags);
}

// --- covers ------------------------------------------------------------------
bool writeCoverBmp(const uint32_t id, const std::vector<uint8_t>& bmp) {
  if (bmp.empty()) return false;
  return writeBytesAtomic(join2(DIR_COVERS, std::to_string(id) + ".bmp"),
                          bmp.data(), bmp.size());
}
bool readCoverBmp(const uint32_t id, std::vector<uint8_t>& bmp) {
  const std::string path = join2(DIR_COVERS, std::to_string(id) + ".bmp");
  HalFile f;
  if (!Storage.openFileForRead("MUSICC", path, f)) return false;
  const uint64_t sz = f.fileSize64();
  if (sz == 0) return false;
  bmp.resize(static_cast<size_t>(sz));
  const int r = f.read(bmp.data(), bmp.size());
  return static_cast<uint64_t>(r) == sz;
}

// --- track-list (TSV) --------------------------------------------------------
std::string encodeTracks(const std::vector<OnlineTrack>& tracks, const uint32_t t) {
  std::string out = "CML1 " + std::to_string(t) + "\n";
  for (const OnlineTrack& tr : tracks) {
    out += std::to_string(tr.id) + "\t";
    out += std::to_string(tr.durationMs) + "\t";
    out += cleanField(tr.artist) + "\t";
    out += cleanField(tr.album) + "\t";
    out += cleanField(tr.title) + "\n";
  }
  return out;
}

bool decodeTracks(const std::string& text, std::vector<OnlineTrack>& out) {
  bool first = true;
  forEachLine(text, [&](std::string& line) {
    if (first) {
      first = false;  // header
      return;
    }
    char* slots[5];
    if (splitFields(&line[0], slots, 5) < 5) return;
    OnlineTrack tr;
    tr.id = static_cast<uint32_t>(std::strtoul(slots[0], nullptr, 10));
    tr.durationMs = static_cast<uint32_t>(std::strtoul(slots[1], nullptr, 10));
    tr.artist = slots[2];
    tr.album = slots[3];
    tr.title = slots[4];
    if (tr.id != 0 && !tr.title.empty()) out.push_back(std::move(tr));
  });
  return !out.empty();
}

uint32_t trackListTime(const std::string& text) {
  unsigned t = 0;
  if (std::sscanf(text.c_str(), "CML1 %u", &t) == 1) return t;
  return 0;
}

bool writePlaylist(const uint64_t playlistId, const std::vector<OnlineTrack>& tracks) {
  const std::string path = join2(DIR_PLAYLISTS, std::to_string(playlistId) + ".json");
  return writeTextAtomic(path, encodeTracks(tracks, nowSec()));
}
bool readPlaylist(const uint64_t playlistId, std::vector<OnlineTrack>& out,
                  const bool requireFresh) {
  const std::string text = readText(join2(DIR_PLAYLISTS, std::to_string(playlistId) + ".json"));
  if (text.empty() || !isFresh(trackListTime(text), TTL_PLAYLIST_TRACKS_S, requireFresh))
    return false;
  return decodeTracks(text, out);
}

bool writeSearch(const std::string& keyword, const std::vector<OnlineTrack>& tracks) {
  const std::string path = join2(DIR_SEARCH, md5Hex(trimCopy(keyword)) + ".json");
  return writeTextAtomic(path, encodeTracks(tracks, nowSec()));
}
bool readSearch(const std::string& keyword, std::vector<OnlineTrack>& out,
                const bool requireFresh) {
  const std::string text = readText(join2(DIR_SEARCH, md5Hex(trimCopy(keyword)) + ".json"));
  if (text.empty() || !isFresh(trackListTime(text), TTL_SEARCH_S, requireFresh))
    return false;
  return decodeTracks(text, out);
}

// --- per-uid playlist list (TSV) --------------------------------------------
bool writeUserPlaylists(const uint64_t uid,
                        const std::vector<OnlineMusicApi::PlaylistInfo>& playlists) {
  std::string out = "CMU1 " + std::to_string(nowSec()) + "\n";
  for (const OnlineMusicApi::PlaylistInfo& p : playlists) {
    out += std::to_string(p.id) + "\t";
    out += std::to_string(p.trackCount) + "\t";
    out += cleanField(p.name) + "\n";
  }
  return writeTextAtomic(join2(DIR_USERS, std::to_string(uid) + ".json"), out);
}
bool readUserPlaylists(const uint64_t uid,
                       std::vector<OnlineMusicApi::PlaylistInfo>& out,
                       const bool requireFresh) {
  const std::string text = readText(join2(DIR_USERS, std::to_string(uid) + ".json"));
  if (text.empty()) return false;
  unsigned t = 0;
  if (std::sscanf(text.c_str(), "CMU1 %u", &t) != 1) return false;
  if (!isFresh(t, TTL_USER_PLAYLISTS_S, requireFresh)) return false;

  bool first = true;
  forEachLine(text, [&](std::string& line) {
    if (first) {
      first = false;
      return;
    }
    char* slots[3];
    if (splitFields(&line[0], slots, 3) < 3) return;
    OnlineMusicApi::PlaylistInfo p;
    p.id = std::strtoull(slots[0], nullptr, 10);
    p.trackCount = static_cast<uint32_t>(std::strtoul(slots[1], nullptr, 10));
    p.name = slots[2];
    if (p.id != 0 && !p.name.empty()) out.push_back(std::move(p));
  });
  return !out.empty();
}

// --- management --------------------------------------------------------------
uint64_t sumDir(const std::string& path, const int depth);

uint64_t totalBytes() { return sumDir(ROOT, 8); }

bool clearAll() {
  Storage.removeDir(ROOT);
  return begin();
}

uint64_t sumDir(const std::string& path, const int depth) {
  if (depth < 0) return 0;
  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) return 0;
  uint64_t sum = 0;
  char name[128];
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.getName(name, sizeof(name)) == 0) continue;
    if (f.isDirectory()) {
      f.close();
      sum += sumDir(join2(path, name), depth - 1);
    } else {
      sum += f.fileSize64();
    }
  }
  return sum;
}

}  // namespace MusicCache
