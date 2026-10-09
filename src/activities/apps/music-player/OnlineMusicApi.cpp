#include "OnlineMusicApi.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#if !defined(SIMULATOR)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#include "Logging.h"
#include "EapiCrypto.h"
#include "MusicCache.h"
#include "WeapiCrypto.h"

namespace OnlineMusicApi {

namespace {

constexpr const char* HOST = "music.163.com";
constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
// On the phone hotspot, a larger body intermittently stalls mid-transfer (a
// segment or window-update ACK is lost and the sender's retransmit/persist
// probe can take well over 15s). Wait long enough for recovery instead of
// aborting a transfer that would otherwise complete.
// A body that stalls for this long mid-transfer never recovers on this link,
// so keep it short and let callers retry with a fresh connection rather than
// freezing for a minute on a permanently wedged transfer.
constexpr uint32_t BODY_STALL_MS = 5000;

// A mainstream mobile-browser identity. A custom UA ("...CrossPoint/1.0") on
// a mobile/carrier egress IP trips NetEase risk control (code 8821) on the QR
// login endpoints; a standard Chrome UA does not.
constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Linux; Android 11; Pixel 5) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Mobile Safari/537.36";

// Simple in-memory cookie jar. Browsers carry the NMTID device-fingerprint
// cookie issued by /unikey into every /client/login poll; dropping it is
// another risk-control trigger.
class CookieJar {
 public:
  void set(const std::string& name, std::string value) {
    for (auto& p : entries_) {
      if (p.first == name) {
        p.second = std::move(value);
        return;
      }
    }
    entries_.push_back({name, std::move(value)});
  }
  std::string get(const std::string& name) const {
    for (const auto& p : entries_) {
      if (p.first == name) return p.second;
    }
    return "";
  }
  bool has(const std::string& name) const { return !get(name).empty(); }
  std::string header() const {
    std::string out;
    for (const auto& p : entries_) {
      if (!out.empty()) out += "; ";
      out += p.first;
      out += "=";
      out += p.second;
    }
    return out;
  }
  bool empty() const { return entries_.empty(); }
  void clear() { entries_.clear(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

CookieJar g_jar;

// Spoofed mainland client IP (X-Real-IP / X-Forwarded-For). The login edge
// honors these headers; empty unless the QR flow sets them.
std::string g_realIp;

struct RawResponse {
  int status = 0;
  std::string body;
  // Lowercased Content-Encoding of the body ("deflate"/"gzip"/""), used to
  // transparently inflate after draining.
  std::string contentEncoding;
  // True when the body uses Transfer-Encoding: chunked; must be de-chunked
  // BEFORE any inflate (each chunk carries the still-compressed bytes).
  bool chunked = false;
  // Captured from Set-Cookie (empty unless the server issued them).
  std::string setMusicU;
  std::string setCsrf;
};

std::string trimmed(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  return s.substr(start);
}

// Pull "name=value" out of a Set-Cookie header line; empty when absent.
std::string cookieValue(const std::string& headerLine, const char* name) {
  const size_t nameLen = std::strlen(name);
  size_t search = 0;
  while (search < headerLine.size()) {
    const size_t eq = headerLine.find('=', search);
    if (eq == std::string::npos || eq < search + nameLen) break;
    if (eq - search == nameLen && headerLine.compare(search, nameLen, name) == 0) {
      size_t end = eq + 1;
      while (end < headerLine.size() && headerLine[end] != ';') ++end;
      return trimmed(headerLine.substr(eq + 1, end - eq - 1));
    }
    const size_t semi = headerLine.find(';', search);
    if (semi == std::string::npos) break;
    search = semi + 1;
    while (search < headerLine.size() && headerLine[search] == ' ') ++search;
  }
  return "";
}

// Read the response headers of an open connection and parse status,
// Set-Cookie (MUSIC_U / __csrf are captured; other cookies enter the jar) and
// Content-Encoding. Works for both plain and TLS clients because both derive
// from WiFiClient.
bool collectResponseHeaders(WiFiClient& client, RawResponse& resp,
                            const uint32_t timeoutMs) {
  // Session cookies observed on THIS response (kept out of g_jar, see below).
  std::string scMusicU, scCsrf;
  // content-encoding drives the transparent inflate after the body is drained.
  std::string hEnc;

  std::string headers;
  uint32_t deadline = millis() + timeoutMs;
  bool headerDone = false;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    while (client.available()) {
      headers += static_cast<char>(client.read());
      if (headers.size() >= 4 && headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
        headerDone = true;
        break;
      }
    }
    if (headerDone) break;
    if (!client.connected()) break;
    delay(2);
  }
  if (!headerDone) {
    LOG_ERR("OnlineMusic", "header timeout");
    return false;
  }

  const size_t eol = headers.find("\r\n");
  const std::string statusLine = headers.substr(0, eol);
  // Token after the first space is the status code ("HTTP/1.1 200 OK");
  // atoi stops at the following space. Finding the second space instead would
  // start the parse at "OK" and yield 0, failing every request.
  const size_t sp = statusLine.find(' ');
  resp.status = std::atoi(statusLine.substr(sp + 1).c_str());

  size_t pos = eol + 2;
  while (pos < headers.size()) {
    const size_t next = headers.find("\r\n", pos);
    const std::string line =
        headers.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    if (line.empty()) break;
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string name = trimmed(line.substr(0, colon));
      if (name.size() == 10 &&
          std::equal(name.begin(), name.end(), "set-cookie",
                     [](char a, char b) { return std::tolower(a) == b; })) {
        // First name=value pair in the header (before any ';').
        std::string pair = trimmed(line.substr(colon + 1));
        const size_t semi = pair.find(';');
        if (semi != std::string::npos) pair.resize(semi);
        pair = trimmed(pair);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
          const std::string cookieName = trimmed(pair.substr(0, eq));
          const std::string cookieVal = trimmed(pair.substr(eq + 1));
          // Never auto-install session cookies from an arbitrary response:
          // a failed login returns an anonymous/empty MUSIC_U, which would
          // make hasSession() flip true and drive later calls unauthenticated.
          // They enter the jar only via setSession() after a verified login.
          if (cookieName == "MUSIC_U") {
            scMusicU = cookieVal;
          } else if (cookieName == "__csrf") {
            scCsrf = cookieVal;
          } else {
            g_jar.set(cookieName, cookieVal);
          }
        }
      } else if (name.size() == 16 &&
                 std::equal(name.begin(), name.end(), "content-encoding",
                            [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == b;
                            })) {
        hEnc = trimmed(line.substr(colon + 1));
      } else if (name.size() == 17 &&
                 std::equal(name.begin(), name.end(), "transfer-encoding",
                            [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == b;
                            })) {
        // HTTP/1.1 servers may chunk even when the client asked Connection: close.
        std::string te = trimmed(line.substr(colon + 1));
        std::transform(te.begin(), te.end(), te.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        resp.chunked = te.find("chunked") != std::string::npos;
      }
    }
    if (next == std::string::npos) break;
    pos = next + 2;
  }

  // Normalize the encoding once for the transparent inflate in rawRequest().
  resp.contentEncoding = hEnc;
  std::transform(resp.contentEncoding.begin(), resp.contentEncoding.end(),
                 resp.contentEncoding.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Prefer cookies issued on this response; fall back to an installed session.
  resp.setMusicU = !scMusicU.empty() ? scMusicU : g_jar.get("MUSIC_U");
  resp.setCsrf = !scCsrf.empty() ? scCsrf : g_jar.get("__csrf");
  return true;
}

// read either into a string (rawRequest) or streamed straight into a filtered
// JSON parser (playlistTracks) instead of buffering a possibly multi-MB body.
bool sendRequestHeaders(const char* method, const std::string& path,
                        const std::string& body, RawResponse& resp,
                        WiFiClient& client,
                        const uint32_t timeoutMs = CONNECT_TIMEOUT_MS) {
  client.setTimeout(timeoutMs);
  client.setNoDelay(true);  // send the request segment immediately, no Nagle wait
  if (!client.connect(HOST, 80)) {
    LOG_ERR("OnlineMusic", "connect failed: %s", HOST);
    return false;
  }
  LOG_INF("OnlineMusic", "connected to %s:%d", client.remoteIP().toString().c_str(),
          client.remotePort());

  std::string request;
  request.reserve(path.size() + body.size() + 512);
  request += method;
  request += ' ';
  request += path;
  // HTTP/1.0: the edge withholds/stalls the body under 1.1 keep-alive framing;
  // 1.0 with Connection: close makes it send the whole body in one go.
  request += " HTTP/1.0\r\n";
  request += "Host: ";
  request += HOST;
  request += "\r\n";
  request += "User-Agent: ";
  request += USER_AGENT;
  request += "\r\n";
  request += "Accept: */*\r\n";
  request += "Accept-Language: zh-CN,zh;q=0.9\r\n";
  // Ask the server to compress JSON. Over the phone hotspot, bodies larger than
  // one TCP window (~5.7KB) frequently stall permanently; the playlist detail
  // is ~9.5KB raw but ~2.5KB under deflate, which fits in a single window.
  // Prefer "deflate" (a plain zlib wrapper the ROM inflator parses directly);
  // fall back to gzip (handled by manually skipping its wrapper).
  request += "Accept-Encoding: deflate, gzip\r\n";
  // The official site calls /api via jQuery $.ajax (X-Requested-With) with CORS
  // Sec-Fetch metadata. Matching it keeps the confirm-time risk check happy.
  request += "X-Requested-With: XMLHttpRequest\r\n";
  if (!g_realIp.empty()) {
    request += "X-Real-IP: " + g_realIp + "\r\n";
    request += "X-Forwarded-For: " + g_realIp + "\r\n";
  }
  request += "Sec-Fetch-Site: same-origin\r\n";
  request += "Sec-Fetch-Mode: cors\r\n";
  request += "Sec-Fetch-Dest: empty\r\n";
  request += "Referer: http://music.163.com/\r\n";
  request += "Connection: close\r\n";
  if (!g_jar.empty()) {
    request += "Cookie: ";
    request += g_jar.header();
    request += "\r\n";
  }
  if (!body.empty()) {
    request += "Content-Type: application/x-www-form-urlencoded\r\n";
    // Browsers and the reference client send Origin on weapi POSTs; the login
    // edge withholds/tarpits the response body when it is missing.
    request += "Origin: http://music.163.com\r\n";
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\n";
  }
  request += "\r\n";
  request += body;
  client.print(request.c_str());

  return collectResponseHeaders(client, resp, timeoutMs);
}

// Read the whole body of an open response (server closes the connection).
// Safety valve for string-buffered responses. Real payloads here are a few KB;
// truncating a runaway body makes the later JSON parse fail instead of letting
// operator new throw std::bad_alloc into a -fno-exceptions build (-> abort).
constexpr size_t MAX_BODY_BYTES = 96 * 1024;

bool drainBody(WiFiClient& client, RawResponse& resp) {
  // --- Body (server closes after) --------------------------------------------
  const uint32_t entryMs = millis();
  uint32_t firstByteMs = 0;
  uint32_t lastDataMs = entryMs;
  // Read unconditionally (a blocking timed read) rather than gating on
  // available(). The gated loop stalled permanently mid-body on this hotspot
  // while an identical unconditional read pulled all 91KB; keeping the socket
  // drained without gaps avoids the window/retransmit deadlock that triggers.
  while (true) {
    char buf[512];
    const int n = client.read((uint8_t*)buf, sizeof(buf));
    if (n > 0) {
      if (firstByteMs == 0) firstByteMs = millis();
      const size_t room =
          resp.body.size() < MAX_BODY_BYTES ? MAX_BODY_BYTES - resp.body.size() : 0;
      resp.body.append(buf, std::min<size_t>(n, room));
      lastDataMs = millis();
      if (resp.body.size() >= MAX_BODY_BYTES) {
        LOG_ERR("OnlineMusic", "body capped at %u bytes",
                static_cast<unsigned>(MAX_BODY_BYTES));
        return true;  // truncated body; the caller's parse will fail safely
      }
    }
    if (!client.connected() && client.available() == 0) break;
    if (static_cast<int32_t>(millis() - lastDataMs) > static_cast<int32_t>(BODY_STALL_MS)) {
      LOG_ERR("OnlineMusic",
              "body STALL after %u ms (first byte %s), got %u bytes, heap %u min %u",
              static_cast<unsigned>(millis() - entryMs),
              firstByteMs ? "yes" : "NO",
              static_cast<unsigned>(resp.body.size()),
              static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMinFreeHeap()));
      return false;
    }
    delay(2);
  }
  LOG_INF("OnlineMusic", "body done %u bytes, first in %u ms, total %u ms",
          static_cast<unsigned>(resp.body.size()),
          static_cast<unsigned>(firstByteMs ? firstByteMs - entryMs : 0),
          static_cast<unsigned>(millis() - entryMs));
  return true;
}

// ESP-ROM ships a miniz inflator (no new library). We only need the single
// memory-to-memory entry point and the zlib-header flag; forward-declaring
// them avoids pulling in the full ROM header and its macro configuration.
extern "C" size_t tinfl_decompress_mem_to_mem(void* out, size_t out_len, const void* src,
                                              size_t src_len, int flags);
constexpr int TINFL_F_PARSE_ZLIB_HEADER = 1;

// Locate the raw-deflate payload inside an RFC1952 gzip blob. tinfl parses a
// zlib wrapper or raw deflate, but not a gzip header, so skip it ourselves.
// Returns false on a malformed/truncated header. The 8-byte gzip trailer may
// remain in [off, len); raw inflate stops at the final block and ignores it.
bool gzipPayloadRange(const std::string& in, size_t& off, size_t& len) {
  if (in.size() < 10 || static_cast<uint8_t>(in[0]) != 0x1f ||
      static_cast<uint8_t>(in[1]) != 0x8b) {  // gzip magic = 1f 8b
    LOG_ERR("OnlineMusic", "gz fail: bad magic or too small (%u bytes)",
            static_cast<unsigned>(in.size()));
    return false;
  }
  // Byte[2]=CM (8=deflate), byte[3]=FL flags; the optional fields below are
  // selected by FL, so read the flags byte, not the compression-method byte.
  const uint8_t fl = static_cast<uint8_t>(in[3]);
  size_t p = 10;
  if (fl & 0x04) {  // FEXTRA: 2-byte LE length + that many bytes
    if (p + 2 > in.size()) {
      LOG_ERR("OnlineMusic", "gz fail: truncated FEXTRA");
      return false;
    }
    const uint16_t xlen = static_cast<uint8_t>(in[p]) |
                          (static_cast<uint16_t>(static_cast<uint8_t>(in[p + 1])) << 8);
    p += 2 + xlen;
  }
  if (fl & 0x08) {  // FNAME: NUL-terminated
    while (p < in.size() && in[p] != '\0') ++p;
    ++p;
  }
  if (fl & 0x10) {  // FCOMMENT: NUL-terminated
    while (p < in.size() && in[p] != '\0') ++p;
    ++p;
  }
  if (fl & 0x02) p += 2;  // FHCRC
  if (p >= in.size()) {
    LOG_ERR("OnlineMusic", "gz fail: header consumed whole body (fl=%02x)",
            static_cast<unsigned>(fl));
    return false;
  }
  off = p;
  len = in.size() - p;
  return true;
}

// Inflate a deflate (zlib-wrapped) or gzip response body into `out`. The
// output length is unknown up front, so start at a generous multiple of the
// compressed size and grow on TINFL_STATUS_HAS_MORE_OUTPUT; tinfl reports both
// that and corruption as (size_t)-1, hence the bounded retries.
bool inflateBody(const std::string& in, std::string& out, bool gzip) {
  const char* src = in.data();
  size_t srcLen = in.size();
  int flags = TINFL_F_PARSE_ZLIB_HEADER;
  if (gzip) {
    size_t off = 0;
    if (!gzipPayloadRange(in, off, srcLen)) return false;
    src += off;
    flags = 0;  // raw deflate after the gzip wrapper
  }
  constexpr size_t MAX_OUT = 128 * 1024;
  size_t cap = std::max<size_t>(srcLen * 6, 4096);
  for (int tries = 0; tries < 6; ++tries) {
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[cap]);
    if (!buf) {
      LOG_ERR("OnlineMusic", "alloc %u failed", static_cast<unsigned>(cap));
      cap = std::min<size_t>(cap * 2, MAX_OUT);
      continue;
    }
    const size_t n = tinfl_decompress_mem_to_mem(buf.get(), cap, src, srcLen, flags);
    if (n == static_cast<size_t>(-1)) {
      LOG_ERR("OnlineMusic", "tinfl -1 cap=%u srcLen=%u gz=%d heap=%u",
              static_cast<unsigned>(cap), static_cast<unsigned>(srcLen),
              gzip ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()));
      if (cap >= MAX_OUT) return false;
      cap = std::min<size_t>(cap * 2, MAX_OUT);
      continue;
    }
    out.assign(reinterpret_cast<const char*>(buf.get()), n);
    return true;
  }
  return false;
}

