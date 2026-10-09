#include "HttpByteStream.h"

#include <HalStorage.h>
#include <Logging.h>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Resolve host to its IPv4 addresses only. The Arduino core's hostByName()
// prefers AAAA as soon as the interface has a global IPv6 and never falls back
// to IPv4 (see the "Workaround" in NetworkManager::hostByName). The China
// Mobile link assigns a global IPv6 via SLAAC but has no upstream IPv6 route,
// so a by-hostname connect fails outright while IPv4 works fine. Force
// AF_INET here and connect by IPAddress to sidestep that broken v6 path.
bool resolveIPv4(const std::string& host, std::vector<uint32_t>& addrs) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (lwip_getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || res == nullptr) {
    return false;
  }
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
      addrs.push_back(sin->sin_addr.s_addr);
    }
  }
  lwip_freeaddrinfo(res);
  return !addrs.empty();
}

// Minimal DNS-over-UDP (Do53) query to an explicit resolver, returning A
// records. The hotspot's forwarded resolver sometimes hands the device generic,
// unreachable NetEase origin IPs (123.58.180.x) instead of the local reachable
// edges, so we ask AliDNS/DNSPod directly: they geo-locate by this query's
// source IP and return the nearby POP (111.47.223.x on this network).
bool dnsResolveUdp(const uint32_t serverIp, const std::string& host,
                   std::vector<uint32_t>& addrs,
                   const uint32_t ecsIp = 0, const int ecsPrefix = 0) {
  uint8_t query[512];
  size_t n = 0;
  auto put16 = [&](uint16_t v) { query[n++] = v >> 8; query[n++] = v & 0xFF; };
  put16(0x1234);            // id
  put16(0x0100);            // flags: RD
  put16(1);                 // qdcount
  put16(0); put16(0); put16(ecsPrefix > 0 ? 1 : 0);  // an/ns/ar
  size_t start = 0;
  while (true) {
    const size_t dot = host.find('.', start);
    const std::string label = host.substr(start, dot == std::string::npos
                                                     ? std::string::npos
                                                     : dot - start);
    query[n++] = static_cast<uint8_t>(label.size());
    for (char c : label) query[n++] = static_cast<uint8_t>(c);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  query[n++] = 0;
  put16(1);  // type A
  put16(1);  // class IN

  // Optional EDNS(0) Client Subnet (option 8) in the additional section.
  if (ecsPrefix > 0) {
    const int addrBytes = (ecsPrefix + 7) / 8;
    const size_t optRdataLen = 4 + addrBytes;  // option code/len + ECS fields
    query[n++] = 0;                 // root name
    put16(41);                      // OPT
    put16(1232);                    // UDP payload size
    put16(0); put16(0);             // ext rcode/version + flags
    put16(optRdataLen + 4);         // RDLENGTH (option header 4 + payload)
    put16(8);                       // option code: ECS
    put16(optRdataLen);             // option length
    put16(1);                       // address family IPv4
    query[n++] = static_cast<uint8_t>(ecsPrefix);  // source prefix
    query[n++] = 0;                                // scope prefix
    uint8_t octets[4];
    memcpy(octets, &ecsIp, 4);  // network order
    for (int b = 0; b < addrBytes; ++b) query[n++] = octets[b];
  }

  const int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  const timeval tv = {2, 0};
  lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = serverIp;

  bool ok = false;
  if (lwip_sendto(fd, query, n, 0, reinterpret_cast<sockaddr*>(&to),
                  sizeof(to)) == static_cast<ssize_t>(n)) {
    uint8_t resp[1024];
    const int rlen = lwip_recv(fd, resp, sizeof(resp), 0);
    if (rlen >= 12 && (resp[3] & 0x0F) == 0 && (resp[2] & 0x80)) {
      const uint16_t ancount = (resp[6] << 8) | resp[7];
      size_t i = 12;
      // Skip question section.
      while (i < static_cast<size_t>(rlen) && resp[i] != 0) i += resp[i] + 1;
      i += 5;
      for (uint16_t a = 0; a < ancount && i + 10 <= static_cast<size_t>(rlen);
           ++a) {
        if ((resp[i] & 0xC0) == 0xC0) i += 2;
        else {
          while (i < static_cast<size_t>(rlen) && resp[i] != 0) i += resp[i] + 1;
          ++i;
        }
        const uint16_t rtype = (resp[i] << 8) | resp[i + 1];
        const uint16_t rdlen = (resp[i + 8] << 8) | resp[i + 9];
        i += 10;
        if (rtype == 1 && rdlen == 4 && i + 4 <= static_cast<size_t>(rlen)) {
          uint32_t ip;
          memcpy(&ip, resp + i, 4);
          addrs.push_back(ip);
          ok = true;
        }
        i += rdlen;
      }
    }
  }
  lwip_close(fd);
  return ok;
}

// Verified-HTTPS GET returning the plain-text body. Used by localEcsSubnet()
// to learn the egress IP on port 443 (the same connection kind DoH uses),
// which can differ from its port-80 egress under per-flow NAT.
std::string httpsGetText(const std::string& url) {
  std::string body;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.user_agent = "curl/8";
  config.user_data = &body;
  config.event_handler = [](esp_http_client_event_t* evt) -> esp_err_t {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr &&
        evt->data_len > 0) {
      static_cast<std::string*>(evt->user_data)
          ->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return "(init fail)";
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200) return "(fail)";
  return body;
}

// Raw DoH GET to AliDNS; returns the response body ("" on failure). Verified
// TLS to 443 cannot be transparently redirected. When ecsSubnet (e.g.
// "223.104.122.0/24") is given it is forwarded as EDNS Client Subnet, which
// forces the geo CDN (kunlun/jiasu) to return the local POP instead of the
// unreachable origin fallback the bare query gets from this device.
std::string dohQueryBody(const std::string& host,
                         const std::string& ecsSubnet = "") {
  // The CDN hostname is [a-z0-9.], so it needs no percent-encoding.
  // Connect to the resolver by IP literal: resolving dns.alidns.com via the
  // system DNS lands on a different Alibaba DoH frontend that ignores ECS and
  // returns the unreachable origin IP. 223.5.5.5 needs no DNS and its cert
  // lists that IP in the SAN, so verification proves we reached real AliDNS.
  std::string url =
      "https://223.5.5.5/resolve?name=" + host + "&type=A";
  if (!ecsSubnet.empty()) {
    // Percent-encode the "/" in "a.b.c.0/24". A raw slash in the query confuses
    // esp_http_client's URL parser (the subnet is dropped and the request goes
    // out as a bare query, which gets the unreachable origin fallback).
    std::string encoded = ecsSubnet;
    const size_t slash = encoded.find('/');
    if (slash != std::string::npos) encoded.replace(slash, 1, "%2F");
    url += "&edns_client_subnet=" + encoded;
  }
  LOG_INF("OnlineMusic", "DoH url: %s", url.c_str());
  std::string body;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.user_agent = "curl/8";
  config.user_data = &body;
  config.event_handler = [](esp_http_client_event_t* evt) -> esp_err_t {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr &&
        evt->data_len > 0) {
      static_cast<std::string*>(evt->user_data)
          ->append(static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return "";
  esp_http_client_set_header(client, "Host", "dns.alidns.com");
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200) {
    LOG_INF("OnlineMusic", "DoH failed err=%s status=%d",
            esp_err_to_name(err), status);
    return "";
  }
  return body;
}

// Parse A records out of a JSON DoH body.
bool parseDoHAnswers(const std::string& body, std::vector<uint32_t>& addrs) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray answers = doc["Answer"].as<JsonArray>();
  for (JsonVariant answer : answers) {
    if ((answer["type"] | 0) != 1) continue;
    const char* data = answer["data"];
    if (data == nullptr) continue;
    IPAddress ip;
    if (ip.fromString(data)) addrs.push_back(static_cast<uint32_t>(ip));
  }
  return !addrs.empty();
}

bool resolveDoH(const std::string& host, std::vector<uint32_t>& addrs,
                const std::string& ecsSubnet = "") {
  const std::string body = dohQueryBody(host, ecsSubnet);
  if (body.empty()) return false;
  return parseDoHAnswers(body, addrs);
}

// Learn this device's public IPv4 and return its /24 as an ECS subnet string
// ("a.b.c.0/24"); "" if it couldn't be determined. Use verified-HTTPS echo
// endpoints: the port-80 path is flaky on this link (often returns no body),
// whereas the 443 echos succeed consistently.
std::string localEcsSubnet() {
  const char* kIpUrls[] = {"https://ipinfo.io/ip", "https://ip.3322.net/"};
  std::string ip;
  for (const char* u : kIpUrls) {
    ip = httpsGetText(u);
    while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' ||
                          ip.back() == ' ')) ip.pop_back();
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a > 0) {
      char buf[40];
      snprintf(buf, sizeof(buf), "%u.%u.%u.0/24", a, b, c);
      return buf;
    }
  }
  return "";
}

constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
// Mid-playback stalls past this never recover on this hotspot, so reconnect
// quickly with a fresh connection (edges are cached, so a reconnect is just a
// TCP+Range) rather than waiting 15s and draining the audio buffer.
constexpr uint32_t STALL_MS = 5000;
constexpr int MAX_RECONNECTS = 6;

// Read-ahead ring sizing. 128 kbps audio is ~16 KB/s; a whole song is usually
// 3-5 MB. We size the ring to hold the WHOLE song when it fits, so once it is
// downloaded the rest of playback cannot stutter no matter what the link does;
// otherwise we use a large fixed ring. The cap leaves comfortable PSRAM
// headroom for image buffers while reading (EPUB / WeRead) with audio running.
constexpr size_t RING_MAX = 4 * 1024 * 1024;
// PSRAM kept free for fonts/BLE and reader image buffers while audio plays.
constexpr size_t RING_PSRAM_RESERVE = 1536 * 1024;
// Below this usable PSRAM budget, don't bother with PSRAM: use internal RAM.
constexpr size_t RING_MIN_PSRAM_BUDGET = 96 * 1024;
// If PSRAM is unavailable, a smaller internal-RAM ring (~4 s).
constexpr size_t RING_FALLBACK_INTERNAL = 64 * 1024;
constexpr size_t PRODUCER_CHUNK = 2048;
constexpr UBaseType_t PRODUCER_PRIO = 6;
constexpr uint32_t PRODUCER_STACK = 6144;
constexpr BaseType_t PRODUCER_CORE = 1;
constexpr uint32_t PRODUCER_JOIN_MS = 4000;

