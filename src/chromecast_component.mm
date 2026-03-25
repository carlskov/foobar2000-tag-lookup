#include <foobar2000/SDK/foobar2000.h>

#import <AppKit/AppKit.h>

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <Security/SecureTransport.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

constexpr GUID kMenuToggleGuid =
    {0xa3a4273d, 0x561b, 0x48d5, {0x9f, 0xae, 0x71, 0x6f, 0x5e, 0x04, 0x95, 0x22}};
constexpr GUID kChromecastDeviceGuid =
    {0xf3d4f3bc, 0x2c9f, 0x4a31, {0x90, 0x7d, 0xe4, 0x35, 0xcf, 0x22, 0x88, 0xa1}};

constexpr size_t kChromecastQueueWindow = 26;

constexpr const char* kCastReceiverNamespace = "urn:x-cast:com.google.cast.receiver";
constexpr const char* kCastConnectionNamespace = "urn:x-cast:com.google.cast.tp.connection";
constexpr const char* kCastHeartbeatNamespace = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr const char* kCastMediaNamespace = "urn:x-cast:com.google.cast.media";
constexpr const char* kDefaultMediaReceiverAppId = "CC1AD845";
constexpr size_t kMaxQueueLoadItems = 100;
constexpr size_t kMaxQueueLoadJsonBytes = 60 * 1024;

cfg_string g_lastChromecastDevice(kChromecastDeviceGuid, "");

struct ChromecastDeviceInfo {
  std::string displayName;
  std::string serviceName;
  std::string host;
  uint16_t port = 8009;
};

struct PreparedQueueItem {
  std::string url;
  std::string mimeType;
  std::string title;
  std::string artist;
  std::string album;
  double durationSec = 0;
};