// Remove HTTP/1.1 chunked transfer framing:
//   <hex-size>[;ext]\r\n <data> \r\n ... 0\r\n [trailers]\r\n
// Runs before inflate because each chunk holds the still-compressed bytes.
bool dechunkBody(std::string& body) {
  std::string out;
  out.reserve(body.size());
  const size_t total = body.size();
  size_t pos = 0;
  while (pos < total) {
    const size_t eol = body.find("\r\n", pos);
    if (eol == std::string::npos) return false;
    std::string sizeLine = body.substr(pos, eol - pos);
    const size_t semi = sizeLine.find(';');  // ignore chunk extensions
    if (semi != std::string::npos) sizeLine.resize(semi);
    sizeLine = trimmed(sizeLine);
    if (sizeLine.empty()) return false;
    size_t chunkLen = 0;
    for (char c : sizeLine) {
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      chunkLen = chunkLen * 16 + d;
    }
    pos = eol + 2;
    if (chunkLen == 0) {  // last chunk; trailers (if any) are discarded
      body = std::move(out);
      return true;
    }
    if (pos + chunkLen + 2 > total) return false;
    out.append(body, pos, chunkLen);
    pos += chunkLen;
    if (body.compare(pos, 2, "\r\n") != 0) return false;
    pos += 2;
  }
  return false;  // never saw the terminating zero-length chunk
}