// Choose the read-ahead ring size in bytes (0 => use the internal-RAM
// fallback). Prefers holding the whole file (+1 byte so the producer can
// still make one call that discovers clean EOF after a full ring instead of
// the consumer blocking), clipped to the largest contiguous free block, a
// reserve, and RING_MAX. Never returns a size EXACTLY equal to fileSize (a
// ring exactly the file length would fill up before EOF is reported, hanging
// the consumer at song end): it is either whole-file+1 or strictly smaller.
size_t chooseRingSize(const uint64_t fileSize) {
  const size_t freeBytes =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const size_t largestBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (freeBytes <= RING_PSRAM_RESERVE) return 0;
  size_t budget = std::min(freeBytes - RING_PSRAM_RESERVE, largestBlock);
  budget = std::min(budget, RING_MAX);
  if (budget < RING_MIN_PSRAM_BUDGET) return 0;

  if (fileSize > 0) {
    const uint64_t whole = fileSize + 1;
    if (whole <= budget) return static_cast<size_t>(whole);  // whole song
    if (budget == fileSize) return static_cast<size_t>(fileSize - 1);
  }
  return budget;
}

std::string lowerToken(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  s = s.substr(start);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// After a "Content-Range: bytes a-b/total" header value, fill total.
bool parseTotalFromRange(const std::string& value, uint64_t& total) {
  const size_t slash = value.find_last_of('/');
  if (slash == std::string::npos) return false;
  const std::string tail = value.substr(slash + 1);
  if (tail == "*") return false;
  total = std::strtoull(tail.c_str(), nullptr, 10);
  return true;
}

}  // namespace

HttpByteStream::~HttpByteStream() {
  close();
}

bool HttpByteStream::parseUrl(const std::string& url, UrlParts& out) {
  constexpr const char* kScheme = "http://";
  if (url.rfind(kScheme, 0) != 0) return false;  // https not supported
  // "http://" is 7 chars: the host begins at index 7. Using 8 here silently
  // dropped the first host character (the "m" in m804.music.126.net), which
  // sent the stream to the unreachable origin 123.58 instead of the CDN.
  const size_t slash = url.find('/', 7);
  const std::string hostPort =
      url.substr(7, slash == std::string::npos ? std::string::npos : slash - 7);
  out.path = slash == std::string::npos ? "/" : url.substr(slash);

  const size_t colon = hostPort.find(':');
  if (colon == std::string::npos) {
    out.host = hostPort;
    out.port = 80;
  } else {
    out.host = hostPort.substr(0, colon);
    out.port = static_cast<uint16_t>(std::atoi(hostPort.substr(colon + 1).c_str()));
  }
  return !out.host.empty();
}

void HttpByteStream::close() {
  stopProducer();
  disableTee();  // producer joined: safe to close/remove the staging file
  if (producerDone_) {
    vSemaphoreDelete(producerDone_);
    producerDone_ = nullptr;
  }
  if (dataReady_) {
    vSemaphoreDelete(dataReady_);
    dataReady_ = nullptr;
  }
  if (freeSpace_) {
    vSemaphoreDelete(freeSpace_);
    freeSpace_ = nullptr;
  }
  if (ringMutex_) {
    vSemaphoreDelete(ringMutex_);
    ringMutex_ = nullptr;
  }
  if (ring_) {
    heap_caps_free(ring_);
    ring_ = nullptr;
  }
  ringCap_ = 0;
  client_.stop();
  open_ = false;
}

bool HttpByteStream::open(const std::string& url) {
  close();
  if (!parseUrl(url, url_)) {
    LOG_ERR("OnlineMusic", "bad url: %s", url.c_str());
    return false;
  }
  position_ = 0;
  totalSize_ = 0;
  stopRequested_.store(false);  // close()/a prior producer may have left it set
  // DNS/edge caching and the first connection happen on the caller's task
  // before the producer exists, so the producer never runs the DoH/ECS path.
  if (!connectRange(0)) return false;
  open_ = true;
  // Size the ring to the whole file when it fits (totalSize_ known now).
  if (!initRingAndLocks(chooseRingSize(totalSize_))) {
    close();
    return false;
  }
  fetchPos_ = position_;  // effective body start (normally 0)
  if (!startProducer()) {
    close();
    return false;
  }
  return true;
}

bool HttpByteStream::seekSet(const uint64_t position) {
  if (!open_) return false;
  // The consumer (audio task) is always joined before a seek, so no one is
  // blocked on the ring while we tear the producer down and reconnect.
  stopProducer();
  // Persist whatever the tee has staged before the producer restarts elsewhere.
  // teeWrite() later decides whether the new byte range stays contiguous.
  if (teeEnabled_ && !teeAborted_ && teeFileOpen_) teeFlushStaging();
  // Clear the stop flag BEFORE connectRange(): its waits abort on this flag,
  // and with the old producer gone a main-task reconnect must run to
  // completion (otherwise the header wait returns false and the seek fails).
  stopRequested_.store(false);
  position_ = position;
  if (!connectRange(position)) return false;
  fetchPos_ = position_;  // effective start (206: position; 200 fallback: 0)
  resetRingLockedState();
  return startProducer();
}