NSString* ToNSString(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

std::string Trim(std::string s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string FromNSString(NSString* value) {
  if (value == nil) {
    return "";
  }
  const char* utf8 = [value UTF8String];
  if (utf8 == nullptr) {
    return "";
  }
  return Trim(std::string(utf8));
}

bool StartsWithHttp(const std::string& value) {
  return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string StripTrailingDot(std::string host) {
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

std::string UrlEncode(const std::string& value) {
  std::ostringstream out;
  out.fill('0');
  out << std::hex << std::uppercase;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out << c;
    } else {
      out << '%' << std::setw(2) << static_cast<int>(c);
    }
  }
  return out.str();
}

std::string UrlDecode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      char hex[3] = {value[i + 1], value[i + 2], 0};
      char* endPtr = nullptr;
      const long v = std::strtol(hex, &endPtr, 16);
      if (endPtr != nullptr && *endPtr == 0) {
        out.push_back(static_cast<char>(v));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

bool SendAllFd(int fd, const char* data, size_t len) {
  while (len > 0) {
    const ssize_t n = send(fd, data, len, 0);
    if (n <= 0) {
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

std::string Basename(const std::string& path) {
  const size_t slashPos = path.find_last_of('/');
  if (slashPos == std::string::npos) {
    return path;
  }
  return path.substr(slashPos + 1);
}

std::string GetMimeType(const std::string& input) {
  const size_t dotPos = input.find_last_of('.');
  if (dotPos == std::string::npos) {
    return "audio/mpeg";
  }
  const std::string ext = ToLower(input.substr(dotPos + 1));

  static const std::unordered_map<std::string, std::string> kMimeByExt = {
      {"mp3", "audio/mpeg"},       {"flac", "audio/flac"},
      {"m4a", "audio/mp4"},        {"mp4", "audio/mp4"},
      {"aac", "audio/aac"},        {"wav", "audio/wav"},
      {"aiff", "audio/aiff"},      {"aif", "audio/aiff"},
      {"ogg", "audio/ogg"},        {"oga", "audio/ogg"},
      {"opus", "audio/opus"},      {"wma", "audio/x-ms-wma"},
      {"mka", "audio/x-matroska"}, {"webm", "audio/webm"},
      {"alac", "audio/mp4"},       {"ac3", "audio/ac3"},
      {"eac3", "audio/eac3"}};

  const auto it = kMimeByExt.find(ext);
  if (it != kMimeByExt.end()) {
    return it->second;
  }
  return "audio/mpeg";
}

class LocalFileHttpServer {
 public:
  LocalFileHttpServer(std::string filePath, std::string bindHost)
      : filePath_(std::move(filePath)), bindHost_(std::move(bindHost)) {
    fileName_ = Basename(filePath_);
    encodedFileName_ = UrlEncode(fileName_);
    mimeType_ = GetMimeType(filePath_);
  }

  ~LocalFileHttpServer() {
    Stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Stop() {
    running_.store(false);

    if (listenFd_ >= 0) {
      shutdown(listenFd_, SHUT_RDWR);
      close(listenFd_);
      listenFd_ = -1;
    }

    std::vector<int> clientFds;
    {
      std::lock_guard<std::mutex> lock(clientMutex_);
      clientFds.assign(activeClientFds_.begin(), activeClientFds_.end());
    }

    for (int fd : clientFds) {
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
  }

  bool Start(std::string& error) {
    if (!BindAndListen(error)) {
      return false;
    }
    running_.store(true);
    worker_ = std::thread([this]() { ServeLoop(); });
    return true;
  }

  uint16_t port() const { return port_; }

  std::string MediaUrl() const {
    std::ostringstream out;
    out << "http://" << bindHost_ << ":" << port_ << "/" << encodedFileName_;
    return out.str();
  }

  std::string mimeType() const { return mimeType_; }

 private:
  bool BindAndListen(std::string& error) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      error = "Failed to create local HTTP socket.";
      return false;
    }

    const int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(listenFd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      error = "Failed to bind local HTTP socket.";
      close(listenFd_);
      listenFd_ = -1;
      return false;
    }

    sockaddr_in bound = {};
    socklen_t boundLen = sizeof(bound);
    if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
      error = "Failed to query local HTTP port.";
      close(listenFd_);
      listenFd_ = -1;
      return false;
    }
    port_ = ntohs(bound.sin_port);

    if (listen(listenFd_, 8) != 0) {
      error = "Failed to listen on local HTTP socket.";
      close(listenFd_);
      listenFd_ = -1;
      return false;
    }

    return true;
  }

  void ServeLoop() {
    const auto started = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::hours(2);

    while (running_.load()) {
      if (std::chrono::steady_clock::now() - started > ttl) {
        break;
      }

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(listenFd_, &rfds);

      timeval tv = {};
      tv.tv_sec = 1;
      tv.tv_usec = 0;

      const int ready = select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
      if (ready <= 0) {
        continue;
      }

      int clientFd = accept(listenFd_, nullptr, nullptr);
      if (clientFd < 0) {
        continue;
      }

      const int yes = 1;
      setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
      {
        std::lock_guard<std::mutex> lock(clientMutex_);
        activeClientFds_.insert(clientFd);
      }
      HandleClient(clientFd);
      {
        std::lock_guard<std::mutex> lock(clientMutex_);
        activeClientFds_.erase(clientFd);
      }
      close(clientFd);
    }
  }

  static bool ParseRange(const std::string& rangeValue, uint64_t size, uint64_t& start,
                         uint64_t& endExclusive) {
    if (rangeValue.rfind("bytes=", 0) != 0) {
      return false;
    }
    std::string spec = Trim(rangeValue.substr(6));
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) {
      return false;
    }

    const std::string startStr = Trim(spec.substr(0, dash));
    const std::string endStr = Trim(spec.substr(dash + 1));

    if (startStr.empty()) {
      // Suffix form: bytes=-N
      char* endPtr = nullptr;
      const unsigned long long suffixLen = std::strtoull(endStr.c_str(), &endPtr, 10);
      if (endPtr == nullptr || *endPtr != 0 || suffixLen == 0) {
        return false;
      }
      if (suffixLen >= size) {
        start = 0;
      } else {
        start = size - suffixLen;
      }
      endExclusive = size;
      return true;
    }

    char* startEndPtr = nullptr;
    const unsigned long long parsedStart = std::strtoull(startStr.c_str(), &startEndPtr, 10);
    if (startEndPtr == nullptr || *startEndPtr != 0) {
      return false;
    }

    start = static_cast<uint64_t>(parsedStart);
    if (start >= size) {
      return false;
    }

    if (endStr.empty()) {
      endExclusive = size;
      return true;
    }

    char* endEndPtr = nullptr;
    const unsigned long long parsedEnd = std::strtoull(endStr.c_str(), &endEndPtr, 10);
    if (endEndPtr == nullptr || *endEndPtr != 0 || parsedEnd < parsedStart) {
      return false;
    }

    uint64_t inclusiveEnd = static_cast<uint64_t>(parsedEnd);
    if (inclusiveEnd >= size) {
      inclusiveEnd = size - 1;
    }
    endExclusive = inclusiveEnd + 1;
    return true;
  }

  void HandleClient(int clientFd) {
    std::string request;
    request.reserve(4096);

    char buf[2048] = {};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
      const ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
      if (n <= 0) {
        return;
      }
      request.append(buf, static_cast<size_t>(n));
    }

    std::istringstream ss(request);
    std::string requestLine;
    if (!std::getline(ss, requestLine)) {
      return;
    }
    requestLine = Trim(requestLine);

    std::istringstream lineParser(requestLine);
    std::string method;
    std::string target;
    std::string version;
    lineParser >> method >> target >> version;

    std::unordered_map<std::string, std::string> headers;
    std::string line;
    while (std::getline(ss, line)) {
      line = Trim(line);
      if (line.empty()) {
        break;
      }
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      std::string key = ToLower(Trim(line.substr(0, colon)));
      std::string value = Trim(line.substr(colon + 1));
      headers[key] = value;
    }

    if (method != "GET" && method != "HEAD") {
      const std::string response =
          "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n";
      SendAllFd(clientFd, response.c_str(), response.size());
      return;
    }

    const size_t queryPos = target.find('?');
    std::string path = queryPos == std::string::npos ? target : target.substr(0, queryPos);
    if (!path.empty() && path.front() == '/') {
      path.erase(path.begin());
    }
    path = UrlDecode(path);

    if (path != fileName_) {
      const std::string response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
      SendAllFd(clientFd, response.c_str(), response.size());
      return;
    }

    std::ifstream file(filePath_, std::ios::binary);
    if (!file) {
      const std::string response =
          "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
      SendAllFd(clientFd, response.c_str(), response.size());
      return;
    }

    file.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    bool partial = false;
    uint64_t start = 0;
    uint64_t endExclusive = size;

    const auto rangeIt = headers.find("range");
    if (rangeIt != headers.end()) {
      partial = ParseRange(rangeIt->second, size, start, endExclusive);
      if (!partial) {
        std::ostringstream response;
        response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                 << "Content-Range: bytes */" << size << "\r\n"
                 << "Connection: close\r\n\r\n";
        const std::string responseStr = response.str();
        SendAllFd(clientFd, responseStr.c_str(), responseStr.size());
        return;
      }
    }

    const uint64_t bodyLen = endExclusive - start;

    std::ostringstream headersOut;
    if (partial) {
      headersOut << "HTTP/1.1 206 Partial Content\r\n";
      headersOut << "Content-Range: bytes " << start << "-" << (endExclusive - 1) << "/" << size
                 << "\r\n";
    } else {
      headersOut << "HTTP/1.1 200 OK\r\n";
    }
    headersOut << "Content-Type: " << mimeType_ << "\r\n";
    headersOut << "Accept-Ranges: bytes\r\n";
    headersOut << "Content-Length: " << bodyLen << "\r\n";
    headersOut << "Connection: close\r\n\r\n";

    const std::string headerText = headersOut.str();
    if (!SendAllFd(clientFd, headerText.c_str(), headerText.size())) {
      return;
    }

    if (method == "HEAD" || bodyLen == 0) {
      return;
    }

    file.seekg(static_cast<std::streamoff>(start), std::ios::beg);

    std::array<char, 64 * 1024> outBuf = {};
    uint64_t left = bodyLen;
    while (left > 0) {
      const size_t toRead = static_cast<size_t>(std::min<uint64_t>(left, outBuf.size()));
      file.read(outBuf.data(), static_cast<std::streamsize>(toRead));
      const std::streamsize got = file.gcount();
      if (got <= 0) {
        break;
      }
      if (!SendAllFd(clientFd, outBuf.data(), static_cast<size_t>(got))) {
        break;
      }
      left -= static_cast<uint64_t>(got);
    }
  }

 private:
  std::string filePath_;
  std::string fileName_;
  std::string encodedFileName_;
  std::string mimeType_;
  std::string bindHost_;

  int listenFd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::mutex clientMutex_;
  std::set<int> activeClientFds_;
};

std::mutex g_serversMutex;
std::vector<std::shared_ptr<LocalFileHttpServer>> g_servers;

void ClearLocalServers() {
  std::vector<std::shared_ptr<LocalFileHttpServer>> servers;
  {
    std::lock_guard<std::mutex> lock(g_serversMutex);
    servers.swap(g_servers);
  }
  for (const auto& server : servers) {
    if (server) {
      server->Stop();
    }
  }
}

std::mutex g_activeCastMutex;
std::optional<ChromecastDeviceInfo> g_activeCastDevice;

class ChromecastPlaybackBridgeDynamic;
std::mutex g_playbackBridgeMutex;
std::unique_ptr<ChromecastPlaybackBridgeDynamic> g_playbackBridge;
bool g_playbackBridgeRegistered = false;
std::atomic<bool> g_componentShuttingDown{false};
std::atomic<uint64_t> g_castReloadSerial{0};
std::mutex g_castActionMutex;
std::mutex g_localVolumeMutex;
bool g_localVolumeMutedForChromecast = false;
float g_savedLocalVolumeDb = 0;

void SetActiveCastDevice(const ChromecastDeviceInfo& device) {
  std::lock_guard<std::mutex> lock(g_activeCastMutex);
  g_activeCastDevice = device;
}

void ClearActiveCastDevice() {
  std::lock_guard<std::mutex> lock(g_activeCastMutex);
  g_activeCastDevice.reset();
}

bool TryGetActiveCastDevice(ChromecastDeviceInfo& out) {
  std::lock_guard<std::mutex> lock(g_activeCastMutex);
  if (!g_activeCastDevice.has_value()) {
    return false;
  }
  out = *g_activeCastDevice;
  return true;
}

bool HasActiveCastDevice() {
  std::lock_guard<std::mutex> lock(g_activeCastMutex);
  return g_activeCastDevice.has_value();
}

void MuteLocalPlaybackForChromecast() {
  auto playback = playback_control::get();
  if (!playback.is_valid()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_localVolumeMutex);
  if (g_localVolumeMutedForChromecast) {
    return;
  }

  g_savedLocalVolumeDb = playback->get_volume();
  playback->set_volume(static_cast<float>(playback_control::volume_mute));
  g_localVolumeMutedForChromecast = true;
}

void RestoreLocalPlaybackVolume() {
  auto playback = playback_control::get();
  if (!playback.is_valid()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_localVolumeMutex);
  if (!g_localVolumeMutedForChromecast) {
    return;
  }

  playback->set_volume(g_savedLocalVolumeDb);
  g_localVolumeMutedForChromecast = false;
}

void RegisterPlaybackBridge();
void UnregisterPlaybackBridge();
void RequestChromecastQueueReloadForDevice(const ChromecastDeviceInfo& device, bool showPopup);
void RequestActiveChromecastQueueReload(bool showPopup = false);
void DisableChromecastSession(bool showPopup, bool sendStop);

std::string GetLocalIpForTarget(const std::string& targetHost) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* results = nullptr;
  if (getaddrinfo(targetHost.c_str(), "9", &hints, &results) != 0 || results == nullptr) {
    return "";
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    freeaddrinfo(results);
    return "";
  }

  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));

  std::string ip;
  if (connect(fd, results->ai_addr, results->ai_addrlen) == 0) {
    sockaddr_in localAddr = {};
    socklen_t len = sizeof(localAddr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&localAddr), &len) == 0) {
      char ipBuf[INET_ADDRSTRLEN] = {};
      if (inet_ntop(AF_INET, &localAddr.sin_addr, ipBuf, sizeof(ipBuf)) != nullptr) {
        ip = ipBuf;
      }
    }
  }

  close(fd);
  freeaddrinfo(results);
  return ip;
}