// Decode a possibly-compressed body by SNIFFING THE BYTES rather than
// trusting Content-Encoding. The interface/edge servers on this carrier network
// sometimes label a plain-JSON reply "gzip" (or otherwise mislabel encoding),
// which made a strict header check discard a perfectly good response. A body
// that is not recognized as compressed is returned as-is (plain JSON).
// Inflate (if needed) an ALREADY-DECHUNKED body, sniffing the bytes rather
// than trusting Content-Encoding. The interface/edge servers on this carrier
// network sometimes label a plain-JSON reply "gzip" (or otherwise mislabel
// encoding), which made a strict header check discard a good response.
bool inflateByMagic(RawResponse& resp) {
  const size_t n = resp.body.size();
  if (n >= 2 &&
      static_cast<uint8_t>(resp.body[0]) == 0x1f &&
      static_cast<uint8_t>(resp.body[1]) == 0x8b) {
    std::string plain;  // gzip magic 1f 8b
    if (!inflateBody(resp.body, plain, true)) return false;
    resp.body = std::move(plain);
    return true;
  }
  // A JSON object/array (or anything printable that is not zlib) is plain.
  // Only inflate when the header promised deflate and the body does not start
  // like JSON; zlib-wrapped streams begin with 0x78.
  if (resp.contentEncoding == "deflate" && n >= 2 &&
      resp.body[0] != '{' && resp.body[0] != '[' &&
      static_cast<uint8_t>(resp.body[0]) == 0x78) {
    std::string plain;
    if (!inflateBody(resp.body, plain, false)) return false;
    resp.body = std::move(plain);
  }
  // Header claimed gzip but there is no gzip magic: the body is plain JSON.
  return true;
}

bool decodeBodyByMagic(RawResponse& resp) {
  // De-chunk first (each chunk holds the still-compressed bytes), then inflate.
  if (resp.chunked && !dechunkBody(resp.body)) {
    LOG_ERR("OnlineMusic", "dechunk failed (%u bytes)",
            static_cast<unsigned>(resp.body.size()));
    return false;
  }
  resp.chunked = false;
  return inflateByMagic(resp);
}

bool rawRequest(const char* method, const std::string& path,
                const std::string& body, RawResponse& resp,
                const uint32_t timeoutMs = CONNECT_TIMEOUT_MS) {
  WiFiClient client;
  if (!sendRequestHeaders(method, path, body, resp, client, timeoutMs))
    return false;
  if (!drainBody(client, resp)) return false;  // body can be large but bounded
  client.stop();
  // Transparently expand a compressed body so every caller parses plain JSON.
  return decodeBodyByMagic(resp);
}

bool rawGet(const std::string& path, RawResponse& resp,
            const uint32_t timeoutMs = CONNECT_TIMEOUT_MS) {
  return rawRequest("GET", path, "", resp, timeoutMs);
}