bool HttpByteStream::connectRange(const uint64_t from) {
  client_.stop();
  abortableDelay(2);

  // Resolve the host's edge IPs once and cache them. The FAST path uses the
  // explicit UDP resolvers (AliDNS/DNSPod by IP, cannot be transparently
  // redirected) plus the system resolver: a few milliseconds. Only when no
  // candidate can be connected do we fall back to DoH-over-HTTPS, which is a
  // full TLS round trip (~seconds) but proves tamper-proof DNS. Re-resolution
  // on mid-playback reconnects never re-runs: the cached set is reused so a
  // reconnect is just a TCP connect.
  static std::string cachedHost;
  static std::vector<uint32_t> edges;
  static bool dohTriedForHost = false;
  if (cachedHost != url_.host) {
    edges.clear();
    dohTriedForHost = false;
    std::vector<uint32_t> edgeAddrs;

    const uint32_t ALI_DNS = IPAddress(223, 5, 5, 5);
    const uint32_t DNSPOD = IPAddress(119, 29, 29, 29);
    dnsResolveUdp(ALI_DNS, url_.host, edgeAddrs);
    std::vector<uint32_t> podAddrs;
    if (dnsResolveUdp(DNSPOD, url_.host, podAddrs)) {
      for (uint32_t a : podAddrs) edgeAddrs.push_back(a);
    }
    std::vector<uint32_t> sysAddrs;
    if (resolveIPv4(url_.host, sysAddrs)) {
      for (uint32_t a : sysAddrs) edgeAddrs.push_back(a);
    }
    for (uint32_t a : edgeAddrs) {
      bool seen = false;
      for (uint32_t b : edges) {
        if (a == b) { seen = true; break; }
      }
      if (!seen) edges.push_back(a);
    }
    if (edges.empty()) {
      // No answer from any UDP/system path: go straight to DoH (with a cached
      // ECS subnet so the CDN returns the local POP). The ECS HTTPS discovery
      // is paid only here, in the rare fallback.
      dohTriedForHost = true;
      static std::string ecsSubnet;
      if (ecsSubnet.empty()) ecsSubnet = localEcsSubnet();
      std::vector<uint32_t> dohAddrs;
      if (resolveDoH(url_.host, dohAddrs, ecsSubnet)) {
        LOG_INF("OnlineMusic", "DoH gave %u edges",
                static_cast<unsigned>(dohAddrs.size()));
        for (uint32_t a : dohAddrs) edges.push_back(a);
      }
    }
    if (edges.empty()) {
      LOG_ERR("OnlineMusic", "all DNS paths failed: %s", url_.host.c_str());
      return false;
    }
    cachedHost = url_.host;
  }

  LOG_INF("OnlineMusic", "%u edge candidates, first=%s",
          static_cast<unsigned>(edges.size()),
          IPAddress(edges[0]).toString().c_str());

  constexpr int CONNECT_ATTEMPTS = 8;
  constexpr uint32_t CONNECT_BACKOFF_MS = 200;
  bool connected = false;
  size_t edge = 0;
  for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; ++attempt) {
    const IPAddress ip(edges[edge % edges.size()]);
    ++edge;
    if (client_.connect(ip, url_.port)) {
      connected = true;
      break;
    }
    LOG_INF("OnlineMusic", "connect attempt %d/%d failed: %s", attempt,
            CONNECT_ATTEMPTS, ip.toString().c_str());
    if (attempt < CONNECT_ATTEMPTS) {
      client_.stop();
      abortableDelay(CONNECT_BACKOFF_MS);
    }
  }
  // UDP resolvers answered but every edge refused us (DNS hijack / dead
  // POP): one verified-DoH retry, then give up.
  if (!connected && !dohTriedForHost) {
    dohTriedForHost = true;
    static std::string ecsSubnet;
    if (ecsSubnet.empty()) ecsSubnet = localEcsSubnet();
    std::vector<uint32_t> dohAddrs;
    if (resolveDoH(url_.host, dohAddrs, ecsSubnet)) {
      LOG_INF("OnlineMusic", "fallback DoH gave %u edges",
              static_cast<unsigned>(dohAddrs.size()));
      for (uint32_t a : dohAddrs) {
        bool seen = false;
        for (uint32_t b : edges) {
          if (a == b) { seen = true; break; }
        }
        if (!seen) edges.push_back(a);
      }
      for (size_t k = 0; k < edges.size(); ++k) {
        const IPAddress ip(edges[k]);
        if (client_.connect(ip, url_.port)) {
          connected = true;
          break;
        }
        LOG_INF("OnlineMusic", "post-DoH connect failed: %s",
                ip.toString().c_str());
        client_.stop();
      }
    }
  }
  if (!connected) {
    LOG_ERR("OnlineMusic", "connect failed: %s", url_.host.c_str());
    return false;
  }
  client_.setTimeout(CONNECT_TIMEOUT_MS);

  std::string request = "GET " + url_.path + " HTTP/1.1\r\n";
  request += "Host: " + url_.host + "\r\n";
  request += "User-Agent: CrossPointEreader/1.0\r\n";
  request += "Accept: */*\r\n";
  request += "Connection: close\r\n";
  if (from > 0) {
    request += "Range: bytes=" + std::to_string(from) + "-\r\n";
  }
  request += "\r\n";
  client_.print(request.c_str());

  // Receive the header block.
  std::string headers;
  const uint32_t deadline = millis() + CONNECT_TIMEOUT_MS;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    if (stopRequested_.load()) return false;
    while (client_.available()) {
      headers += static_cast<char>(client_.read());
      if (headers.size() >= 4 && headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
        return parseHeaders(headers, from);
      }
    }
    if (!client_.connected()) break;
    abortableDelay(2);
  }
  LOG_ERR("OnlineMusic", "header timeout");
  return false;
}

