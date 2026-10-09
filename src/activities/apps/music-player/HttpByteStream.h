#pragma once

// Seekable HTTP byte stream for online music playback.
//
// Opens an http:// URL and serves it like a random-access file: seek()
// reconnects with an HTTP Range request, so the MP3 decoder's header analysis
// and the user's progress scrub work the same as on the SD path. Dropped
// connections are re-opened transparently at the current position.
//
// Only plain HTTP is supported (the NetEase CDN m*.music.126.net uses it).
//
// Internally a dedicated producer task keeps a PSRAM ring buffer filled a few
// tens of seconds ahead of the decoder. The public read() therefore serves
// from memory and never blocks on the (frequently stalling) link, while the
// producer absorbs reconnects in the background. The public interface is
// unchanged; all buffering is private.

#include <WiFi.h>
// WiFiClient is provided by WiFi.h (its includes on Arduino-ESP32; directly
// on the simulator shim), so no separate WiFiClient.h include here.

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class HalFile;

class HttpByteStream {
 public:
  HttpByteStream() = default;
  ~HttpByteStream();

  // Connect and start streaming from byte 0. Returns false on DNS/connect
  // failure or an unusable response.
  bool open(const std::string& url);
  void close();

  // Fill dst with len bytes (loops across partial reads). Returns bytes read
  // (may be short at EOF), 0 at clean EOF, -1 on unrecoverable failure.
  int read(uint8_t* dst, size_t len);
  // Reconnect with a Range request.
  bool seekSet(uint64_t position);

  uint64_t fileSize() const { return totalSize_; }
  uint64_t position() const { return position_; }

  // --- disk tee (cache the song as it streams; best effort) ------------------
  using TeeCommitCallback = std::function<void(bool success)>;
  // Arm BEFORE playMp3() after open(). The 16 KiB staging is allocated here;
  // failure leaves the stream usable but the tee disabled.
  void enableTee(const std::string& partPath, const std::string& finalPath,
                  uint32_t id, const std::string& sourceUrl);
  void disableTee();  // abort + free staging + drop config
  void setTeeCommitCallback(TeeCommitCallback cb);
  // True only across the playMp3() call (decoder header analysis). A seek in
  // this window is tolerated; any later seek (user scrub) aborts the tee.
  void setTeeHeaderWindow(bool inWindow);

 private:
  struct UrlParts {
    std::string host;
    uint16_t port = 80;
    std::string path;  // starts with '/'
  };
  static bool parseUrl(const std::string& url, UrlParts& out);
  // Connect with a Range request (plain GET when from == 0) and consume the
  // response headers, updating position_/totalSize_.
  bool connectRange(uint64_t from);
  bool parseHeaders(const std::string& headers, uint64_t requestedFrom);

  // --- ring buffer + producer -------------------------------------------------
  // Allocate a psramSize ring in PSRAM (0 => internal-RAM fallback) + sems.
  bool initRingAndLocks(size_t psramSize);
  void resetRingLockedState();  // indices/flags; flush stray semaphore tokens
  bool startProducer();
  void stopProducer();
  static void producerEntry(void* arg);
  void producerLoop();
  // Copy `n` producer bytes into the ring at head_ (handles wrap).
  void ringCopyIn(const uint8_t* src, size_t n);
  // Copy `n` bytes out of the ring at tail_ (handles wrap) and advance tail_.
  void ringCopyOut(uint8_t* dst, size_t n);
  // One socket read into d (single chunk); 0 = clean EOF, -1 = abort/fatal.
  // Reconnects transparently at fetchPos_ on a dead/stalled link.
  int rawReadSocket(uint8_t* d, size_t maxN, int& reconnectsLeft);
  bool reconnectAt(uint64_t at);
  // Delay up to ms, returning early (true) when a producer stop is requested.
  bool abortableDelay(uint32_t ms);

  // --- tee internals ---------------------------------------------------------
  void teeOpenLazy();
  void teeWrite(const uint8_t* d, size_t n);  // abs offset = fetchPos_ - n
  void teeFlushStaging();
  void teeBeforeRestart();
  void teeCommit();
  void teeAbort(const char* reason);
  friend class HttpTee;  // (no-op grouping; tee methods live in the .cpp)

  WiFiClient client_;
  UrlParts url_;
  uint64_t position_ = 0;    // bytes the CONSUMER has pulled out of the ring
  uint64_t totalSize_ = 0;
  bool open_ = false;

  uint8_t* ring_ = nullptr;  // PSRAM (internal-RAM fallback)
  size_t ringCap_ = 0;
  size_t head_ = 0;          // producer write index
  size_t tail_ = 0;          // consumer read index
  size_t fill_ = 0;          // bytes currently buffered
  bool ringEof_ = false;
  bool ringError_ = false;
  uint64_t fetchPos_ = 0;    // absolute byte cursor of bytes PRODUCED

  SemaphoreHandle_t ringMutex_ = nullptr;
  SemaphoreHandle_t freeSpace_ = nullptr;   // producer waits: ring full -> space
  SemaphoreHandle_t dataReady_ = nullptr;   // consumer waits: ring empty -> data
  SemaphoreHandle_t producerDone_ = nullptr;  // latched when producer exits

  TaskHandle_t producerTask_ = nullptr;
  std::atomic<bool> stopRequested_{false};

  // Tee state.
  bool teeEnabled_ = false;
  bool teeFileOpen_ = false;
  bool teeHeaderWindow_ = false;
  bool teeAborted_ = false;
  uint32_t teeId_ = 0;
  std::string teePartPath_;
  std::string teeFinalPath_;
  std::string teeUrl_;
  std::unique_ptr<HalFile> teeFile_;
  static constexpr size_t TEE_STAGING = 16 * 1024;
  uint8_t* teeStaging_ = nullptr;
  size_t teeStagingFill_ = 0;
  uint64_t teeStagingOffset_ = 0;  // song offset of staging[0]
  bool ringInPsram_ = false;       // set in initRingAndLocks
  TeeCommitCallback teeCb_;
};