// --- HTTPS transport to the official app backend -----------------------------
// On some phone hotspots port 80 to music.163.com (111.48.137.146) is fully
// blackholed at the response-header stage, while the interface host on 443
// answers in ~150 ms. The same /api/... paths live there, so this is the
// reliable transport for every post-login call. Identity is verified with the
// embedded Mozilla CA bundle and the request carries a PC-client fingerprint.
namespace {

constexpr const char* IFACE_HOST = "interface.music.163.com";

extern "C" const uint8_t _binary_x509_crt_bundle_start[];
extern "C" const uint8_t _binary_x509_crt_bundle_end[];

std::string g_ifaceDeviceId;

const std::string& ifaceDeviceId() {
  if (g_ifaceDeviceId.empty()) {
    static const char* kHex = "0123456789ABCDEF";
    for (int i = 0; i < 52; ++i) g_ifaceDeviceId += kHex[esp_random() % 16];
  }
  return g_ifaceDeviceId;
}

// PC-client fingerprint cookie prefix used by every interface request,
// optionally followed by the installed session (MUSIC_U/csrf).
std::string pcFingerprint() {
  const uint64_t unixSec = static_cast<uint64_t>(time(nullptr));
  char requestId[32];
  std::snprintf(requestId, sizeof(requestId), "%llu_%04u",
                static_cast<unsigned long long>(unixSec) * 1000,
                esp_random() % 10000);
  char fp[360];
  std::snprintf(
      fp, sizeof(fp),
      "osver=Microsoft-Windows-10-Professional-build-19045-64bit; "
      "deviceId=%s; os=pc; appver=3.1.17.204416; versioncode=140; "
      "buildver=%llu; channel=netease; requestId=%s",
      ifaceDeviceId().c_str(), static_cast<unsigned long long>(unixSec),
      requestId);
  return std::string(fp);
}

std::string ifaceCookie() {
  std::string c = pcFingerprint();
  if (!g_jar.empty()) c += "; " + g_jar.header();
  return c;
}

// ArduinoJson allocator that places EVERYTHING in PSRAM (8 MB, ~6.7 MB free),
// with an internal-RAM fallback. Large decoded bodies and their JSON tree thus
// never pressure the scarce ~270 KB internal pool.
class PoolAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    if (size == 0) return nullptr;
#if !defined(SIMULATOR)
    if (void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
      return p;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);  // internal fallback
#else
    return malloc(size);
#endif
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
#if !defined(SIMULATOR)
    heap_caps_free(ptr);  // frees a block from either pool
#else
    free(ptr);
#endif
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    if (size == 0) {
      deallocate(ptr);
      return nullptr;
    }
#if !defined(SIMULATOR)
    // Relocates (copy+free) if the old block lives in a different pool.
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return realloc(ptr, size);
#endif
  }
};

// Read-only Stream over a fixed memory buffer, so ArduinoJson can parse an
// arbitrary (PSRAM-resident) buffer without needing a NUL-terminated string.
class MemStream : public Stream {
 public:
  MemStream(const void* data, size_t len)
      : data_(static_cast<const uint8_t*>(data)), len_(len) {}

  int available() override { return static_cast<int>(len_ - pos_); }
  int read() override {
    if (pos_ >= len_) return -1;
    return data_[pos_++];
  }
  int peek() override {
    if (pos_ >= len_) return -1;
    return data_[pos_];
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}
  size_t readBytes(char* buffer, size_t length) override {
    const size_t k = std::min(length, len_ - pos_);
    std::memcpy(buffer, data_ + pos_, k);
    pos_ += k;
    return k;
  }

 private:
  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
};

// Decode a (de-chunked, possibly gzip/deflate) response into a single plain
// buffer allocated through `alloc`. gzip is detected by magic; deflate only
// when the header promises it and the bytes look like a zlib stream; otherwise
// the body is copied as-is (plain JSON).
bool decodeToBuffer(const RawResponse& raw, ArduinoJson::Allocator& alloc,
                    void*& out, size_t& outLen) {
  out = nullptr;
  outLen = 0;
  const size_t n = raw.body.size();

  const char* src = raw.body.data();
  size_t srcLen = n;
  int flags = TINFL_F_PARSE_ZLIB_HEADER;
  if (n >= 2 && static_cast<uint8_t>(raw.body[0]) == 0x1f &&
      static_cast<uint8_t>(raw.body[1]) == 0x8b) {
    size_t off = 0;  // gzip: skip the wrapper, then raw-deflate
    if (!gzipPayloadRange(raw.body, off, srcLen)) return false;
    src += off;
    flags = 0;
  } else if (!(raw.contentEncoding == "deflate" && n >= 2 &&
               raw.body[0] != '{' && raw.body[0] != '[' &&
               static_cast<uint8_t>(raw.body[0]) == 0x78)) {
    void* b = alloc.allocate(n == 0 ? 1 : n);  // plain (or mislabeled) JSON
    if (!b) return false;
    std::memcpy(b, raw.body.data(), n);
    out = b;
    outLen = n;
    return true;
  }

  // Decode target lives in PSRAM, so a generous ceiling is fine. A v6 body
  // carries the whole trackIds array plus `n` full tracks; n=200 inflates to
  // ~500 KB, so the old 512 KB hard cap (and srcLen*6 start) cut it off and a
  // truncated inflate reported failure. Start larger and allow up to 2 MB.
  size_t cap = std::max<size_t>(srcLen * 9, 16384);
  void* buf = alloc.allocate(cap);
  if (!buf) return false;
  for (int tries = 0; tries < 10; ++tries) {
    const size_t r = tinfl_decompress_mem_to_mem(buf, cap, src, srcLen, flags);
    if (r != static_cast<size_t>(-1)) {
      out = buf;
      outLen = r;
      return true;
    }
    const size_t nc = std::min<size_t>(cap * 2, 2 * 1024 * 1024);
    if (nc <= cap) break;
    void* grown = alloc.reallocate(buf, nc);
    if (!grown) break;
    buf = grown;
    cap = nc;
  }
  alloc.deallocate(buf);
  return false;
}

bool secureRequest(const char* method, const std::string& path,
                   const std::string& body, RawResponse& resp,
                   const uint32_t timeoutMs = CONNECT_TIMEOUT_MS,
                   const bool decode = true) {
  WiFiClientSecure client;
  client.setCACertBundle(
      _binary_x509_crt_bundle_start,
      static_cast<size_t>(_binary_x509_crt_bundle_end -
                          _binary_x509_crt_bundle_start));
  client.setTimeout(timeoutMs);
  if (!client.connect(IFACE_HOST, 443)) {
    LOG_ERR("OnlineMusic", "iface connect failed: %s", IFACE_HOST);
    return false;
  }

  // PC-client fingerprint, followed by the installed session (MUSIC_U/csrf).
  const std::string cookie = ifaceCookie();
  LOG_INF("OnlineMusic", "iface %s %s (jar entries=%u)", method, path,
          static_cast<unsigned>(g_jar.empty() ? 0 : 1));

  std::string request;
  request.reserve(path.size() + body.size() + 512);
  request += method;
  request += ' ' + path + " HTTP/1.1\r\n";
  request += "Host: " + std::string(IFACE_HOST) + "\r\n";
  request +=
      "User-Agent: Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Safari/537.36 Chrome/91.0.4472.164 "
      "NeteaseMusicDesktop/3.1.29.205117\r\n";
  request += "Accept: */*\r\n";
  request += "Accept-Encoding: deflate, gzip\r\n";
  request += "Cookie: " + cookie + "\r\n";
  if (!body.empty()) {
    request += "Content-Type: application/x-www-form-urlencoded\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  request += body;
  client.print(request.c_str());

  if (!collectResponseHeaders(client, resp, timeoutMs)) return false;
  if (!drainBody(client, resp)) return false;
  client.stop();

  // Always remove chunk framing (the caller of a raw body still needs a
  // contiguous stream); only the inflate is optional.
  if (resp.chunked && !dechunkBody(resp.body)) {
    LOG_ERR("OnlineMusic", "dechunk failed (%u bytes)",
            static_cast<unsigned>(resp.body.size()));
    return false;
  }
  resp.chunked = false;
  // decode==false: leave the (contiguous, compressed) body for a PSRAM decode.
  return decode ? inflateByMagic(resp) : true;
}

}  // namespace

bool apiGet(const std::string& path, std::string& body,
            const uint32_t timeoutMs = CONNECT_TIMEOUT_MS) {
  RawResponse resp;
  if (!secureRequest("GET", path, "", resp, timeoutMs)) {
    LOG_ERR("OnlineMusic", "GET failed: %s", path.c_str());
    return false;
  }
  if (resp.status != 200) {
    LOG_ERR("OnlineMusic", "status %d: %s", resp.status, path.c_str());
    return false;
  }
  body = std::move(resp.body);
  return true;
}