bool HttpByteStream::parseHeaders(const std::string& headers, const uint64_t requestedFrom) {
  // Status line: HTTP/1.1 CODE ...
  const size_t eol = headers.find("\r\n");
  std::string statusLine = headers.substr(0, eol);
  // The code is the token BETWEEN the first and second spaces
  // ("HTTP/1.1 206 Content" -> "206"). Taking everything after the second
  // space yields the reason phrase, whose atoi is 0 and rejected every reply.
  const size_t sp1 = statusLine.find(' ');
  const size_t sp2 = statusLine.find(' ', sp1 + 1);
  const int code = std::atoi(
      statusLine.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
  if (code != 200 && code != 206) {
    LOG_ERR("OnlineMusic", "http status %d", code);
    return false;
  }

  uint64_t contentLength = 0;
  bool haveLength = false;
  uint64_t rangeTotal = 0;
  bool haveRangeTotal = false;

  size_t pos = eol + 2;
  while (pos < headers.size()) {
    const size_t next = headers.find("\r\n", pos);
    const std::string line = headers.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    if (line.empty()) break;
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string name = lowerToken(line.substr(0, colon));
      const std::string value = line.substr(colon + 1);
      if (name == "content-length") {
        contentLength = std::strtoull(value.c_str(), nullptr, 10);
        haveLength = true;
      } else if (name == "content-range") {
        haveRangeTotal = parseTotalFromRange(value, rangeTotal);
      }
    }
    if (next == std::string::npos) break;
    pos = next + 2;
  }

  if (code == 206) {
    // Range honored: start where asked.
    position_ = requestedFrom;
    totalSize_ = haveRangeTotal ? rangeTotal : requestedFrom + contentLength;
  } else {
    // Server ignored the Range and replied with the whole entity.
    position_ = 0;
    totalSize_ = haveLength ? contentLength : 0;
  }
  return haveLength;
}

// ---------------------------------------------------------------------------
// Ring / lock setup and teardown
// ---------------------------------------------------------------------------