struct BrowseEntry {
  std::string serviceName;
  std::string regType;
  std::string domain;
  uint32_t interfaceIndex = 0;
};

struct BrowseContext {
  std::mutex mutex;
  std::vector<BrowseEntry> entries;
};

struct ResolveContext {
  bool done = false;
  bool ok = false;
  std::string host;
  uint16_t port = 8009;
  std::string friendlyName;
};

std::string ParseFriendlyNameFromTxt(uint16_t txtLen, const unsigned char* txtRecord) {
  uint8_t valueLen = 0;
  const void* value = TXTRecordGetValuePtr(txtLen, txtRecord, "fn", &valueLen);
  if (value == nullptr || valueLen == 0) {
    return "";
  }
  return std::string(static_cast<const char*>(value), static_cast<size_t>(valueLen));
}

void DNSSD_API BrowseCallback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
                              DNSServiceErrorType errorCode, const char* serviceName,
                              const char* regtype, const char* replyDomain, void* context) {
  (void)sdRef;
  if (errorCode != kDNSServiceErr_NoError || context == nullptr || serviceName == nullptr ||
      regtype == nullptr || replyDomain == nullptr) {
    return;
  }

  if ((flags & kDNSServiceFlagsAdd) == 0) {
    return;
  }

  auto* ctx = static_cast<BrowseContext*>(context);
  std::lock_guard<std::mutex> lock(ctx->mutex);
  ctx->entries.push_back({serviceName, regtype, replyDomain, interfaceIndex});
}

void DNSSD_API ResolveCallback(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
                               DNSServiceErrorType errorCode, const char* fullname,
                               const char* hosttarget, uint16_t port, uint16_t txtLen,
                               const unsigned char* txtRecord, void* context) {
  (void)sdRef;
  (void)flags;
  (void)interfaceIndex;
  (void)fullname;

  auto* ctx = static_cast<ResolveContext*>(context);
  if (ctx == nullptr) {
    return;
  }

  ctx->done = true;
  if (errorCode != kDNSServiceErr_NoError || hosttarget == nullptr) {
    ctx->ok = false;
    return;
  }

  ctx->ok = true;
  ctx->host = StripTrailingDot(hosttarget);
  ctx->port = ntohs(port);
  ctx->friendlyName = ParseFriendlyNameFromTxt(txtLen, txtRecord);
}

bool PumpDnsService(DNSServiceRef service, int timeoutMs) {
  const int fd = DNSServiceRefSockFD(service);
  if (fd < 0) {
    return false;
  }

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);

  timeval tv = {};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;

  const int selected = select(fd + 1, &rfds, nullptr, nullptr, &tv);
  if (selected <= 0) {
    return false;
  }

  if (!FD_ISSET(fd, &rfds)) {
    return false;
  }

  const DNSServiceErrorType err = DNSServiceProcessResult(service);
  return err == kDNSServiceErr_NoError;
}

std::optional<ChromecastDeviceInfo> ResolveService(const BrowseEntry& entry) {
  ResolveContext ctx;
  DNSServiceRef resolveRef = nullptr;
  const DNSServiceErrorType err =
      DNSServiceResolve(&resolveRef, 0, entry.interfaceIndex, entry.serviceName.c_str(),
                        entry.regType.c_str(), entry.domain.c_str(), ResolveCallback, &ctx);
  if (err != kDNSServiceErr_NoError || resolveRef == nullptr) {
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (!ctx.done && std::chrono::steady_clock::now() < deadline) {
    if (!PumpDnsService(resolveRef, 200)) {
      continue;
    }
  }

  DNSServiceRefDeallocate(resolveRef);
  if (!ctx.done || !ctx.ok || ctx.host.empty() || ctx.port == 0) {
    return std::nullopt;
  }

  ChromecastDeviceInfo device;
  device.serviceName = entry.serviceName;
  device.displayName = ctx.friendlyName.empty() ? entry.serviceName : ctx.friendlyName;
  device.host = ctx.host;
  device.port = ctx.port;
  return device;
}

std::vector<ChromecastDeviceInfo> DiscoverChromecastDevices(std::string& statusMessage) {
  statusMessage.clear();

  DNSServiceRef browseRef = nullptr;
  BrowseContext browseCtx;

  const DNSServiceErrorType err =
      DNSServiceBrowse(&browseRef, 0, 0, "_googlecast._tcp", nullptr, BrowseCallback, &browseCtx);

  if (err != kDNSServiceErr_NoError || browseRef == nullptr) {
    statusMessage = "Bonjour discovery failed to start.";
    return {};
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
  while (std::chrono::steady_clock::now() < deadline) {
    PumpDnsService(browseRef, 250);
  }

  DNSServiceRefDeallocate(browseRef);

  std::vector<BrowseEntry> entries;
  {
    std::lock_guard<std::mutex> lock(browseCtx.mutex);
    entries = browseCtx.entries;
  }

  std::vector<ChromecastDeviceInfo> devices;
  std::set<std::string> dedupe;
  for (const auto& entry : entries) {
    const auto resolved = ResolveService(entry);
    if (!resolved.has_value()) {
      continue;
    }

    const std::string key = ToLower(resolved->displayName) + "|" + ToLower(resolved->host);
    if (dedupe.insert(key).second) {
      devices.push_back(*resolved);
    }
  }

  std::sort(devices.begin(), devices.end(), [](const ChromecastDeviceInfo& a,
                                               const ChromecastDeviceInfo& b) {
    return ToLower(a.displayName) < ToLower(b.displayName);
  });

  if (devices.empty()) {
    statusMessage = "No Chromecast devices found. You can still type a device name.";
  } else {
    statusMessage = "Discovered " + std::to_string(devices.size()) +
                    " device(s). You can choose or type manually.";
  }

  return devices;
}

std::optional<ChromecastDeviceInfo> FindDeviceByName(const std::vector<ChromecastDeviceInfo>& devices,
                                                     const std::string& name) {
  const std::string wanted = ToLower(Trim(name));
  if (wanted.empty()) {
    return std::nullopt;
  }

  for (const auto& d : devices) {
    if (ToLower(d.displayName) == wanted || ToLower(d.serviceName) == wanted) {
      return d;
    }
  }
  return std::nullopt;
}

bool PromptChromecastDevice(std::string& deviceName,
              const std::vector<ChromecastDeviceInfo>& discoveredDevices,
              const std::string& statusMessage) {
  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Chromecast Audio";
  alert.informativeText =
    @"Pick or type a Chromecast device. Current track and upcoming playlist items will be cast.";

    [alert addButtonWithTitle:@"Cast"];
    [alert addButtonWithTitle:@"Cancel"];

  NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 680, 84)];

  NSTextField* deviceLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 52, 120, 22)];
    deviceLabel.stringValue = @"Device";
    deviceLabel.bezeled = NO;
    deviceLabel.drawsBackground = NO;
    deviceLabel.editable = NO;
    deviceLabel.selectable = NO;

  NSComboBox* deviceInput = [[NSComboBox alloc] initWithFrame:NSMakeRect(128, 50, 540, 24)];
    deviceInput.usesDataSource = NO;
    deviceInput.completes = YES;
    for (const auto& item : discoveredDevices) {
      [deviceInput addItemWithObjectValue:ToNSString(item.displayName)];
    }
    deviceInput.stringValue = ToNSString(deviceName);
    if (deviceName.empty() && !discoveredDevices.empty()) {
      deviceInput.stringValue = ToNSString(discoveredDevices.front().displayName);
    }

    NSTextField* statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(128, 18, 540, 22)];
    statusLabel.bezeled = NO;
    statusLabel.drawsBackground = NO;
    statusLabel.editable = NO;
    statusLabel.selectable = NO;
    statusLabel.font = [NSFont systemFontOfSize:12.0];
    statusLabel.textColor = [NSColor secondaryLabelColor];
    statusLabel.stringValue = ToNSString(statusMessage);

    [view addSubview:deviceLabel];
    [view addSubview:deviceInput];
    [view addSubview:statusLabel];

    alert.accessoryView = view;

    const NSModalResponse response = [alert runModal];
    if (response != NSAlertFirstButtonReturn) {
      return false;
    }

    deviceName = FromNSString(deviceInput.stringValue);
    return true;
  }
}