// Parse one [mm:ss.xx] / [mm:ss.xxx] stamp.
bool parseStamp(const std::string& stamp, uint32_t& outMs) {
  const size_t colon = stamp.find(':');
  if (colon == std::string::npos) return false;
  const long minutes = std::atol(stamp.substr(0, colon).c_str());
  std::string rest = stamp.substr(colon + 1);
  const size_t dot = rest.find('.');
  long seconds = 0;
  long fraction = 0;
  if (dot == std::string::npos) {
    seconds = std::atol(rest.c_str());
  } else {
    seconds = std::atol(rest.substr(0, dot).c_str());
    std::string frac = rest.substr(dot + 1);
    while (frac.size() < 3) frac += '0';
    if (frac.size() > 3) frac.resize(3);
    fraction = std::atol(frac.c_str());
  }
  if (minutes < 0 || seconds < 0 || seconds >= 60) return false;
  outMs = static_cast<uint32_t>(minutes * 60000 + seconds * 1000 + fraction);
  return true;
}

void parseLrcText(const std::string& text, TrackTags& tags) {
  std::vector<LrcLine> lines;

  size_t lineStart = 0;
  while (lineStart <= text.size()) {
    size_t eol = text.find('\n', lineStart);
    std::string line =
        text.substr(lineStart, eol == std::string::npos ? std::string::npos : eol - lineStart);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::vector<uint32_t> stamps;
    size_t p = 0;
    while (!line.empty() && p < line.size() && line[p] == '[') {
      const size_t close = line.find(']', p);
      if (close == std::string::npos) break;
      uint32_t ms = 0;
      if (parseStamp(line.substr(p + 1, close - p - 1), ms)) stamps.push_back(ms);
      p = close + 1;
    }

    const std::string body = line.substr(p);
    for (uint32_t ms : stamps) lines.push_back({ms, body});

    if (eol == std::string::npos) break;
    lineStart = eol + 1;
  }

  std::sort(lines.begin(), lines.end(),
            [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
  tags.synced = std::move(lines);
}

}  // namespace

// Defined later alongside the account/playlist loaders; needed by resolvePlayUrl.
bool apiGetWithRetries(const std::string& path, std::string& body,
                       int triesPerSession, int extraWifiSessions,
                       uint32_t attemptTimeoutMs, bool rawBody = false);

std::string urlEncode(const std::string& text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

bool search(const std::string& keyword, std::vector<OnlineTrack>& out) {
  out.clear();
  if (keyword.empty()) return false;

  // Fresh cache -> network -> stale cache.
  if (MusicCache::readSearch(keyword, out, /*requireFresh=*/true)) return true;

  const std::string path =
      "/api/search/get?s=" + urlEncode(keyword) + "&type=1&limit=30";

  std::string body;
  bool netOk = apiGet(path, body);
  if (netOk) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      LOG_ERR("OnlineMusic", "search json parse failed");
      netOk = false;
    } else {
      JsonArray songs = doc["result"]["songs"].as<JsonArray>();
      for (JsonObject song : songs) {
        if (out.size() >= MAX_RESULTS) break;

        OnlineTrack item;
        item.id = song["id"] | 0;
        item.title = song["name"] | "";
        item.durationMs = song["duration"] | 0;

        std::string artist;
        for (JsonObject a : song["artists"].as<JsonArray>()) {
          const char* name = a["name"];
          if (!name) continue;
          if (!artist.empty()) artist += "、";
          artist += name;
        }
        item.artist = artist;
        item.album = song["album"]["name"] | "";

        if (item.id != 0 && !item.title.empty()) out.push_back(std::move(item));
      }
    }
  }

  if (!out.empty()) {
    MusicCache::writeSearch(keyword, out);  // best effort
    return true;
  }
  // Network failed/empty: accept a stale result when one exists.
  return MusicCache::readSearch(keyword, out, /*requireFresh=*/false);
}

bool resolvePlayUrl(OnlineTrack& track) {
  const std::string ids = urlEncode("[" + std::to_string(track.id) + "]");

  std::string body;
  // Fail-fast retries with a short per-attempt header wait. A blackholed
  // connection to interface.music.163.com used to hang ~19 s on select before
  // giving up; a fresh connection with a 5 s wait gets the URL in ~1 s on
  // this hotspot.
  if (!apiGetWithRetries(
          "/api/song/enhance/player/url?ids=" + ids + "&br=128000", body,
          /*triesPerSession=*/3, /*extraWifiSessions=*/1, /*timeout=*/5000))
    return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject entry = doc["data"][0].as<JsonObject>();
  const char* url = entry["url"];
  if (!url) return false;  // copyrighted / VIP: not available
  track.playUrl = url;
  track.size = entry["size"] | 0;
  return true;
}

bool resolveCover(OnlineTrack& track) {
  if (!track.coverUrl.empty()) return true;
  const std::string ids = urlEncode("[" + std::to_string(track.id) + "]");

  std::string body;
  if (!apiGetWithRetries("/api/song/detail?ids=" + ids, body,
                         /*triesPerSession=*/2, /*extraWifiSessions=*/0,
                         /*timeout=*/5000))
    return false;

  JsonDocument detailDoc;
  if (deserializeJson(detailDoc, body)) return false;
  const char* pic = detailDoc["songs"][0]["album"]["picUrl"];
  if (pic) track.coverUrl = pic;
  return pic != nullptr;
}

bool parseLyricBody(const std::string& body, TrackTags& tags) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  const char* lrc = doc["lrc"]["lyric"];
  if (lrc) {
    parseLrcText(lrc, tags);
    if (!tags.synced.empty()) return true;
  }
  return false;
}

bool loadLyrics(const uint32_t id, TrackTags& tags) {
  if (MusicCache::readLyrics(id, tags)) return true;

  const std::string path = "/api/song/lyric?os=pc&id=" + std::to_string(id) +
                           "&lv=-1&kv=-1&tv=-1";
  std::string body;
  if (!apiGet(path, body)) return false;
  if (!parseLyricBody(body, tags)) return false;

  MusicCache::writeLyrics(id, body);  // raw body; best effort
  return true;
}

// --- Account binding ----------------------------------------------------------

// No-SMS, no-login self-test: runs the SAME encrypted-POST machinery against
// cloudsearch (the reference client uses it as an unauthenticated oracle).
// A body coming back proves the weapi POST channel works end to end; a stall
// localizes the fault to the request/edge rather than the login or captcha.
bool probeWeapiChannel() {
  const std::string kw = "\xE6\x99\xB4\xE5\xA4\xA9";  // "晴天" in UTF-8
  const std::string json = "{\"s\":\"" + kw + "\",\"type\":1,\"limit\":3}";

  std::string params, encSecKey;
  if (!WeapiCrypto::encrypt(json, params, encSecKey)) return false;
  const std::string form =
      "params=" + urlEncode(params) + "&encSecKey=" + encSecKey;

  RawResponse resp;
  WiFiClient client;
  const uint32_t t0 = millis();
  if (!sendRequestHeaders("POST", "/weapi/cloudsearch/get/web", form, resp,
                          client)) {
    client.stop();
    LOG_ERR("OnlineMusic", "PROBE header failed");
    return false;
  }
  const bool drained = drainBody(client, resp);
  client.stop();
  LOG_INF("OnlineMusic", "PROBE status=%d drained=%d %u ms body=%u: %.120s",
          resp.status, static_cast<int>(drained),
          static_cast<unsigned>(millis() - t0),
          static_cast<unsigned>(resp.body.size()), resp.body.c_str());
  return drained && resp.status == 200;
}