bool HttpByteStream::initRingAndLocks(const size_t psramSize) {
  // PSRAM first (the chosen, possibly whole-song size), then an internal-RAM
  // ring as fallback when PSRAM is unavailable or the allocation fails.
  if (psramSize > 0) {
    ring_ = static_cast<uint8_t*>(
        heap_caps_malloc(psramSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ringCap_ = ring_ != nullptr ? psramSize : 0;
  }
  if (ring_ == nullptr) {
    ring_ = static_cast<uint8_t*>(
        heap_caps_malloc(RING_FALLBACK_INTERNAL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ringCap_ = ring_ != nullptr ? RING_FALLBACK_INTERNAL : 0;
  }
  if (ring_ == nullptr) {
    ringCap_ = 0;
    LOG_ERR("OnlineMusic", "ring alloc failed");
    return false;
  }
  const bool inPsram = ringCap_ != RING_FALLBACK_INTERNAL;
  ringInPsram_ = inPsram;
  LOG_INF("OnlineMusic", "read-ahead ring %u bytes in %s (PSRAM free=%u)",
          static_cast<unsigned>(ringCap_), inPsram ? "PSRAM" : "internal RAM",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  ringMutex_ = xSemaphoreCreateMutex();
  freeSpace_ = xSemaphoreCreateBinary();
  dataReady_ = xSemaphoreCreateBinary();
  producerDone_ = xSemaphoreCreateBinary();
  if (ringMutex_ == nullptr || freeSpace_ == nullptr || dataReady_ == nullptr ||
      producerDone_ == nullptr) {
    LOG_ERR("OnlineMusic", "semaphore create failed");
    return false;
  }
  resetRingLockedState();
  return true;
}

void HttpByteStream::resetRingLockedState() {
  head_ = 0;
  tail_ = 0;
  fill_ = 0;
  ringEof_ = false;
  ringError_ = false;
  // Drop any tokens latched from a previous producer run.
  if (freeSpace_) {
    while (xSemaphoreTake(freeSpace_, 0) == pdTRUE) {
    }
  }
  if (dataReady_) {
    while (xSemaphoreTake(dataReady_, 0) == pdTRUE) {
    }
  }
  if (producerDone_) {
    while (xSemaphoreTake(producerDone_, 0) == pdTRUE) {
    }
  }
}

void HttpByteStream::ringCopyIn(const uint8_t* src, size_t n) {
  const size_t first = std::min(n, ringCap_ - head_);
  memcpy(ring_ + head_, src, first);
  if (n > first) memcpy(ring_, src + first, n - first);
  head_ = (head_ + n) % ringCap_;
}

void HttpByteStream::ringCopyOut(uint8_t* dst, size_t n) {
  const size_t first = std::min(n, ringCap_ - tail_);
  memcpy(dst, ring_ + tail_, first);
  if (n > first) memcpy(dst + first, ring_, n - first);
  tail_ = (tail_ + n) % ringCap_;
}

// ---------------------------------------------------------------------------
// Producer task
// ---------------------------------------------------------------------------

bool HttpByteStream::startProducer() {
  stopRequested_.store(false);
  if (producerDone_) {
    while (xSemaphoreTake(producerDone_, 0) == pdTRUE) {
    }
  }
  if (xTaskCreatePinnedToCore(&HttpByteStream::producerEntry, "http_fetch", PRODUCER_STACK, this,
                              PRODUCER_PRIO, &producerTask_, PRODUCER_CORE) != pdPASS) {
    producerTask_ = nullptr;
    LOG_ERR("OnlineMusic", "producer task create failed");
    return false;
  }
  return true;
}

void HttpByteStream::stopProducer() {
  if (producerTask_ == nullptr) return;
  stopRequested_.store(true);
  // Unblock a producer parked because the ring was full. A stray token is
  // harmless; every wait also re-checks stopRequested_.
  if (freeSpace_) xSemaphoreGive(freeSpace_);
  // Wait (task-independent) for the producer to latch producerDone_.
  xSemaphoreTake(producerDone_, pdMS_TO_TICKS(PRODUCER_JOIN_MS));
  producerTask_ = nullptr;
}

void HttpByteStream::producerEntry(void* arg) {
  static_cast<HttpByteStream*>(arg)->producerLoop();
}

void HttpByteStream::producerLoop() {
  uint8_t* tmp = static_cast<uint8_t*>(
      heap_caps_malloc(PRODUCER_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (tmp == nullptr) {
    xSemaphoreTake(ringMutex_, portMAX_DELAY);
    ringError_ = true;
    xSemaphoreGive(ringMutex_);
    xSemaphoreGive(dataReady_);
    goto done;
  }

  {
    int reconnectsLeft = MAX_RECONNECTS;
    while (!stopRequested_.load()) {
      // Wait until the ring has room.
      xSemaphoreTake(ringMutex_, portMAX_DELAY);
      while (fill_ == ringCap_) {
        if (stopRequested_.load()) {
          xSemaphoreGive(ringMutex_);
          goto loop_exit;
        }
        xSemaphoreGive(ringMutex_);
        xSemaphoreTake(freeSpace_, portMAX_DELAY);
        xSemaphoreTake(ringMutex_, portMAX_DELAY);
      }
      const size_t space = ringCap_ - fill_;
      xSemaphoreGive(ringMutex_);

      const int n = rawReadSocket(tmp, std::min(PRODUCER_CHUNK, space), reconnectsLeft);
      if (n <= 0) {
        // Clean EOF: verify and publish the cached file. Fatal error: drop it.
        if (n == 0) teeCommit();
        else teeAbort("stream error");
        xSemaphoreTake(ringMutex_, portMAX_DELAY);
        if (n == 0) ringEof_ = true; else ringError_ = true;
        xSemaphoreGive(ringMutex_);
        xSemaphoreGive(dataReady_);  // wake the consumer to learn the status
        goto loop_exit;
      }
      // A delivered chunk resets the reconnect budget for the next stall.
      reconnectsLeft = MAX_RECONNECTS;

      xSemaphoreTake(ringMutex_, portMAX_DELAY);
      const bool wasEmpty = (fill_ == 0);
      ringCopyIn(tmp, static_cast<size_t>(n));
      fill_ += static_cast<size_t>(n);
      xSemaphoreGive(ringMutex_);
      // Tee AFTER releasing the ring mutex: SD writes must not block ring use.
      teeWrite(tmp, static_cast<size_t>(n));
      if (wasEmpty) xSemaphoreGive(dataReady_);
    }
  loop_exit:
    heap_caps_free(tmp);
  }

done:
  // Latch completion (and wake stopProducer) before self-deleting.
  xSemaphoreGive(producerDone_);
  vTaskDelete(nullptr);
}

bool HttpByteStream::reconnectAt(const uint64_t at) {
  // Flush coalesced bytes and re-anchor at the restart position so the tee
  // stays contiguous across the dropped connection.
  teeBeforeRestart();
  // connectRange() repurposes position_ for its own start, so preserve the
  // consumer cursor across the call and adopt the effective fetch position.
  const uint64_t consumerPos = position_;
  const bool ok = connectRange(at);
  if (ok) fetchPos_ = position_;
  position_ = consumerPos;
  return ok;
}

int HttpByteStream::rawReadSocket(uint8_t* d, const size_t maxN, int& reconnectsLeft) {
  uint32_t lastDataMs = millis();
  for (;;) {
    if (stopRequested_.load()) return -1;

    const int avail = client_.available();
    if (avail > 0) {
      const int r = client_.read(d, std::min<int>(avail, static_cast<int>(maxN)));
      if (r > 0) {
        fetchPos_ += r;
        return r;
      }
    }

    // Clean EOF before any disconnect/stall handling.
    if (totalSize_ > 0 && fetchPos_ >= totalSize_) return 0;

    if (!client_.connected()) {
      if (reconnectsLeft-- <= 0) {
        LOG_ERR("OnlineMusic", "reconnect attempts exhausted");
        return -1;
      }
      LOG_INF("OnlineMusic", "reconnect at %llu", fetchPos_);
      if (!reconnectAt(fetchPos_)) return -1;
      lastDataMs = millis();
      continue;
    }
    if (static_cast<int32_t>(millis() - lastDataMs) > static_cast<int32_t>(STALL_MS)) {
      if (reconnectsLeft-- <= 0) return -1;
      LOG_INF("OnlineMusic", "stall; reconnect at %llu", fetchPos_);
      if (!reconnectAt(fetchPos_)) return -1;
      lastDataMs = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

bool HttpByteStream::abortableDelay(const uint32_t ms) {
  const uint32_t deadline = millis() + ms;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    if (stopRequested_.load()) return true;
    const uint32_t remaining = deadline - millis();
    vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(10, remaining)));
  }
  return false;
}

// ---------------------------------------------------------------------------
// Consumer read: serves only from the ring, never touches the socket
// ---------------------------------------------------------------------------

int HttpByteStream::read(uint8_t* dst, const size_t len) {
  if (!open_) return -1;
  if (len == 0) return 0;
  if (totalSize_ > 0 && position_ >= totalSize_) return 0;

  size_t got = 0;
  xSemaphoreTake(ringMutex_, portMAX_DELAY);

  while (got < len) {
    while (fill_ == 0) {
      if (ringEof_ || ringError_) {
        xSemaphoreGive(ringMutex_);
        if (got > 0) return static_cast<int>(got);
        return ringError_ ? -1 : 0;
      }
      xSemaphoreGive(ringMutex_);
      xSemaphoreTake(dataReady_, portMAX_DELAY);
      xSemaphoreTake(ringMutex_, portMAX_DELAY);
    }

    const size_t n = std::min(len - got, fill_);
    const bool wasFull = (fill_ == ringCap_);
    ringCopyOut(dst + got, n);
    fill_ -= n;
    position_ += n;
    got += n;

    const bool done =
        got == len || (totalSize_ > 0 && position_ >= totalSize_);
    xSemaphoreGive(ringMutex_);
    if (wasFull) xSemaphoreGive(freeSpace_);
    if (done) return static_cast<int>(got);
    xSemaphoreTake(ringMutex_, portMAX_DELAY);
  }
  return static_cast<int>(got);
}

// ---------------------------------------------------------------------------
// Disk tee: persist every produced byte, verify at EOF, then publish
// ---------------------------------------------------------------------------

void HttpByteStream::enableTee(const std::string& partPath,
                               const std::string& finalPath, const uint32_t id,
                               const std::string& sourceUrl) {
  // Re-arming on a live stream: drop the previous attempt first.
  if (teeEnabled_) disableTee();

  uint8_t* staging = static_cast<uint8_t*>(
      heap_caps_malloc(TEE_STAGING, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (staging == nullptr) {
    LOG_INF("MusicCache", "tee: no %u-byte staging; caching disabled",
            static_cast<unsigned>(TEE_STAGING));
    return;
  }
  teeStaging_ = staging;
  teeStagingFill_ = 0;
  teeStagingOffset_ = 0;
  teePartPath_ = partPath;
  teeFinalPath_ = finalPath;
  teeId_ = id;
  teeUrl_ = sourceUrl;
  teeFileOpen_ = false;
  teeHeaderWindow_ = false;
  teeAborted_ = false;
  teeEnabled_ = true;

  // The producer started in open() and may already have buffered a few KiB in
  // the ring (the consumer has not read yet, so they cover [0, fill)). Seed the
  // tee from those bytes; the open->enableTee gap on the caller's task is only
  // microseconds, so fill is normally 0.
  size_t seed = 0;
  xSemaphoreTake(ringMutex_, portMAX_DELAY);
  if (position_ == 0 && fill_ > 0 && fill_ <= TEE_STAGING) {
    seed = fill_;
    for (size_t k = 0; k < seed; ++k) {
      teeStaging_[k] = ring_[(tail_ + k) % ringCap_];
    }
  }
  xSemaphoreGive(ringMutex_);

  teeOpenLazy();  // no-op when PSRAM ring absent (aborts the tee)
  if (!teeFileOpen_) return;

  if (seed > 0) {
    teeStagingFill_ = seed;
    teeStagingOffset_ = 0;
    teeFlushStaging();  // persist the seed before producer bytes arrive
  }
}

void HttpByteStream::disableTee() {
  const bool wasEnabled = teeEnabled_;
  if (wasEnabled && teeFileOpen_) {
    teeAbort("disabled");  // close + remove .part; fires cb(false) once
  }
  teeEnabled_ = false;
  if (teeStaging_) {
    heap_caps_free(teeStaging_);
    teeStaging_ = nullptr;
  }
  teeStagingFill_ = 0;
  teeFileOpen_ = false;
  teeFile_.reset();
  teePartPath_.clear();
  teeFinalPath_.clear();
  teeUrl_.clear();
  teeCb_ = nullptr;
}

void HttpByteStream::setTeeCommitCallback(TeeCommitCallback cb) {
  teeCb_ = std::move(cb);
}

void HttpByteStream::setTeeHeaderWindow(const bool inWindow) {
  teeHeaderWindow_ = inWindow;
}

void HttpByteStream::teeOpenLazy() {
  if (teeFileOpen_ || teeAborted_) return;
  if (!ringInPsram_) {
    teeAbort("internal ring fallback");
    return;
  }
  auto f = std::make_unique<HalFile>();
  if (!Storage.openFileForWrite("MusicTee", teePartPath_, *f)) {
    teeAbort("open .part failed");
    return;
  }
  teeFile_ = std::move(f);
  teeFileOpen_ = true;
  teeStagingOffset_ = 0;
  teeStagingFill_ = 0;
}

void HttpByteStream::teeWrite(const uint8_t* d, const size_t n) {
  if (!teeEnabled_ || teeAborted_ || n == 0) return;

  // Lead guard: once the consumer is within 48 KiB of the producer past the
  // first 96 KiB, stop caching. Audio continuity beats filling the cache.
  uint64_t pos = position_;
  if (pos != position_) pos = position_;  // re-read on a torn 64-bit load
  if (fetchPos_ >= 96 * 1024 && fetchPos_ - pos < 48 * 1024) {
    teeAbort("consumer lead under 48 KiB");
    return;
  }

  teeOpenLazy();
  if (!teeFileOpen_) return;  // open failure already aborted

  const uint64_t absStart = fetchPos_ - static_cast<uint64_t>(n);
  if (teeStagingFill_ > 0) {
    const uint64_t expected = teeStagingOffset_ + teeStagingFill_;
    if (absStart == expected) {
      // contiguous: fall through to append
    } else if (absStart < expected) {
      // decoder/seek rewind: persist staging, then overwrite from absStart
      // (re-fetched CDN bytes are identical); it joins seamlessly later.
      teeFlushStaging();
      if (teeAborted_) return;
      teeStagingOffset_ = absStart;
    } else {
      // unbridgeable hole (forward scrub past produced bytes): invalid cache
      teeAbort("byte gap after seek");
      return;
    }
  } else {
    teeStagingOffset_ = absStart;
  }

  size_t taken = 0;
  while (taken < n) {
    const size_t space = TEE_STAGING - teeStagingFill_;
    const size_t m = std::min(n - taken, space);
    memcpy(teeStaging_ + teeStagingFill_, d + taken, m);
    teeStagingFill_ += m;
    taken += m;
    if (teeStagingFill_ == TEE_STAGING) {
      teeFlushStaging();
      if (teeAborted_) return;
    }
  }
}

void HttpByteStream::teeFlushStaging() {
  if (!teeFileOpen_ || teeStagingFill_ == 0) return;
  if (!teeFile_->seek64(teeStagingOffset_)) {
    teeAbort("seek failed");
    return;
  }
  const size_t w = teeFile_->write(teeStaging_, teeStagingFill_);
  if (w != teeStagingFill_) {
    teeAbort("short write");
    return;
  }
  teeStagingOffset_ += w;
  teeStagingFill_ = 0;
}

void HttpByteStream::teeBeforeRestart() {
  if (!teeEnabled_ || teeAborted_) return;
  if (teeFileOpen_) {
    teeFlushStaging();
    if (!teeAborted_) teeStagingOffset_ = fetchPos_;
  }
}

void HttpByteStream::teeCommit() {
  if (!teeEnabled_ || teeAborted_) return;
  teeOpenLazy();
  if (!teeFileOpen_) return;  // aborted during lazy open
  teeFlushStaging();
  if (teeAborted_) return;

  const uint64_t got = teeFile_->fileSize64();
  if (totalSize_ == 0 || got != totalSize_) {
    LOG_ERR("MusicCache", "tee: size mismatch %llu != %llu; discard",
            got, totalSize_);
    teeAbort("size mismatch");
    return;
  }
  teeFile_->close();
  teeFileOpen_ = false;
  teeFile_.reset();

  if (!Storage.rename(teePartPath_.c_str(), teeFinalPath_.c_str())) {
    Storage.remove(teePartPath_.c_str());
    teeAborted_ = true;
    if (teeCb_) teeCb_(false);
    return;
  }
  teeAborted_ = true;  // terminal: later close() must not touch the file
  LOG_INF("MusicCache", "tee commit ok: id=%u size=%llu", teeId_, got);
  if (teeCb_) teeCb_(true);
}

void HttpByteStream::teeAbort(const char* reason) {
  if (teeAborted_) return;
  teeAborted_ = true;
  if (teeFileOpen_) {
    teeFile_->close();
    teeFileOpen_ = false;
  }
  teeFile_.reset();
  if (teeEnabled_) Storage.remove(teePartPath_.c_str());
  teeStagingFill_ = 0;
  LOG_INF("MusicCache", "tee abort: %s", reason ? reason : "?");
  if (teeCb_) teeCb_(false);
}