class SecureTlsSocket {
 public:
  ~SecureTlsSocket() { Close(); }

  bool Connect(const std::string& host, uint16_t port, std::string& error) {
    if (!ConnectTcp(host, port, error)) {
      return false;
    }

    ssl_ = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (ssl_ == nullptr) {
      error = "Failed to create TLS context.";
      Close();
      return false;
    }

    SSLSetIOFuncs(ssl_, SocketRead, SocketWrite);
    SSLSetConnection(ssl_, reinterpret_cast<SSLConnectionRef>(static_cast<intptr_t>(fd_)));
    SSLSetProtocolVersionMin(ssl_, kTLSProtocol1);
    SSLSetEnableCertVerify(ssl_, false);

    while (true) {
      const OSStatus status = SSLHandshake(ssl_);
      if (status == noErr) {
        break;
      }
      if (status == errSSLWouldBlock) {
        continue;
      }
      error = "TLS handshake failed.";
      Close();
      return false;
    }

    return true;
  }

  bool WriteAll(const uint8_t* data, size_t len, std::string& error) {
    size_t offset = 0;
    while (offset < len) {
      size_t written = 0;
      const OSStatus status = SSLWrite(ssl_, data + offset, len - offset, &written);
      if (status == noErr) {
        offset += written;
        continue;
      }
      if (status == errSSLWouldBlock) {
        offset += written;
        if (offset < len && !WaitWritable(1000)) {
          error = "Timed out waiting for Chromecast write readiness.";
          return false;
        }
        continue;
      }
      if (status == errSSLClosedGraceful || status == errSSLClosedNoNotify) {
        error = "Chromecast closed the TLS connection.";
        return false;
      }
      char buf[96];
      snprintf(buf, sizeof(buf), "TLS write failed (OSStatus=%d).", static_cast<int>(status));
      error = buf;
      return false;
    }
    return true;
  }

  bool ReadExact(uint8_t* data, size_t len, int timeoutMs, std::string& error) {
    size_t offset = 0;
    while (offset < len) {
      if (!WaitReadable(timeoutMs)) {
        error = "Timed out waiting for Chromecast response.";
        return false;
      }

      size_t got = 0;
      const OSStatus status = SSLRead(ssl_, data + offset, len - offset, &got);
      if (status == noErr || status == errSSLWouldBlock) {
        offset += got;
        continue;
      }
      if (status == errSSLClosedGraceful || status == errSSLClosedNoNotify) {
        error = "Chromecast closed the TLS connection.";
        return false;
      }
      error = "TLS read failed.";
      return false;
    }
    return true;
  }

  void Close() {
    if (ssl_ != nullptr) {
      SSLClose(ssl_);
      CFRelease(ssl_);
      ssl_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

 private:
  static OSStatus SocketRead(SSLConnectionRef connection, void* data, size_t* dataLength) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(connection));
    ssize_t n = recv(fd, data, *dataLength, 0);
    if (n > 0) {
      *dataLength = static_cast<size_t>(n);
      return noErr;
    }
    if (n == 0) {
      *dataLength = 0;
      return errSSLClosedGraceful;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      *dataLength = 0;
      return errSSLWouldBlock;
    }
    *dataLength = 0;
    return errSecIO;
  }

  static OSStatus SocketWrite(SSLConnectionRef connection, const void* data, size_t* dataLength) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(connection));
    ssize_t n = send(fd, data, *dataLength, 0);
    if (n > 0) {
      *dataLength = static_cast<size_t>(n);
      return noErr;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      *dataLength = 0;
      return errSSLWouldBlock;
    }
    *dataLength = 0;
    return errSecIO;
  }

  bool ConnectTcp(const std::string& host, uint16_t port, std::string& error) {
    addrinfo hints = {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr) {
      error = "Could not resolve Chromecast host.";
      return false;
    }

    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
      int candidate = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
      if (candidate < 0) {
        continue;
      }

      const int yes = 1;
      setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));

      if (connect(candidate, it->ai_addr, it->ai_addrlen) == 0) {
        fd_ = candidate;
        break;
      }

      close(candidate);
    }

    freeaddrinfo(result);

    if (fd_ < 0) {
      error = "Failed to connect to Chromecast.";
      return false;
    }

    return true;
  }

  bool WaitReadable(int timeoutMs) {
    if (fd_ < 0) {
      return false;
    }

    // SecureTransport may have already decrypted and buffered data internally
    // from a previous SSLRead call.  select() on the raw socket fd is blind to
    // this internal buffer, so we must check it explicitly before blocking.
    if (ssl_ != nullptr) {
      size_t buffered = 0;
      if (SSLGetBufferedReadSize(ssl_, &buffered) == noErr && buffered > 0) {
        return true;
      }
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);

    timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    const int selected = select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
    return selected > 0 && FD_ISSET(fd_, &rfds);
  }

  bool WaitWritable(int timeoutMs) {
    if (fd_ < 0) {
      return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);

    timeval tv = {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    const int selected = select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
    return selected > 0 && FD_ISSET(fd_, &wfds);
  }

  int fd_ = -1;
  SSLContextRef ssl_ = nullptr;
};

void AppendVarint(uint64_t value, std::vector<uint8_t>& out) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

bool ReadVarint(const std::vector<uint8_t>& data, size_t& offset, uint64_t& value) {
  value = 0;
  int shift = 0;
  while (offset < data.size() && shift <= 63) {
    const uint8_t byte = data[offset++];
    value |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
    shift += 7;
  }
  return false;
}

void AppendFieldVarint(uint32_t field, uint64_t value, std::vector<uint8_t>& out) {
  AppendVarint((static_cast<uint64_t>(field) << 3) | 0, out);
  AppendVarint(value, out);
}

void AppendFieldString(uint32_t field, const std::string& value, std::vector<uint8_t>& out) {
  AppendVarint((static_cast<uint64_t>(field) << 3) | 2, out);
  AppendVarint(value.size(), out);
  out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> BuildCastMessage(const std::string& sourceId, const std::string& destinationId,
                                      const std::string& castNamespace,
                                      const std::string& jsonPayload) {
  std::vector<uint8_t> payload;
  AppendFieldVarint(1, 0, payload);  // CASTV2_1_0
  AppendFieldString(2, sourceId, payload);
  AppendFieldString(3, destinationId, payload);
  AppendFieldString(4, castNamespace, payload);
  AppendFieldVarint(5, 0, payload);  // STRING
  AppendFieldString(6, jsonPayload, payload);
  return payload;
}

bool SendCastMessage(SecureTlsSocket& sock, const std::string& sourceId,
                     const std::string& destinationId, const std::string& castNamespace,
                     const json& payload, std::string& error) {
  const std::string payloadText = payload.dump();
  std::vector<uint8_t> message =
      BuildCastMessage(sourceId, destinationId, castNamespace, payloadText);
  const std::string msgType = payload.value("type", "unknown");

  std::array<uint8_t, 4> frameHeader = {
      static_cast<uint8_t>((message.size() >> 24) & 0xFF),
      static_cast<uint8_t>((message.size() >> 16) & 0xFF),
      static_cast<uint8_t>((message.size() >> 8) & 0xFF),
      static_cast<uint8_t>(message.size() & 0xFF),
  };

  if (!sock.WriteAll(frameHeader.data(), frameHeader.size(), error)) {
    error = "send(" + castNamespace + ", type=" + msgType + ", header=4, body=" +
            std::to_string(message.size()) + ") failed: " + error;
    return false;
  }
  if (!sock.WriteAll(message.data(), message.size(), error)) {
    error = "send(" + castNamespace + ", type=" + msgType + ", body=" +
            std::to_string(message.size()) + ", json=" + std::to_string(payloadText.size()) +
            ") failed: " + error;
    return false;
  }
  return true;
}

json MakeConnectPayload() {
  // Minimal payload matching what pychromecast and Chrome send for old Chromecast Audio
  return json{{"type", "CONNECT"}, {"origin", json::object()}};
}

bool ReadCastFrame(SecureTlsSocket& sock, std::vector<uint8_t>& frame, int timeoutMs,
                   std::string& error) {
  std::array<uint8_t, 4> hdr = {};
  if (!sock.ReadExact(hdr.data(), hdr.size(), timeoutMs, error)) {
    return false;
  }

  const uint32_t len = (static_cast<uint32_t>(hdr[0]) << 24) |
                       (static_cast<uint32_t>(hdr[1]) << 16) |
                       (static_cast<uint32_t>(hdr[2]) << 8) | static_cast<uint32_t>(hdr[3]);

  if (len == 0 || len > 1024 * 1024) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Invalid frame size: %u (0x%08X).", (unsigned)len, (unsigned)len);
    error = buf;
    return false;
  }

  frame.resize(len);
  return sock.ReadExact(frame.data(), frame.size(), timeoutMs, error);
}