bool sendSmsCode(const std::string& phone) {
  const std::string path =
      "/api/sms/captcha/sent?cellphone=" + urlEncode(phone) + "&ctcode=86";
  std::string body;
  if (!apiGet(path, body)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    LOG_ERR("OnlineMusic", "sms sent json parse failed");
    return false;
  }
  const int code = doc["code"] | 0;
  const char* msg = doc["msg"];
  if (!msg) msg = doc["message"];
  if (code != 200) {
    LOG_ERR("OnlineMusic", "sms sent code=%d msg=%s body=%.120s", code,
            msg ? msg : "-", body.c_str());
  }
  return code == 200;
}

bool loginWithSmsCode(const std::string& phone, const std::string& captcha,
                      std::string& cookieOut, std::string& csrfOut) {
  // Values are digits only, so build the JSON directly.
  char json[160];
  std::snprintf(json, sizeof(json),
                "{\"phone\":\"%s\",\"countrycode\":\"86\",\"captcha\":\"%s\","
                "\"rememberLogin\":\"true\",\"csrf_token\":\"\"}",
                phone.c_str(), captcha.c_str());

  std::string params, encSecKey;
  if (!WeapiCrypto::encrypt(json, params, encSecKey)) {
    LOG_ERR("OnlineMusic", "weapi encrypt failed");
    return false;
  }
  const std::string form =
      "params=" + urlEncode(params) + "&encSecKey=" + encSecKey;

  RawResponse resp;
  if (!rawRequest("POST", "/weapi/login/cellphone", form, resp)) {
    LOG_ERR("OnlineMusic", "sms login transport failed");
    return false;
  }

  // Dump the raw verdict: distinguishes bad/expired captcha (502/512),
  // risk control / rate limit (503, -460), bad encryption (506/-460) etc.
  LOG_INF("OnlineMusic", "sms login http=%d body=%u: %.160s", resp.status,
          static_cast<unsigned>(resp.body.size()), resp.body.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, resp.body)) {
    LOG_ERR("OnlineMusic", "login json parse failed, body %u bytes",
            static_cast<unsigned>(resp.body.size()));
    return false;
  }
  const int code = doc["code"] | -1;
  const char* msg = doc["msg"];
  if (!msg) msg = doc["message"];
  if (!msg) msg = "-";
  LOG_INF("OnlineMusic", "sms login code=%d musicU=%u msg=%s", code,
          static_cast<unsigned>(resp.setMusicU.size()), msg);
  if (code != 200 || resp.setMusicU.empty()) return false;  // 503 bad code, etc.

  cookieOut = resp.setMusicU;
  csrfOut = resp.setCsrf;
  return true;
}

void setSession(const std::string& cookie, const std::string& csrf) {
  g_jar.set("MUSIC_U", cookie);
  if (!csrf.empty()) g_jar.set("__csrf", csrf);
}

void clearSession() {
  g_jar.clear();
}

std::string sessionCookieHeader() {
  return g_jar.header();
}

bool hasSession() {
  return g_jar.has("MUSIC_U");
}

// --- QR login over /weapi ----------------------------------------------------
// The full weapi flow was verified on a host to reach 803 with MUSIC_U. On
// the device, unikey/polls over the phone hotspot reach 801/802 but the
// confirm poll is risk-controlled with 8821 — the only difference is the
// mobile-carrier egress IP. The edge honors X-Real-IP/X-Forwarded-For, so we
// present a stable mainland client IP from the same 116.25-94.*.* pool the
// reference server uses.

// Pick one spoofed IP for the whole run (keep it stable across polls).
void ensureRealIp() {
  if (!g_realIp.empty()) return;
  char ip[24];
  std::snprintf(ip, sizeof(ip), "116.%u.%u.%u",
                25u + (esp_random() % 70), 1u + (esp_random() % 254),
                1u + (esp_random() % 254));
  g_realIp = ip;
}

// Encrypt `jsonBody` and POST it to `weapiPath`.
bool weapiPost(const std::string& weapiPath, const std::string& jsonBody,
               RawResponse& resp) {
  ensureRealIp();
  std::string params, encSecKey;
  if (!WeapiCrypto::encrypt(jsonBody, params, encSecKey)) {
    LOG_ERR("OnlineMusic", "weapi encrypt failed: %s", weapiPath.c_str());
    return false;
  }
  const std::string form =
      "params=" + urlEncode(params) + "&encSecKey=" + encSecKey;
  return rawRequest("POST", weapiPath, form, resp);
}

// --- QR login over eapi (official desktop-client channel) --------------------
// weapi QR login is risk-controlled (8821) from a carrier-egress IP, but the
// desktop client's eapi channel is not: real desktop apps log in over such
// networks. The request is a TLS POST carrying a PC-client fingerprint in the
// Cookie header; the reply is plain JSON (same 800/801/802/803 codes).
namespace {

constexpr const char* EAPI_HOST = "interfacepc.music.163.com";
constexpr const char* EAPI_UA =
    "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Safari/537.36 Chrome/91.0.4472.164 NeteaseMusicDesktop/3.1.29.205117";
constexpr const char* EAPI_OSVER =
    "Microsoft-Windows-10-Professional-build-19045-64bit";
constexpr const char* EAPI_APPVER = "3.1.17.204416";

std::string g_eapiDeviceId;

const std::string& eapiDeviceId() {
  if (g_eapiDeviceId.empty()) {
    static const char* kHex = "0123456789ABCDEF";
    for (int i = 0; i < 52; ++i) g_eapiDeviceId += kHex[esp_random() % 16];
  }
  return g_eapiDeviceId;
}

// Cookie header carrying the desktop-client fingerprint.
std::string eapiCookieHeader() {
  const uint64_t unixMs = static_cast<uint64_t>(time(nullptr)) * 1000;
  char requestId[32];
  std::snprintf(requestId, sizeof(requestId), "%llu_%04u",
                static_cast<unsigned long long>(unixMs), esp_random() % 10000);
  char c[440];
  std::snprintf(
      c, sizeof(c),
      "osver=%s; deviceId=%s; os=pc; appver=%s; versioncode=140; mobilename=; "
      "buildver=%llu; resolution=1920x1080; __csrf=; channel=netease; requestId=%s",
      EAPI_OSVER, eapiDeviceId().c_str(), EAPI_APPVER,
      static_cast<unsigned long long>(unixMs / 1000), requestId);
  return c;
}

// POST the eapi-encrypted body. `apiPath` is the signed /api/... path; the
// request itself goes to /eapi/<same suffix>, server identity verified via
// the embedded CA bundle.
bool eapiPost(const std::string& apiPath, const std::string& jsonBody,
              RawResponse& resp) {
  std::string params;
  if (!EapiCrypto::encrypt(apiPath, jsonBody, params)) {
    LOG_ERR("OnlineMusic", "eapi encrypt failed: %s", apiPath.c_str());
    return false;
  }
  const std::string form = "params=" + params;

  WiFiClientSecure client;
  client.setCACertBundle(
      _binary_x509_crt_bundle_start,
      static_cast<size_t>(_binary_x509_crt_bundle_end -
                          _binary_x509_crt_bundle_start));
  client.setTimeout(CONNECT_TIMEOUT_MS);
  if (!client.connect(EAPI_HOST, 443)) {
    LOG_ERR("OnlineMusic", "eapi connect failed: %s", EAPI_HOST);
    return false;
  }
  LOG_INF("OnlineMusic", "eapi connected to %s",
          client.remoteIP().toString().c_str());

  const std::string eapiPath = "/eapi/" + apiPath.substr(5);  // drop "/api"
  std::string request;
  request.reserve(form.size() + 512);
  request += "POST " + eapiPath + " HTTP/1.1\r\n";
  request += "Host: ";
  request += EAPI_HOST;
  request += "\r\n";
  request += "User-Agent: ";
  request += EAPI_UA;
  request += "\r\n";
  request += "Accept: */*\r\n";
  request += "Cookie: " + eapiCookieHeader() + "\r\n";
  request += "Content-Type: application/x-www-form-urlencoded\r\n";
  request += "Content-Length: " + std::to_string(form.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += form;
  client.print(request.c_str());

  if (!collectResponseHeaders(client, resp, CONNECT_TIMEOUT_MS)) return false;
  if (!drainBody(client, resp)) return false;
  client.stop();

  return decodeBodyByMagic(resp);
}

}  // namespace

bool startQrLogin(QrLoginStart& out) {
  RawResponse resp;
  if (!eapiPost("/api/login/qrcode/unikey", "{\"type\":3}", resp)) {
    LOG_ERR("OnlineMusic", "unikey transport failed");
    return false;
  }
  const std::string& body = resp.body;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    LOG_ERR("OnlineMusic", "unikey json: %s, body %u bytes", jerr.c_str(),
            static_cast<unsigned>(body.size()));
    return false;
  }
  const char* key = doc["unikey"];
  if ((doc["code"] | 0) != 200 || !key) {
    LOG_ERR("OnlineMusic", "unikey code=%d head: %.200s", doc["code"] | -1,
            body.c_str());
    return false;
  }