bool ParseCastMessagePayloadUtf8(const std::vector<uint8_t>& frame, std::string& payloadUtf8,
                                 std::string& sourceId, std::string& destinationId,
                                 std::string& castNamespace) {
  payloadUtf8.clear();
  sourceId.clear();
  destinationId.clear();
  castNamespace.clear();

  size_t offset = 0;
  while (offset < frame.size()) {
    uint64_t key = 0;
    if (!ReadVarint(frame, offset, key)) {
      return false;
    }

    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint32_t wireType = static_cast<uint32_t>(key & 0x07);

    if (wireType == 0) {
      uint64_t ignored = 0;
      if (!ReadVarint(frame, offset, ignored)) {
        return false;
      }
      continue;
    }

    if (wireType == 2) {
      uint64_t len = 0;
      if (!ReadVarint(frame, offset, len)) {
        return false;
      }
      if (offset + len > frame.size()) {
        return false;
      }

      const char* ptr = reinterpret_cast<const char*>(frame.data() + offset);
      const std::string value(ptr, ptr + len);
      if (field == 2) {
        sourceId = value;
      } else if (field == 3) {
        destinationId = value;
      } else if (field == 4) {
        castNamespace = value;
      } else if (field == 6) {
        payloadUtf8 = value;
      }
      offset += static_cast<size_t>(len);
      continue;
    }

    if (wireType == 1) {
      if (offset + 8 > frame.size()) {
        return false;
      }
      offset += 8;
      continue;
    }

    if (wireType == 5) {
      if (offset + 4 > frame.size()) {
        return false;
      }
      offset += 4;
      continue;
    }

    return false;
  }

  return !payloadUtf8.empty();
}

std::optional<std::pair<std::string, std::string>>
GetMediaSessionInfoFromReceiverStatus(const json& payload) {
  if (!payload.is_object()) {
    return std::nullopt;
  }

  const auto statusIt = payload.find("status");
  if (statusIt == payload.end() || !statusIt->is_object()) {
    return std::nullopt;
  }

  const auto appsIt = statusIt->find("applications");
  if (appsIt == statusIt->end() || !appsIt->is_array()) {
    return std::nullopt;
  }

  std::optional<std::pair<std::string, std::string>> fallback;
  for (const auto& app : *appsIt) {
    if (!app.is_object()) {
      continue;
    }
    const std::string transportId = app.value("transportId", "");
    const std::string sessionId = app.value("sessionId", "");
    if (transportId.empty() || sessionId.empty()) {
      continue;
    }

    const std::string appId = app.value("appId", "");
    if (appId == kDefaultMediaReceiverAppId) {
      return std::make_pair(transportId, sessionId);
    }

    if (!fallback.has_value()) {
      fallback = std::make_pair(transportId, sessionId);
    }
  }

  return fallback;
}

bool EnsureDefaultMediaReceiver(SecureTlsSocket& sock, std::string& selectedSourceId,
                                std::string& transportId, std::string& sessionId,
                                std::string& output, std::string& error) {
  const std::string source = "sender-0";
  const std::string receiver = "receiver-0";

  output += "trying senderId: " + source + "\n";

  if (!SendCastMessage(sock, source, receiver, kCastConnectionNamespace, MakeConnectPayload(),
                       error)) {
    error = "CONNECT failed: " + error;
    return false;
  }
  SendCastMessage(sock, source, receiver, kCastHeartbeatNamespace, json{{"type", "PING"}}, error);

  int statusRequestId = 1;
  SendCastMessage(sock, source, receiver, kCastReceiverNamespace,
                  json{{"type", "GET_STATUS"}, {"requestId", statusRequestId}}, error);

  bool launchRequested = false;
  bool channelClosed = false;
  auto nextPingAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto nextStatusAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto nextLaunchAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

  while (!channelClosed && std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();

    if (now >= nextPingAt) {
      SendCastMessage(sock, source, receiver, kCastHeartbeatNamespace, json{{"type", "PING"}},
                      error);
      nextPingAt = now + std::chrono::seconds(5);
    }

    if (now >= nextStatusAt) {
      SendCastMessage(sock, source, receiver, kCastReceiverNamespace,
                      json{{"type", "GET_STATUS"}, {"requestId", ++statusRequestId}}, error);
      nextStatusAt = now + std::chrono::seconds(2);
    }

    if (launchRequested && now >= nextLaunchAt) {
      SendCastMessage(sock, source, receiver, kCastReceiverNamespace,
                      json{{"type", "LAUNCH"},
                           {"appId", kDefaultMediaReceiverAppId},
                           {"requestId", ++statusRequestId}},
                      error);
      nextLaunchAt = now + std::chrono::seconds(3);
    }

    std::vector<uint8_t> frame;
    std::string readErr;
    if (!ReadCastFrame(sock, frame, 1000, readErr)) {
      // Only surface non-timeout errors
      if (readErr.find("Timed out") == std::string::npos) {
        output += "[read-err] " + readErr + "\n";
      }
      continue;
    }

    std::string payloadUtf8;
    std::string src;
    std::string dst;
    std::string castNs;
    if (!ParseCastMessagePayloadUtf8(frame, payloadUtf8, src, dst, castNs)) {
      output += "[parse-err] failed to parse frame (" + std::to_string(frame.size()) + " bytes)\n";
      continue;
    }

    // Log every received message for diagnostics
    {
      const std::string nsShort = castNs.size() > 30 ? castNs.substr(castNs.rfind('.') + 1) : castNs;
      const std::string preview = payloadUtf8.size() <= 250
                                      ? payloadUtf8
                                      : payloadUtf8.substr(0, 250) + "...";
      output += "[recv] ns=" + nsShort + " src=" + src + " dst=" + dst + "\n       " + preview + "\n";
    }

    json payload;
    try {
      payload = json::parse(payloadUtf8);
    } catch (...) {
      output += "[json-err] could not parse payload\n";
      continue;
    }

    const std::string type = payload.value("type", "");

    if (castNs == kCastHeartbeatNamespace) {
      if (type == "PING") {
        SendCastMessage(sock, source, receiver, kCastHeartbeatNamespace, json{{"type", "PONG"}},
                        error);
      }
      continue;
    }

    if (castNs == kCastConnectionNamespace) {
      if (type == "CLOSE") {
        output += "[close] connection namespace closed by device\n";
        channelClosed = true;
      }
      continue;
    }

    if (type == "RECEIVER_STATUS") {
      auto info = GetMediaSessionInfoFromReceiverStatus(payload);
      if (info.has_value()) {
        selectedSourceId = source;
        transportId = info->first;
        sessionId = info->second;
        output += "receiver ready: transportId=" + transportId + "\n";
        return true;
      }
      // No CC1AD845 app running yet — launch it
      if (!launchRequested) {
        launchRequested = true;
        output += "launching Default Media Receiver...\n";
        SendCastMessage(sock, source, receiver, kCastReceiverNamespace,
                        json{{"type", "LAUNCH"},
                             {"appId", kDefaultMediaReceiverAppId},
                             {"requestId", ++statusRequestId}},
                        error);
        nextLaunchAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      }
      continue;
    }

    if (type == "LAUNCH_ERROR") {
      output += "launch error: " + payload.value("reason", "unknown") + "\n";
      break;
    }
  }

  error = "Timed out waiting for Chromecast media receiver app.";
  return false;
}

std::string RenderTag(metadb_handle_ptr const& handle, const char* script) {
  pfc::string8 out;
  if (handle.is_valid() && handle->format_title_legacy(nullptr, out, script, nullptr)) {
    return Trim(static_cast<const char*>(out));
  }
  return "";
}

bool CollectPlaybackQueue(std::vector<metadb_handle_ptr>& outItems, double& outStartSeconds,
                          std::string& statusMessage) {
  outItems.clear();
  outStartSeconds = 0;
  statusMessage.clear();

  auto playback = playback_control::get();
  if (!playback->is_playing()) {
    statusMessage = "No track is currently playing.";
    return false;
  }

  metadb_handle_ptr nowPlaying;
  if (!playback->get_now_playing(nowPlaying) || nowPlaying.is_empty()) {
    statusMessage = "Could not resolve currently playing track.";
    return false;
  }

  outStartSeconds = playback->playback_get_position();
  if (!std::isfinite(outStartSeconds) || outStartSeconds < 0) {
    outStartSeconds = 0;
  }

  auto playlists = playlist_manager::get();
  t_size playlistIndex = 0;
  t_size itemIndex = 0;
  if (!playlists->get_playing_item_location(&playlistIndex, &itemIndex)) {
    outItems.push_back(nowPlaying);
    statusMessage = "Now playing item is not tied to a playlist. Casting current track only.";
    return true;
  }

  const t_size count = playlists->playlist_get_item_count(playlistIndex);
  const t_size limit = std::min<t_size>(count, itemIndex + kChromecastQueueWindow);
  for (t_size i = itemIndex; i < limit; ++i) {
    metadb_handle_ptr item;
    if (playlists->playlist_get_item_handle(item, playlistIndex, i) && item.is_valid()) {
      outItems.push_back(item);
    }
  }

  if (outItems.empty()) {
    outItems.push_back(nowPlaying);
    statusMessage = "Could not enumerate upcoming playlist items. Casting current track only.";
    return true;
  }

  if (outItems.front() != nowPlaying) {
    if (outItems.size() >= kChromecastQueueWindow) {
      outItems.pop_back();
    }
    outItems.insert(outItems.begin(), nowPlaying);
  }

  statusMessage = "Prepared current track + next up to 25 playlist items.";
  return true;
}

bool ResolveHandleToMedia(const ChromecastDeviceInfo& device, const std::string& bindIp,
                          metadb_handle_ptr const& handle, PreparedQueueItem& out,
                          std::string& output, std::string& error) {
  const char* rawPath = handle->get_path();
  if (rawPath == nullptr || *rawPath == 0) {
    error = "Track has empty path.";
    return false;
  }

  std::string source(rawPath);
  out.title = RenderTag(handle, "%title%");
  out.artist = RenderTag(handle, "%artist%");
  out.album = RenderTag(handle, "%album%");
  if (out.title.empty()) {
    out.title = Basename(source);
  }

  const double length = handle->get_length();
  if (std::isfinite(length) && length > 0) {
    out.durationSec = length;
  }

  if (StartsWithHttp(source)) {
    out.url = source;
    out.mimeType = GetMimeType(source);
    return true;
  }

  std::string localPath = source;
  if (ToLower(localPath).rfind("file://", 0) == 0) {
    localPath = localPath.substr(7);
    localPath = UrlDecode(localPath);
  }

  if (!localPath.empty() && localPath[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      localPath = std::string(home) + localPath.substr(1);
    }
  }

  std::ifstream test(localPath, std::ios::binary);
  if (!test) {
    error = "Media file not found: " + localPath;
    return false;
  }

  auto server = std::make_shared<LocalFileHttpServer>(localPath, bindIp);
  if (!server) {
    error = "Failed to initialize local file server.";
    return false;
  }

  if (!server->Start(error)) {
    return false;
  }

  out.url = server->MediaUrl();
  out.mimeType = server->mimeType();

  {
    std::lock_guard<std::mutex> lock(g_serversMutex);
    g_servers.push_back(server);
  }
  return true;
}

bool RunChromecastCast(const ChromecastDeviceInfo& device,
                       const std::vector<metadb_handle_ptr>& queueItems,
                       double startPositionSeconds, std::string& output) {
  output.clear();

  if (queueItems.empty()) {
    output = "No items to cast.";
    return false;
  }

  std::string error;

  const std::string bindIp = GetLocalIpForTarget(device.host);
  if (bindIp.empty()) {
    output += "Could not determine local LAN IP reachable by Chromecast.";
    return false;
  }

  // Pre-resolve a bounded queue before Cast app negotiation so we can send
  // immediately after receiver readiness and avoid huge payloads.
  ClearLocalServers();
  json items = json::array();
  size_t resolvedCount = 0;
  bool queueTruncated = false;

  for (size_t i = 0; i < queueItems.size(); ++i) {
    if (resolvedCount >= kMaxQueueLoadItems) {
      queueTruncated = true;
      break;
    }

    PreparedQueueItem prepared;
    if (!ResolveHandleToMedia(device, bindIp, queueItems[i], prepared, output, error)) {
      ClearLocalServers();
      output += error;
      return false;
    }

    json metadata = {
        {"metadataType", 3},
        {"title", prepared.title},
    };
    if (!prepared.artist.empty()) {
      metadata["artist"] = prepared.artist;
    }
    if (!prepared.album.empty()) {
      metadata["albumName"] = prepared.album;
    }

    json media = {
        {"contentId", prepared.url},
        {"streamType", "BUFFERED"},
        {"contentType", prepared.mimeType},
        {"metadata", metadata},
    };
    if (prepared.durationSec > 0) {
      media["duration"] = prepared.durationSec;
    }

    json item = {
        {"autoplay", true},
        {"preloadTime", 20},
        {"media", media},
    };
    if (resolvedCount == 0 && startPositionSeconds > 0) {
      item["startTime"] = startPositionSeconds;
    }

    items.push_back(item);

    // Keep the queued payload below a conservative limit for older devices.
    json probe = {
        {"type", "QUEUE_LOAD"},
        {"requestId", 3},
        {"sessionId", "probe"},
        {"startIndex", 0},
        {"currentTime", std::max(0.0, startPositionSeconds)},
        {"items", items},
    };
    if (probe.dump().size() > kMaxQueueLoadJsonBytes) {
      items.erase(std::prev(items.end()));
      queueTruncated = true;
      break;
    }

    ++resolvedCount;
  }

  if (items.empty()) {
    ClearLocalServers();
    output += "No queue items could be prepared within Chromecast payload limits.";
    return false;
  }

  output += "prepared queue items: " + std::to_string(items.size()) +
            (queueTruncated ? " (truncated)\n" : "\n");

  SecureTlsSocket sock;
  if (!sock.Connect(device.host, device.port, error)) {
    ClearLocalServers();
    output = error;
    return false;
  }

  output += "connected: " + device.displayName + " (" + device.host + ":" +
            std::to_string(device.port) + ")\n";

  std::string sourceId;

  std::string transportId;
  std::string sessionId;
  if (!EnsureDefaultMediaReceiver(sock, sourceId, transportId, sessionId, output, error)) {
    ClearLocalServers();
    output += error;
    return false;
  }

  if (!SendCastMessage(sock, sourceId, transportId, kCastConnectionNamespace,
                       MakeConnectPayload(), error)) {
    ClearLocalServers();
    output += error;
    return false;
  }

  json queueLoad = {
      {"type", "QUEUE_LOAD"},
      {"requestId", 3},
      {"sessionId", sessionId},
      {"startIndex", 0},
      {"currentTime", std::max(0.0, startPositionSeconds)},
      {"items", items},
  };

  if (!SendCastMessage(sock, sourceId, transportId, kCastMediaNamespace, queueLoad, error)) {
    const std::string queueError = error;

    // Fallback: some older Chromecast firmware rejects larger QUEUE_LOAD
    // messages; a plain LOAD for the first item keeps playback working.
    json loadPayload = {
        {"type", "LOAD"},
        {"requestId", 4},
        {"sessionId", sessionId},
        {"autoplay", true},
        {"currentTime", std::max(0.0, startPositionSeconds)},
        {"media", items.front().at("media")},
    };

    if (!SendCastMessage(sock, sourceId, transportId, kCastMediaNamespace, loadPayload, error)) {
      ClearLocalServers();
      output += queueError + "\n";
      output += "LOAD fallback failed: " + error;
      return false;
    }

    output += queueError + "\n";
    output += "QUEUE_LOAD failed; started first item via LOAD fallback.\n";
    output += "queue items: 1 (fallback)\n";
    output += "start position: " + std::to_string(std::max(0.0, startPositionSeconds)) + "\n";
    output += "device: " + device.displayName + "\n";
    return true;
  }

  output += "queue items: " + std::to_string(items.size()) + "\n";
  output += "start position: " + std::to_string(std::max(0.0, startPositionSeconds)) + "\n";
  output += "device: " + device.displayName + "\n";
  return true;
}