  out.unikey = key;
  out.qrUrl = std::string("https://music.163.com/login?codekey=") + key;
  return true;
}

QrPollCode pollQrLogin(const std::string& unikey, std::string& cookieOut,
                       std::string& csrfOut) {
  // unikey only contains hex chars and dashes, safe inside a JSON string.
  char json[128];
  std::snprintf(json, sizeof(json), "{\"key\":\"%s\",\"type\":3}",
                unikey.c_str());
  RawResponse resp;
  if (!eapiPost("/api/login/qrcode/client/login", json, resp)) {
    return QrPollCode::Error;
  }

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, resp.body);
  if (jerr) {
    LOG_ERR("OnlineMusic", "poll json: %s, body %u bytes", jerr.c_str(),
            static_cast<unsigned>(resp.body.size()));
    return QrPollCode::Error;
  }
  const int code = doc["code"] | -1;
  LOG_INF("OnlineMusic", "poll code=%d musicU=%u csrf=%u", code,
          static_cast<unsigned>(resp.setMusicU.size()),
          static_cast<unsigned>(resp.setCsrf.size()));
  switch (code) {
    case 800:
      return QrPollCode::Expired;
    case 801:
      return QrPollCode::Waiting;
    case 802:
      return QrPollCode::Scanned;
    case 803:
      if (!resp.setMusicU.empty()) {
        cookieOut = resp.setMusicU;
        csrfOut = resp.setCsrf;
        return QrPollCode::Confirmed;
      }
      LOG_ERR("OnlineMusic", "803 no cookie (%u b): %.80s",
              static_cast<unsigned>(resp.body.size()), resp.body.c_str());
      return QrPollCode::Error;
    default:
      // Keep the dump short: the USB-Serial-JTAG TX FIFO drops long bursts.
      LOG_ERR("OnlineMusic", "code=%d (%u b): %.80s", code,
              static_cast<unsigned>(resp.body.size()), resp.body.c_str());
      return QrPollCode::Error;
  }
}

bool getAccount(AccountInfo& out) {
  std::string body;
  // The profile lookup gates the uid that userPlaylists needs; a single
  // header timeout would skip the (well-retried) playlist fetch entirely.
  // Use the retry envelope so one flaky music.163.com response is not fatal.
  if (!apiGetWithRetries("/api/nuser/account/get", body, 3, 1, 6000))
    return false;

  JsonDocument doc;
  const DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    LOG_ERR("OnlineMusic", "account json: %s, body %u bytes", jerr.c_str(),
            static_cast<unsigned>(body.size()));
    return false;
  }
  if ((doc["code"] | 0) != 200) {
    LOG_ERR("OnlineMusic", "account code=%d body head: %.200s", doc["code"] | -1, body.c_str());
    return false;
  }
  JsonObject account = doc["account"].as<JsonObject>();
  if (account.isNull()) {
    // 200 with no account: the session was not accepted (cookie missing/
    // invalid). Log it — otherwise this silent failure looks like a network
    // error with no OnlineMusic output.
    LOG_ERR("OnlineMusic", "account/get: code=200 but account null (%.160s)",
            body.c_str());
    return false;
  }
  out.userId = account["id"] | static_cast<uint64_t>(0);
  const char* nickname = doc["profile"]["nickname"];
  if (nickname) out.nickname =nickname;
  LOG_INF("OnlineMusic", "account/get -> uid=%llu",
          static_cast<unsigned long long>(out.userId));
  return out.userId != 0;
}

bool reassociateWifi();

// GET with the transport-retry envelope. A blackholed hotspot connection never
// recovers: TCP stays established but no bytes ever arrive, so each attempt
// opens a fresh connection with a short header wait. Attempts are spaced with a
// growing backoff to let the link/NAT settle, and after one full session the
// WiFi association is reset and the session is repeated extraWifiSessions
// times. Used for both the playlist list and the (larger) playlist detail.
bool apiGetWithRetries(const std::string& path, std::string& body,
                       const int triesPerSession,
                       const int extraWifiSessions,
                       const uint32_t attemptTimeoutMs,
                       const bool rawBody) {
  static const uint32_t kBackoffMs[] = {300, 800, 1500, 2500};
  const int totalSessions = 1 + std::max(0, extraWifiSessions);
  for (int session = 0; session < totalSessions; ++session) {
    if (session > 0 && !reassociateWifi()) break;
    for (int attempt = 0; attempt < triesPerSession; ++attempt) {
      bool ok;
      if (rawBody) {
        // Keep the compressed, de-chunked body for a PSRAM decode elsewhere.
        RawResponse rr;
        ok = secureRequest("GET", path, "", rr, attemptTimeoutMs,
                           /*decode=*/false) &&
             rr.status == 200;
        if (ok) body = std::move(rr.body);
      } else {
        ok = apiGet(path, body, attemptTimeoutMs);
      }
      if (ok) return true;
      LOG_INF("OnlineMusic", "GET retry %d/%d session %d: %s", attempt + 1,
              triesPerSession, session + 1, path.c_str());
      if (attempt + 1 < triesPerSession) {
        const size_t bi = std::min<size_t>(
            attempt, sizeof(kBackoffMs) / sizeof(kBackoffMs[0]) - 1);
        delay(kBackoffMs[bi]);
      }
    }
  }
  return false;
}