std::optional<int> ExtractMediaSessionIdFromMediaStatus(const json& payload) {
  if (!payload.is_object() || payload.value("type", "") != "MEDIA_STATUS") {
    return std::nullopt;
  }

  const auto statusIt = payload.find("status");
  if (statusIt == payload.end() || !statusIt->is_array() || statusIt->empty()) {
    return std::nullopt;
  }

  const auto& first = (*statusIt)[0];
  if (!first.is_object()) {
    return std::nullopt;
  }

  const auto mediaSessionIt = first.find("mediaSessionId");
  if (mediaSessionIt == first.end()) {
    return std::nullopt;
  }
  if (mediaSessionIt->is_number_integer()) {
    return mediaSessionIt->get<int>();
  }
  if (mediaSessionIt->is_number_float()) {
    return static_cast<int>(mediaSessionIt->get<double>());
  }

  return std::nullopt;
}

bool RunChromecastMediaControl(const ChromecastDeviceInfo& device, const std::string& commandType,
                               std::string& output,
                               std::optional<double> currentTime = std::nullopt) {
  output.clear();

  SecureTlsSocket sock;
  std::string error;
  if (!sock.Connect(device.host, device.port, error)) {
    output = error;
    return false;
  }

  output += "connected: " + device.displayName + " (" + device.host + ":" +
            std::to_string(device.port) + ")\n";

  std::string sourceId;
  std::string transportId;
  std::string sessionId;
  if (!EnsureDefaultMediaReceiver(sock, sourceId, transportId, sessionId, output, error)) {
    output += error;
    return false;
  }

  if (!SendCastMessage(sock, sourceId, transportId, kCastConnectionNamespace,
                       MakeConnectPayload(), error)) {
    output += error;
    return false;
  }

  if (!SendCastMessage(sock, sourceId, transportId, kCastMediaNamespace,
                       json{{"type", "GET_STATUS"}, {"requestId", 9001}}, error)) {
    output += error;
    return false;
  }

  std::optional<int> mediaSessionId;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<uint8_t> frame;
    std::string readErr;
    if (!ReadCastFrame(sock, frame, 800, readErr)) {
      if (readErr.find("Timed out") == std::string::npos) {
        output += "[read-err] " + readErr + "\n";
      }
      continue;
    }

    std::string payloadUtf8;
    std::string src;
    std::string dst;
    std::string castNs;
    if (!ParseCastMessagePayloadUtf8(frame, payloadUtf8, src, dst, castNs)) {
      continue;
    }

    if (castNs != kCastMediaNamespace) {
      continue;
    }

    try {
      const json payload = json::parse(payloadUtf8);
      mediaSessionId = ExtractMediaSessionIdFromMediaStatus(payload);
      if (mediaSessionId.has_value()) {
        break;
      }
    } catch (...) {
      continue;
    }
  }

  if (!mediaSessionId.has_value()) {
    output += "No active Chromecast media session found.";
    return false;
  }

  json controlPayload = {
      {"type", commandType},
      {"requestId", 9002},
      {"mediaSessionId", *mediaSessionId},
  };
  if (currentTime.has_value()) {
    controlPayload["currentTime"] = std::max(0.0, *currentTime);
    if (commandType == "SEEK") {
      controlPayload["resumeState"] = "PLAYBACK_START";
    }
  }

  if (!SendCastMessage(sock, sourceId, transportId, kCastMediaNamespace, controlPayload, error)) {
    output += error;
    return false;
  }

  output += "sent command: " + commandType + "\n";
  output += "device: " + device.displayName + "\n";
  return true;
}

void RunChromecastMediaControlAsync(const ChromecastDeviceInfo& device,
                                    const std::string& commandType,
                                    const std::string& commandName,
                                    bool showSuccessPopup = true,
                                    std::optional<double> currentTime = std::nullopt) {
  std::thread([device, commandType, commandName, showSuccessPopup, currentTime]() {
    std::string output;
    const bool ok = RunChromecastMediaControl(device, commandType, output, currentTime);

    if (g_componentShuttingDown.load(std::memory_order_relaxed)) {
      return;
    }

    fb2k::inMainThread([ok, device, commandName, output, showSuccessPopup]() {
      if (g_componentShuttingDown.load(std::memory_order_relaxed)) {
        return;
      }
      if (ok) {
        if (showSuccessPopup) {
          pfc::string_formatter msg;
          msg << "Chromecast " << commandName.c_str() << " sent.\n\nDevice: "
              << device.displayName.c_str();
          if (!output.empty()) {
            msg << "\n\nOutput:\n" << output.c_str();
          }
          popup_message::g_show(msg, "Chromecast");
        }
      } else if (showSuccessPopup) {
        pfc::string_formatter msg;
        msg << "Failed to send Chromecast command.\n\n";
        if (!output.empty()) {
          msg << output.c_str();
        } else {
          msg << "Native CastV2 command session failed.";
        }
        popup_message::g_show(msg, "Chromecast Error");
      }
    });
  }).detach();
}