bool userPlaylists(const uint64_t uid, std::vector<PlaylistInfo>& out) {
  out.clear();
  // Fresh cache -> network -> stale cache (offline).
  if (MusicCache::readUserPlaylists(uid, out, /*requireFresh=*/true)) return true;

  bool netOk = true;

  // Page the list, 10 per request. A limit=30 reply is ~8.3KB gzip and on the
  // phone hotspot every transfer dies a few bytes from the end (a window/
  // retransmit deadlock); limit=10 replies are ~2.5KB and come through whole.
  // Stop at a short page, and cap the page count regardless.
  constexpr int PAGE_SIZE = 10;
  constexpr int MAX_PAGES = 6;  // up to 60 playlists
  for (int page = 0; page < MAX_PAGES && netOk; ++page) {
    std::string body;
    const std::string path = "/api/user/playlist?uid=" + std::to_string(uid) +
                             "&limit=" + std::to_string(PAGE_SIZE) +
                             "&offset=" + std::to_string(page * PAGE_SIZE);
    if (!apiGetWithRetries(path, body, 3, 1, 6000)) {
      netOk = false;
      break;
    }

    // Only the three fields we render are kept; the full per-playlist detail
    // (creator objects, cover sets) is discarded while parsing.
    JsonDocument filter;
    {
      JsonObject f0 = filter["playlist"][0].to<JsonObject>();
      f0["id"] = true;
      f0["name"] = true;
      f0["trackCount"] = true;
    }

    JsonDocument doc;
    const DeserializationError jerr =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (jerr) {
      LOG_ERR("OnlineMusic", "playlists json: %s, body %u bytes", jerr.c_str(),
              static_cast<unsigned>(body.size()));
      netOk = false;
      break;
    }

    const JsonArray pageItems = doc["playlist"].as<JsonArray>();
    for (JsonObject pl : pageItems) {
      PlaylistInfo info;
      info.id = pl["id"] | static_cast<uint64_t>(0);
      info.name = pl["name"] | "";
      info.trackCount = pl["trackCount"] | 0;
      if (info.id == 0 || info.name.empty()) continue;
      // Defensive: a shifting offset between pages must not duplicate a row.
      const bool seen = std::any_of(out.begin(), out.end(),
          [&](const PlaylistInfo& p) { return p.id == info.id; });
      if (!seen) out.push_back(std::move(info));
    }

    const int got = static_cast<int>(pageItems.size());
    LOG_INF("OnlineMusic", "playlist page %d: %d items (%u total)", page + 1,
            got, static_cast<unsigned>(out.size()));
    if (got < PAGE_SIZE) break;  // last page
  }

  if (!out.empty()) {
    LOG_INF("OnlineMusic", "userPlaylists -> %d", static_cast<int>(out.size()));
    MusicCache::writeUserPlaylists(uid, out);  // best effort
    return true;
  }
  // Nothing via network: accept a stale list when cached.
  return MusicCache::readUserPlaylists(uid, out, /*requireFresh=*/false);
}

// Drop and re-establish the WiFi association. After many flash-driven
// reconnects the phone's association/NAT state can go bad: the origin sends one
// receive window and never gets a window-update ACK back, so every larger body
// stalls forever. A fresh association (and NAT mapping) restores it without the
// user touching the hotspot.
bool reassociateWifi() {
  LOG_INF("OnlineMusic", "forcing WiFi reassociation (RSSI %d)", WiFi.RSSI());
  WiFi.reconnect();
  const uint32_t to = millis() + 20000;
  while (static_cast<int32_t>(millis() - to) < 0) {
    if (WiFi.status() == WL_CONNECTED) {
      delay(400);  // let data-path / ARP settle
      return true;
    }
    delay(200);
  }
  return false;
}

bool playlistTracks(const uint64_t playlistId, std::vector<OnlineTrack>& out) {
  out.clear();
  // Fresh cache -> network -> stale cache (offline).
  if (MusicCache::readPlaylist(playlistId, out, /*requireFresh=*/true)) return true;

  const bool netOk = [&] {
  // One v6 request. Its body embeds the playlist's ENTIRE id list (trackIds —
  // ~232 KB for 1314 tracks), so fetch the compressed body and decode + parse
  // in PSRAM (8 MB) instead of the scarce ~270 KB internal pool. `n` caps how
  // many FULL tracks the server populates; that is the window we surface.
  constexpr int WINDOW = 200;
  const std::string path = "/api/v6/playlist/detail?id=" +
                           std::to_string(playlistId) +
                           "&n=" + std::to_string(WINDOW) + "&s=0";

  std::string compressed;
  if (!apiGetWithRetries(path, compressed, /*triesPerSession=*/4,
                         /*extraWifiSessions=*/2, /*timeout=*/8000,
                         /*rawBody=*/true)) {
    LOG_ERR("OnlineMusic", "playlist open failed after retries");
    return false;
  }

  PoolAllocator alloc;

  // Reconstruct a RawResponse so decodeToBuffer can sniff the framing. The
  // transport only handed back the body; derive the encoding from its magic.
  RawResponse raw;
  raw.body = std::move(compressed);
  if (raw.body.size() >= 2 &&
      static_cast<uint8_t>(raw.body[0]) == 0x1f &&
      static_cast<uint8_t>(raw.body[1]) == 0x8b) {
    raw.contentEncoding = "gzip";
  } else if (static_cast<uint8_t>(raw.body[0]) == 0x78) {
    raw.contentEncoding = "deflate";
  }

  void* plain = nullptr;
  size_t plainLen = 0;
  if (!decodeToBuffer(raw, alloc, plain, plainLen)) {
    LOG_ERR("OnlineMusic", "playlist decode failed (%u compressed)",
            static_cast<unsigned>(raw.body.size()));
    return false;
  }

  // Keep only the rendered fields; v6 full tracks use the SHORT names
  // dt/ar/al. The JSON tree itself lives in PSRAM via PoolAllocator.
  JsonDocument filter;
  {
    JsonObject ft = filter["playlist"]["tracks"][0].to<JsonObject>();
    ft["id"] = true;
    ft["name"] = true;
    ft["dt"] = true;
    ft["ar"][0]["name"] = true;
    ft["al"]["name"] = true;
    // picUrl deliberately omitted: 200 long image URLs in the internal heap
    // (~15-20 KB) fragmented it enough that mbedTLS could not get a contiguous
    // handshake block (-32512). The cover for the track actually playing is
    // resolved after audio starts via resolveCover().
  }

  JsonDocument doc(&alloc);
  MemStream ms(plain, plainLen);
  const DeserializationError jerr =
      deserializeJson(doc, ms, DeserializationOption::Filter(filter));
  alloc.deallocate(plain);
  if (jerr) {
    LOG_ERR("OnlineMusic", "playlist json: %s, plain %u bytes", jerr.c_str(),
            static_cast<unsigned>(plainLen));
    return false;
  }

  const JsonArray songs = doc["playlist"]["tracks"].as<JsonArray>();
  for (JsonObject song : songs) {
    if (out.size() >= MAX_RESULTS) break;

    OnlineTrack item;
    item.id = song["id"] | 0;
    item.title = song["name"] | "";
    item.durationMs = song["dt"] | 0;

    std::string artist;
    for (JsonObject a : song["ar"].as<JsonArray>()) {
      const char* name = a["name"];
      if (!name) continue;
      if (!artist.empty()) artist += "、";
      artist += name;
    }
    item.artist = artist;
    item.album = song["al"]["name"] | "";

    if (item.id != 0 && !item.title.empty()) out.push_back(std::move(item));
  }
  LOG_INF("OnlineMusic", "playlistTracks -> %d", static_cast<int>(out.size()));
  return true;
  }();

  if (netOk && !out.empty()) {
    MusicCache::writePlaylist(playlistId, out);  // best effort
    return true;
  }
  // Network failed: fall back to a stale cached track list when present.
  return MusicCache::readPlaylist(playlistId, out, /*requireFresh=*/false);
}

}  // namespace OnlineMusicApi