void RequestChromecastQueueReloadForDevice(const ChromecastDeviceInfo& device, bool showPopup) {
  const uint64_t serial = g_castReloadSerial.fetch_add(1, std::memory_order_relaxed) + 1;

  fb2k::inMainThread([device, showPopup, serial]() {
    if (g_componentShuttingDown.load(std::memory_order_relaxed)) {
      return;
    }

    std::vector<metadb_handle_ptr> queueItems;
    double startSeconds = 0;
    std::string queueStatus;
    if (!CollectPlaybackQueue(queueItems, startSeconds, queueStatus)) {
      if (showPopup) {
        popup_message::g_show(queueStatus.c_str(), "Chromecast Error");
      }
      return;
    }

    std::thread([device, queueItems, startSeconds, queueStatus, showPopup, serial]() {
      std::lock_guard<std::mutex> castLock(g_castActionMutex);
      if (g_componentShuttingDown.load(std::memory_order_relaxed) ||
          g_castReloadSerial.load(std::memory_order_relaxed) != serial) {
        return;
      }

      std::string output;
      bool ok = RunChromecastCast(device, queueItems, startSeconds, output);

      if (!ok && !queueItems.empty()) {
        std::string fallbackOutput;
        std::vector<metadb_handle_ptr> singleItem;
        singleItem.push_back(queueItems.front());
        if (RunChromecastCast(device, singleItem, startSeconds, fallbackOutput)) {
          ok = true;
          output += "\nSingle-item fallback succeeded.\n";
          output += fallbackOutput;
        }
      }

      if (g_componentShuttingDown.load(std::memory_order_relaxed) ||
          g_castReloadSerial.load(std::memory_order_relaxed) != serial) {
        return;
      }

      fb2k::inMainThread([ok, device, queueItems, startSeconds, queueStatus, output, showPopup]() {
        if (g_componentShuttingDown.load(std::memory_order_relaxed)) {
          return;
        }

        if (ok) {
          SetActiveCastDevice(device);
          RegisterPlaybackBridge();
          MuteLocalPlaybackForChromecast();
        }

        if (!showPopup) {
          return;
        }

        if (!ok) {
          pfc::string_formatter msg;
          msg << "Failed to sync Chromecast playback.\n\n";
          if (!output.empty()) {
            msg << output.c_str();
          }
          popup_message::g_show(msg, "Chromecast Error");
          return;
        }

        pfc::string_formatter msg;
        msg << "Chromecast enabled.\n\nDevice: " << device.displayName.c_str()
            << "\nQueue items: " << static_cast<unsigned>(queueItems.size())
            << "\nStart position: " << pfc::format_time_ex(startSeconds, 1);
        if (!queueStatus.empty()) {
          msg << "\n" << queueStatus.c_str();
        }
        if (!output.empty()) {
          msg << "\n\nOutput:\n" << output.c_str();
        }
        popup_message::g_show(msg, "Chromecast");
      });
    }).detach();
  });
}

void RequestActiveChromecastQueueReload(bool showPopup) {
  ChromecastDeviceInfo device;
  if (!TryGetActiveCastDevice(device)) {
    return;
  }
  RequestChromecastQueueReloadForDevice(device, showPopup);
}

void DisableChromecastSession(bool showPopup, bool sendStop) {
  g_castReloadSerial.fetch_add(1, std::memory_order_relaxed);

  ChromecastDeviceInfo device;
  const bool hadDevice = TryGetActiveCastDevice(device);

  UnregisterPlaybackBridge();
  ClearActiveCastDevice();
  RestoreLocalPlaybackVolume();

  ClearLocalServers();

  if (hadDevice && sendStop && !g_componentShuttingDown.load(std::memory_order_relaxed)) {
    RunChromecastMediaControlAsync(device, "STOP", "Disable", false);
  }

  if (showPopup) {
    popup_message::g_show("Chromecast disabled.", "Chromecast");
  }
}

class PlaybackMenuCommands : public mainmenu_commands {
 public:
  uint32_t get_command_count() override { return 1; }

  GUID get_command(uint32_t index) override { return index == 0 ? kMenuToggleGuid : pfc::guid_null; }

  void get_name(uint32_t index, pfc::string_base& out) override {
    if (index == 0) {
      out = HasActiveCastDevice() ? "Disable Chromecast" : "Enable Chromecast";
    }
  }

  bool get_description(uint32_t index, pfc::string_base& out) override {
    if (index != 0) {
      return false;
    }
    out = HasActiveCastDevice()
              ? "Disable Chromecast session control from foobar2000."
              : "Start the current track on Chromecast and queue the next 25 tracks.";
    return true;
  }

  GUID get_parent() override { return mainmenu_groups::playback; }

  bool get_display(uint32_t index, pfc::string_base& out, uint32_t& flags) override {
    if (index != 0) {
      return false;
    }
    flags = 0;
    get_name(index, out);
    if (HasActiveCastDevice()) {
      flags |= mainmenu_commands::flag_checked;
    }
    return true;
  }

  void execute(uint32_t index, service_ptr_t<service_base> p_callback) override {
    (void)p_callback;
    if (index != 0) {
      return;
    }

    if (HasActiveCastDevice()) {
      DisableChromecastSession(true, true);
      return;
    }

    std::string deviceName = static_cast<const char*>(g_lastChromecastDevice.get());
    std::string discoveryStatus;
    std::vector<ChromecastDeviceInfo> discovered = DiscoverChromecastDevices(discoveryStatus);

    if (!PromptChromecastDevice(deviceName, discovered, discoveryStatus)) {
      return;
    }

    if (deviceName.empty()) {
      popup_message::g_show("Chromecast device name cannot be empty.", "Chromecast");
      return;
    }

    auto selected = FindDeviceByName(discovered, deviceName);
    if (!selected.has_value()) {
      std::string refreshStatus;
      discovered = DiscoverChromecastDevices(refreshStatus);
      selected = FindDeviceByName(discovered, deviceName);
      if (!selected.has_value()) {
        pfc::string_formatter msg;
        msg << "Could not resolve Chromecast device: " << deviceName.c_str()
            << "\nTry selecting one from the dropdown after discovery.";
        popup_message::g_show(msg, "Chromecast Error");
        return;
      }
    }

    g_lastChromecastDevice = deviceName.c_str();
    RequestChromecastQueueReloadForDevice(*selected, true);
  }
};

class ChromecastPlaybackBridgeDynamic : public play_callback {
 public:
  void on_playback_starting(play_control::t_track_command, bool) override {}

  void on_playback_new_track(metadb_handle_ptr) override {
    RequestActiveChromecastQueueReload(false);
  }

  void on_playback_pause(bool p_state) override {
    ChromecastDeviceInfo device;
    if (!TryGetActiveCastDevice(device)) {
      return;
    }

    RunChromecastMediaControlAsync(device, p_state ? "PAUSE" : "PLAY", p_state ? "Pause" : "Play",
                                   false);
  }

  void on_playback_seek(double timeSeconds) override {
    ChromecastDeviceInfo device;
    if (!TryGetActiveCastDevice(device)) {
      return;
    }

    RunChromecastMediaControlAsync(device, "SEEK", "Seek", false,
                                   std::max(0.0, timeSeconds));
  }

  void on_playback_stop(play_control::t_stop_reason p_reason) override {
    if (p_reason != play_control::stop_reason_user) {
      return;
    }

    ChromecastDeviceInfo device;
    if (!TryGetActiveCastDevice(device)) {
      return;
    }

    RunChromecastMediaControlAsync(device, "STOP", "Stop", false);
  }

  void on_playback_edited(metadb_handle_ptr) override {}
  void on_playback_dynamic_info(const file_info&) override {}
  void on_playback_dynamic_info_track(const file_info&) override {}
  void on_playback_time(double) override {}
  void on_volume_change(float) override {}
};

void RegisterPlaybackBridge() {
  std::lock_guard<std::mutex> lock(g_playbackBridgeMutex);
  if (g_playbackBridgeRegistered) {
    return;
  }

  if (!g_playbackBridge) {
    g_playbackBridge = std::make_unique<ChromecastPlaybackBridgeDynamic>();
  }

  play_callback_manager::get()->register_callback(
      g_playbackBridge.get(), play_callback::flag_on_playback_new_track |
                                  play_callback::flag_on_playback_seek |
                                  play_callback::flag_on_playback_pause |
                                  play_callback::flag_on_playback_stop,
      false);
  g_playbackBridgeRegistered = true;
}

void UnregisterPlaybackBridge() {
  std::lock_guard<std::mutex> lock(g_playbackBridgeMutex);
  if (!g_playbackBridgeRegistered || !g_playbackBridge) {
    return;
  }
  play_callback_manager::get()->unregister_callback(g_playbackBridge.get());
  g_playbackBridgeRegistered = false;
}

class ChromecastInitQuit : public initquit {
 public:
  void on_quit() override {
    g_componentShuttingDown.store(true, std::memory_order_relaxed);
    DisableChromecastSession(false, false);
  }
};

static mainmenu_commands_factory_t<PlaybackMenuCommands> g_playback_menu_factory;
static initquit_factory_t<ChromecastInitQuit> g_chromecast_initquit_factory;

DECLARE_COMPONENT_VERSION("Chromecast Audio Sender (native)", "0.3.22",
                          "Cast currently playing track from current position and continue "
                          "upcoming playlist items using native Bonjour and CastV2 TLS.");

}  // namespace
