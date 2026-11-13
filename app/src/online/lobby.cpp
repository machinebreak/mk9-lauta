// MK9 online: the lobby (lobby.h).

#include "online/lobby.h"
#include "online/profiles.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
// mk9-lauta (2026-11-13): Linux: libcurl (curl_http.h) where Windows has WinHTTP
#include "mk9_platform.h"
#include "online/curl_http.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mk9_lobby_url, "wss://107-172-143-35.sslip.io/lobby?game=mk9pc", "MK9 Online",
                      "The online lobby: rooms, the global chat, quick match (ws:// or wss://; \"off\" = none: tests)");
REXCVAR_DEFINE_BOOL(mk9_lobby_by_address, false, "MK9 Online",
                    "The lobby at its address from the start (a session's game: the game that started it found that "
                    "this connection's DNS does not know the lobby's name)");

namespace mk9::lobby {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxChat = 200;
constexpr auto kPingEvery = std::chrono::seconds(25);

// Once the DNS failed for the lobby's name, every connection after goes straight to its address (no 8 s wait first).
std::atomic<bool> g_by_address{false};

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

struct Url {
  bool ok = false, secure = false;
  std::wstring host, path;  // path with its query
  INTERNET_PORT port = 0;
};

Url Parse(const std::string& text) {
  Url url;
  std::string s = text;
  // WinHttpCrackUrl knows http(s) only: ws(s) is the same URL otherwise
  if (s.rfind("wss://", 0) == 0) s = "https://" + s.substr(6);
  if (s.rfind("ws://", 0) == 0) s = "http://" + s.substr(5);
  std::wstring w = Wide(s);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &parts)) return url;
  url.ok = true;
  url.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  url.host = host;
  url.port = parts.nPort;
  url.path = std::wstring(path) + extra;
  return url;
}

// mk9-lauta (2026-11-11): the address a name like the lobby's carries ("107-172-143-35.sslip.io" -> "107.172.143.35");
// "" for any other name. Some providers' DNS (Claro's, a player's diagnosis: WinHTTP 12007) does not answer for
// sslip.io: the game then goes to that address without asking the DNS.
std::wstring AddressInName(const std::wstring& host) {
  std::wstring out;
  size_t i = 0;
  for (int part = 0; part < 4; ++part) {
    size_t digits = 0;
    unsigned value = 0;
    while (i < host.size() && host[i] >= L'0' && host[i] <= L'9' && digits < 3) {
      value = value * 10 + unsigned(host[i] - L'0');
      ++i;
      ++digits;
    }
    if (digits == 0 || value > 255 || i >= host.size() || host[i] != (part < 3 ? L'-' : L'.')) return {};
    ++i;
    out += std::to_wstring(value);
    if (part < 3) out += L'.';
  }
  return out;
}


// mk9-lauta (2026-11-12): the lobby at its address with the name kept, on every Windows. A player on 0.5.0 still had
// "dns" (12007): WINHTTP_OPTION_RESOLUTION_HOSTNAME is not in every Windows. Instead WinHTTP goes through a proxy of
// this game's own on 127.0.0.1: it asks it for the name (CONNECT name:443, or an absolute http:// URL), so it never
// asks the DNS; the proxy connects to the address the name carries (AddressInName; no other name) and passes the
// bytes as they are. TLS stays WinHTTP's, end to end: the server name (SNI), the certificate's check and Host.
std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }  // ASCII names and numbers

bool SendAll(SOCKET socket, const char* data, size_t size) {
  while (size) {
    const int n = send(socket, data, int(std::min<size_t>(size, 1 << 16)), 0);
    if (n <= 0) return false;
    data += n;
    size -= size_t(n);
  }
  return true;
}

// One way of a tunnel: what 'from' sends goes to 'to'; at the end 'to' is told nothing more comes.
void Pass(SOCKET from, SOCKET to) {
  char buffer[16384];
  for (;;) {
    const int n = recv(from, buffer, int(sizeof(buffer)), 0);
    if (n <= 0 || !SendAll(to, buffer, size_t(n))) break;
  }
  shutdown(to, SD_SEND);
}

void Refuse(SOCKET client, const char* why) {
  REXLOG_WARN("[lobby] the local proxy refused a request: {}", why);
  static const char kAnswer[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  SendAll(client, kAnswer, sizeof(kAnswer) - 1);
  closesocket(client);
}

void ServeProxy(SOCKET client) {
  std::string head;
  char buffer[4096];
  while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384) {
    const int n = recv(client, buffer, int(sizeof(buffer)), 0);
    if (n <= 0) {
      closesocket(client);
      return;
    }
    head.append(buffer, size_t(n));
  }
  const size_t line_end = head.find("\r\n");
  if (head.find("\r\n\r\n") == std::string::npos || line_end == std::string::npos) return Refuse(client, "no header");
  const std::string line = head.substr(0, line_end);
  const size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
  if (b == std::string::npos) return Refuse(client, "a request line without a target");
  const std::string method = line.substr(0, a), target = line.substr(a + 1, b - a - 1), version = line.substr(b + 1);
  const bool tunnel = method == "CONNECT";
  std::string authority = target, path = "/";
  uint16_t port = tunnel ? 443 : 80;
  if (!tunnel) {
    if (target.rfind("http://", 0) != 0) return Refuse(client, "not an http:// URL");
    authority = target.substr(7);
    if (const size_t slash = authority.find('/'); slash != std::string::npos) {
      path = authority.substr(slash);
      authority.resize(slash);
    }
  }
  if (const size_t colon = authority.rfind(':'); colon != std::string::npos) {
    port = uint16_t(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
    authority.resize(colon);
  }
  const std::string address = Narrow(AddressInName(std::wstring(authority.begin(), authority.end())));
  if (address.empty() || port == 0) return Refuse(client, "a name with no address in it");
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  SOCKET server = INVALID_SOCKET;
  if (inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1 ||
      (server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET ||
      connect(server, reinterpret_cast<const sockaddr*>(&to), int(sizeof(to))) != 0) {
    const int error = WSAGetLastError();
    if (server != INVALID_SOCKET) closesocket(server);
    REXLOG_WARN("[lobby] the local proxy could not reach {}:{} (Winsock error {})", address, port, error);
    return Refuse(client, "the address did not answer");
  }
  bool ok;
  if (tunnel) {
    static const char kOpen[] = "HTTP/1.1 200 Connection established\r\n\r\n";
    const std::string rest = head.substr(head.find("\r\n\r\n") + 4);
    ok = SendAll(client, kOpen, sizeof(kOpen) - 1) && SendAll(server, rest.data(), rest.size());
  } else {
    const std::string request = method + " " + path + " " + version + head.substr(line_end);
    ok = SendAll(server, request.data(), request.size());
  }
  if (ok) {
    std::thread up([client, server] { Pass(client, server); });
    Pass(server, client);
    up.join();
  }
  closesocket(server);
  closesocket(client);
}

// The proxy's "127.0.0.1:<port>" (started the first time), or "" when it could not start.
std::wstring LocalProxy() {
  static std::mutex mutex;
  static uint16_t port = 0;
  static bool failed = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (!port && !failed) {
    WSADATA data;
    SOCKET listener = WSAStartup(MAKEWORD(2, 2), &data) == 0 ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
                                                               : INVALID_SOCKET;
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = int(sizeof(at));
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<const sockaddr*>(&at), size) != 0 ||
        listen(listener, 8) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&at), &size) != 0) {
      REXLOG_ERROR("[lobby] the local proxy could not start (Winsock error {})", WSAGetLastError());
      if (listener != INVALID_SOCKET) closesocket(listener);
      failed = true;
    } else {
      port = ntohs(at.sin_port);
      std::thread([listener] {
        for (;;) {
          const SOCKET client = accept(listener, nullptr, nullptr);
          if (client == INVALID_SOCKET) return;
          std::thread(ServeProxy, client).detach();
        }
      }).detach();
      REXLOG_INFO("[lobby] local proxy on 127.0.0.1:{}: the lobby's connections go to its address (no DNS)", port);
    }
  }
  return port ? L"127.0.0.1:" + std::to_wstring(port) : std::wstring();
}

// A WinHTTP session: as always, or (by_address) through the local proxy.
HINTERNET OpenSession(bool by_address) {
  if (by_address) {
    const std::wstring proxy = LocalProxy();
    if (!proxy.empty()) {
      return WinHttpOpen(L"mk9-online/1.0", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    }
  }
  return WinHttpOpen(L"mk9-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                     WINHTTP_NO_PROXY_BYPASS, 0);
}

// What a failed connection was, for the screens: "dns" (the name not found), "connect" (no answer, refused: a firewall,
// an antivirus, the provider), "timeout", "tls" (the secure connection: the PC's date, its certificates), "http" (the
// server answered something else), "other".
std::string Why(DWORD error, DWORD status) {
  switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "dns";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connect";
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: return "tls";
    default: break;
  }
  if (status && status != 101) return "http";
  return "other";
}
#else
// mk9-lauta (2026-11-13): Linux. The lobby's URL (ws:// or wss://): its parts.
struct Url {
  bool ok = false, secure = false;
  std::string host, path;  // path with its query
  long port = 0;
};

Url Parse(const std::string& text) {
  Url url;
  const size_t colon = text.find("://");
  if (colon == std::string::npos) return url;
  const std::string scheme = text.substr(0, colon);
  if (scheme == "wss" || scheme == "https") {
    url.secure = true;
  } else if (scheme != "ws" && scheme != "http") {
    return url;
  }
  const size_t host_at = colon + 3;
  size_t path_at = text.find_first_of("/?", host_at);
  if (path_at == std::string::npos) path_at = text.size();
  std::string authority = text.substr(host_at, path_at - host_at);
  url.path = path_at < text.size() ? text.substr(path_at) : "/";
  if (url.path[0] == '?') url.path = "/" + url.path;
  url.port = url.secure ? 443 : 80;
  if (const size_t c = authority.rfind(':'); c != std::string::npos) {
    url.port = std::strtol(authority.c_str() + c + 1, nullptr, 10);
    authority.resize(c);
  }
  url.host = authority;
  url.ok = !url.host.empty() && url.port > 0 && url.port < 65536;
  return url;
}

// The address a name like the lobby's carries ("107-172-143-35.sslip.io" -> "107.172.143.35"); "" for any other name
// (lobby.cpp's Windows AddressInName, in bytes).
std::string AddressInName(const std::string& host) {
  std::string out;
  size_t i = 0;
  for (int part = 0; part < 4; ++part) {
    size_t digits = 0;
    unsigned value = 0;
    while (i < host.size() && host[i] >= '0' && host[i] <= '9' && digits < 3) {
      value = value * 10 + unsigned(host[i] - '0');
      ++i;
      ++digits;
    }
    if (digits == 0 || value > 255 || i >= host.size() || host[i] != (part < 3 ? '-' : '.')) return {};
    ++i;
    out += std::to_string(value);
    if (part < 3) out += '.';
  }
  return out;
}

// The URL of a GET on the lobby's host (its /ice): the same scheme as the lobby's, http(s).
std::string HttpUrl(const Url& url, const std::string& path) {
  return std::string(url.secure ? "https://" : "http://") + url.host + ":" + std::to_string(url.port) + path;
}
#endif

class Client {
 public:
  void Start(const std::string& name) {
    std::unique_lock<std::mutex> lock(mutex_);
    wanted_name_ = name;
    if (running_) {
      lock.unlock();
      Send({{"t", "name"}, {"name", name}});
      return;
    }
    running_ = true;
    stopping_ = false;
    state_.link = Link::kConnecting;
    ++state_.version;
    worker_ = std::thread([this] { Run(); });
    sender_ = std::thread([this] { SendLoop(); });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) return;
      stopping_ = true;
    }
    send_cv_.notify_all();
    CloseSocket();
    if (worker_.joinable()) worker_.join();
    if (sender_.joinable()) sender_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    state_.link = Link::kOff;
    ++state_.version;
  }

  State Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  // mutex_ held: a message for the sending thread.
  void PushLocked(const json& message) {
    outbox_.push_back(message.dump());
    if (outbox_.size() > 64) outbox_.pop_front();
  }

  // mutex_ held: the join of the room this game is a guest of: the one joined, else its profile room's guest "pf".
  json JoinLocked() const {
    if (!guest_room_.empty()) return {{"t", "join"}, {"room", guest_room_}, {"id", guest_id_}};
    return {{"t", "join"}, {"room", pf_room_}, {"id", pf_room_.empty() ? "" : "pf"}};
  }

  void Send(const json& message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) return;
      // what was asked while offline goes once the link is up; a hello is said at each connection anyway
      outbox_.push_back(message.dump());
      if (outbox_.size() > 64) outbox_.pop_front();
    }
    send_cv_.notify_one();
  }

  void SetCountry(const std::string& country) {
    std::string name;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (country_ == country) return;
      country_ = country;
      name = wanted_name_;
      if (!running_ || name.empty()) return;
    }
    Send({{"t", "name"}, {"name", name}, {"country", country}});
  }

  bool PollEvent(Event* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    *event = std::move(events_.front());
    events_.pop_front();
    return true;
  }

  bool PollIce(Event* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ice_.empty()) return false;
    *event = std::move(ice_.front());
    ice_.pop_front();
    return true;
  }

  // mk9-lauta (2026-11-11): the profiles' messages (profiles.h), apart.
  bool PollProfile(Event* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profiles_.empty()) return false;
    *event = std::move(profiles_.front());
    profiles_.pop_front();
    return true;
  }

  // The room this game is a guest of (the room's flow: "" leaves it). mk9-lauta (2026-11-11): kept, so a link that
  // comes back joins it again, and with none the game is the guest of its own profile room (profiles.h).
  void Join(const std::string& room, const std::string& id) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      guest_room_ = room;
      guest_id_ = id;
      if (!running_) return;
      PushLocked(JoinLocked());
    }
    send_cv_.notify_one();
  }

  // mk9-lauta (2026-11-13): the room's STUN and TURN servers, which a player behind a provider's NAT needs to reach
  // the other one at all (the relay). The session's game asked for them as it started, while its first try at the
  // lobby was still finding out that the DNS does not know the lobby's name (Claro and others): /ice went by name,
  // failed, and the session ran with no STUN and no TURN ("STUN none, 0 TURN server(s)": 30 s of "path not yet",
  // "se fue de la sesión" on the other side). Now /ice goes to the lobby's address too when the name is not found
  // (the game that relaunched it says so from the start: mk9_lobby_by_address), tries again once, and the last
  // answer that came is kept for when none does (Cloudflare's credentials last 12 hours; the lobby keeps an answer
  // an hour).
#if defined(_WIN32)
  json IceServers() {
    static const bool by_address_from_start = REXCVAR_GET(mk9_lobby_by_address);
    if (by_address_from_start) g_by_address = true;
    const Url url = Parse(REXCVAR_GET(mk9_lobby_url));
    if (!url.ok) return json::array();
    const std::wstring address = AddressInName(url.host);
    std::string why;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::seconds(1));
      DWORD error = 0;
      json out = FetchIce(url, g_by_address, &error);
      if (!out.empty()) return KeepIce(out);
      if (!g_by_address && !address.empty() && error == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
        out = FetchIce(url, true, &error);
        if (!out.empty()) {
          g_by_address = true;
          REXLOG_WARN("[ice] this connection's DNS does not find the lobby's name: its STUN and TURN servers asked "
                      "at its address");
          return KeepIce(out);
        }
      }
      why = error ? Why(error, 0) : "no answer";
    }
    json kept = KeptIce();
    REXLOG_WARN("[ice] the lobby did not give its STUN and TURN servers ({}): {}", why,
                kept.empty() ? "none (the connection may not go through)" : "the ones kept from before");
    return kept;
  }
#else
  json IceServers() {
    static const bool by_address_from_start = REXCVAR_GET(mk9_lobby_by_address);
    if (by_address_from_start) g_by_address = true;
    const Url url = Parse(REXCVAR_GET(mk9_lobby_url));
    if (!url.ok) return json::array();
    const std::string address = AddressInName(url.host);
    std::string why;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::seconds(1));
      std::string kind;
      json out = FetchIce(url, g_by_address && !address.empty() ? address : std::string(), &kind);
      if (!out.empty()) return KeepIce(out);
      if (!g_by_address && !address.empty() && kind == "dns") {
        out = FetchIce(url, address, &kind);
        if (!out.empty()) {
          g_by_address = true;
          REXLOG_WARN("[ice] this connection's DNS does not find the lobby's name: its STUN and TURN servers asked "
                      "at its address");
          return KeepIce(out);
        }
      }
      why = kind.empty() ? "no answer" : kind;
    }
    json kept = KeptIce();
    REXLOG_WARN("[ice] the lobby did not give its STUN and TURN servers ({}): {}", why,
                kept.empty() ? "none (the connection may not go through)" : "the ones kept from before");
    return kept;
  }
#endif

 private:
#if defined(_WIN32)
  // One GET of the lobby's /ice (by name, or by_address through the local proxy): the servers, or an empty array
  // (error: WinHTTP's error, 0 when the answer was not a list).
  json FetchIce(const Url& url, bool by_address, DWORD* error) {
    *error = 0;
    HINTERNET session = OpenSession(by_address);
    if (!session) {
      *error = GetLastError();
      return json::array();
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    json out = json::array();
    if (HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0)) {
      if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/ice", nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0)) {
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
          std::string body;
          DWORD available = 0;
          while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
            body.append(chunk.data(), read);
          }
          out = json::parse(body, nullptr, false);
          if (!out.is_array()) out = json::array();
        } else {
          *error = GetLastError();
        }
        WinHttpCloseHandle(request);
      } else {
        *error = GetLastError();
      }
      WinHttpCloseHandle(connect);
    } else {
      *error = GetLastError();
    }
    WinHttpCloseHandle(session);
    return out;
  }

  // The last /ice answer that came, kept beside the game (cache\ice_servers.json) with when it came.
  static std::filesystem::path IceFile() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / L"cache" / L"ice_servers.json";
  }

  json KeepIce(const json& servers) {
    std::error_code ec;
    std::filesystem::create_directories(IceFile().parent_path(), ec);
    const json kept = {{"at", int64_t(std::time(nullptr))}, {"servers", servers}};
    if (std::FILE* f = _wfopen(IceFile().c_str(), L"wb")) {
      const std::string text = kept.dump();
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    return servers;
  }

  json KeptIce() {
    std::FILE* f = _wfopen(IceFile().c_str(), L"rb");
    if (!f) return json::array();
    std::string text;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
    std::fclose(f);
    const json kept = json::parse(text, nullptr, false);
    if (!kept.is_object() || !kept.contains("at") || !kept.contains("servers")) return json::array();
    const int64_t age = int64_t(std::time(nullptr)) - kept.value("at", int64_t(0));
    if (age < 0 || age > 10 * 3600 || !kept["servers"].is_array()) return json::array();
    return kept["servers"];
  }
#else
  // One GET of the lobby's /ice (address: at that address with the name kept; "" by name): the servers, or an empty
  // array (kind: what failed, "" when the answer was not a list).
  json FetchIce(const Url& url, const std::string& address, std::string* kind) {
    kind->clear();
    net::HttpOptions options;
    options.resolve_address = address;
    options.timeout_ms = 5000;
    options.connect_timeout_ms = 5000;
    std::string body;
    const net::HttpResult r = net::HttpGetText(HttpUrl(url, "/ice"), options, &body, 1u << 20);
    if (!r.ok) {
      *kind = r.kind;
      return json::array();
    }
    json out = json::parse(body, nullptr, false);
    return out.is_array() ? out : json::array();
  }

  // The last /ice answer that came, kept beside the game (cache/ice_servers.json) with when it came.
  static std::filesystem::path IceFile() { return platform::ExeDir() / "cache" / "ice_servers.json"; }

  json KeepIce(const json& servers) {
    std::error_code ec;
    std::filesystem::create_directories(IceFile().parent_path(), ec);
    const json kept = {{"at", int64_t(std::time(nullptr))}, {"servers", servers}};
    if (std::FILE* f = platform::OpenFile(IceFile(), "wb")) {
      const std::string text = kept.dump();
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    return servers;
  }

  json KeptIce() {
    std::FILE* f = platform::OpenFile(IceFile(), "rb");
    if (!f) return json::array();
    std::string text;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
    std::fclose(f);
    const json kept = json::parse(text, nullptr, false);
    if (!kept.is_object() || !kept.contains("at") || !kept.contains("servers")) return json::array();
    const int64_t age = int64_t(std::time(nullptr)) - kept.value("at", int64_t(0));
    if (age < 0 || age > 10 * 3600 || !kept["servers"].is_array()) return json::array();
    return kept["servers"];
  }
#endif


 private:
  void Run() {
    int retry_s = 1;
    while (!Stopping()) {
      SetLink(Link::kConnecting);
      if (Connect()) {
        retry_s = 1;
        SetLink(Link::kOnline);
        std::string name, country;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          name = wanted_name_;
          country = country_;
        }
        json hello = {{"t", "hello"}, {"v", 2}, {"sub", true}, {"name", name}};
        if (!country.empty()) hello["country"] = country;
        SendNow(hello);
        // mk9-lauta (2026-11-11): a new link is nobody's guest: the room's join again (the profile room's comes with
        // the lobby's "me", once it has said the name)
        json join;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!guest_room_.empty()) join = JoinLocked();
        }
        if (!join.is_null()) SendNow(join);
        ReceiveLoop();
        CloseSocket();
      }
      if (Stopping()) break;
      SetLink(Link::kRetrying);
      for (int i = 0; i < retry_s * 10 && !Stopping(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      retry_s = std::min(retry_s * 2, 30);
    }
  }

#if defined(_WIN32)
  bool Connect() {
    const std::string text = REXCVAR_GET(mk9_lobby_url);
    const Url url = Parse(text);
    if (!url.ok) {
      static bool warned = false;
      if (!warned) REXLOG_ERROR("[lobby] mk9_lobby_url is not a ws:// or wss:// URL: {}", text);
      warned = true;
      return false;
    }
    const std::wstring address = AddressInName(url.host);
    static const bool by_address_from_start = REXCVAR_GET(mk9_lobby_by_address);
    if (by_address_from_start && !address.empty()) g_by_address = true;
    if (Attempt(text, url, address, g_by_address)) return true;
    // the name not found: its address straight away (the provider's DNS), and from now on
    if (!g_by_address && !address.empty() && LastWhy() == "dns" && Attempt(text, url, address, true)) {
      g_by_address = true;
      REXLOG_WARN("[lobby] this connection's DNS does not find {}: connected to its address, {}", text,
                  std::string(address.begin(), address.end()));
      return true;
    }
    return false;
  }

  std::string LastWhy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.link_error;
  }

  // One try at the lobby: by name, or (by_address) at the name's address with the name kept for TLS and Host.
  bool Attempt(const std::string& text, const Url& url, const std::wstring& address, bool by_address) {
    HINTERNET session = OpenSession(by_address && !address.empty());
    if (!session) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 0);  // receiving waits as long as it takes (the pings keep it alive)
    HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     url.secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    HINTERNET socket = nullptr;
    const bool address_set = by_address && !address.empty();
    // mk9-lauta (2026-11-11): the failing call's error, taken at once (closing a handle may change it), and why in
    // words for the screens (a player with no lobby sees the cause and what to try)
    DWORD error = connect ? 0 : GetLastError(), status = 0;
    if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD size = sizeof(status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status == 101) socket = WinHttpWebSocketCompleteUpgrade(request, 0);
      if (!socket && status == 101) error = GetLastError();
    } else if (!error) {
      error = GetLastError();
    }
    if (request) WinHttpCloseHandle(request);
    if (!socket) {
      const std::string why = Why(error, status);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.link_error != why || state_.link_code != error) {
          state_.link_error = why;
          state_.link_code = error;
          state_.link_status = status;
          ++state_.version;
          REXLOG_WARN("[lobby] could not reach {}{} ({}: WinHTTP error {}, HTTP status {}): trying again", text,
                      address_set ? " at its address" : "", why, error, status);
        }
      }
      if (connect) WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      session_ = session;
      connect_ = connect;
      socket_ = socket;
    }
    last_ping_ = Clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.link_error.clear();
      state_.link_code = state_.link_status = 0;
    }
    REXLOG_INFO("[lobby] connected to {}{}", text, address_set ? " (at its address: no DNS)" : "");
    send_cv_.notify_one();
    return true;
  }
#else
  bool Connect() {
    const std::string text = REXCVAR_GET(mk9_lobby_url);
    const Url url = Parse(text);
    if (!url.ok) {
      static bool warned = false;
      if (!warned) REXLOG_ERROR("[lobby] mk9_lobby_url is not a ws:// or wss:// URL: {}", text);
      warned = true;
      return false;
    }
    const std::string address = AddressInName(url.host);
    static const bool by_address_from_start = REXCVAR_GET(mk9_lobby_by_address);
    if (by_address_from_start && !address.empty()) g_by_address = true;
    if (Attempt(text, address, g_by_address)) return true;
    // the name not found: its address straight away (the provider's DNS), and from now on
    if (!g_by_address && !address.empty() && LastWhy() == "dns" && Attempt(text, address, true)) {
      g_by_address = true;
      REXLOG_WARN("[lobby] this connection's DNS does not find {}: connected to its address, {}", text, address);
      return true;
    }
    return false;
  }

  std::string LastWhy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.link_error;
  }

  // One try at the lobby: by name, or (by_address) at the name's address with the name kept for TLS and Host.
  bool Attempt(const std::string& text, const std::string& address, bool by_address) {
    const bool address_set = by_address && !address.empty();
    net::HttpOptions options;
    options.resolve_address = address_set ? address : std::string();
    options.connect_timeout_ms = 8000;
    net::HttpResult r;
    std::shared_ptr<net::WebSocket> socket = net::WebSocket::Connect(text, options, &r);
    if (!socket) {
      const std::string why = r.kind.empty() ? "other" : r.kind;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.link_error != why || state_.link_code != uint32_t(r.code)) {
          state_.link_error = why;
          state_.link_code = uint32_t(r.code);
          state_.link_status = uint32_t(r.status);
          ++state_.version;
          REXLOG_WARN("[lobby] could not reach {}{} ({}: curl error {}, HTTP status {}: {}): trying again", text,
                      address_set ? " at its address" : "", why, r.code, r.status, r.text);
        }
      }
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      socket_ = std::move(socket);
    }
    last_ping_ = Clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.link_error.clear();
      state_.link_code = state_.link_status = 0;
    }
    REXLOG_INFO("[lobby] connected to {}{}", text, address_set ? " (at its address: no DNS)" : "");
    send_cv_.notify_one();
    return true;
  }
#endif

#if defined(_WIN32)
  void CloseSocket() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_) {
      WinHttpCloseHandle(socket_);  // abortive: a receive waiting on it returns at once
      socket_ = nullptr;
    }
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
    connect_ = session_ = nullptr;
  }

  void ReceiveLoop() {
    std::string message;
    std::vector<char> buffer(64 * 1024);
    for (;;) {
      HINTERNET socket;
      {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket = socket_;
      }
      if (!socket) return;
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
      if (WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR) return;
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return;
      message.append(buffer.data(), read);
      if (message.size() > 256 * 1024) return;
      if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
        continue;
      OnMessage(message);
      message.clear();
    }
  }
#else
  void CloseSocket() {
    std::shared_ptr<net::WebSocket> socket;
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      socket = std::move(socket_);
      socket_.reset();
    }
    if (socket) socket->Abort();  // a receive waiting on it returns at once; it goes when its last user lets go
  }

  void ReceiveLoop() {
    std::string message;
    for (;;) {
      std::shared_ptr<net::WebSocket> socket;
      {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket = socket_;
      }
      if (!socket) return;
      if (!socket->Receive(&message, 256 * 1024)) return;
      OnMessage(message);
      message.clear();
    }
  }
#endif

  // Sends what is queued, and a ping every 25 s, while the link is up.
  void SendLoop() {
    for (;;) {
      std::string text;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        send_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_ || (!outbox_.empty() && Up()); });
        if (stopping_) return;
        if (Up() && Clock::now() - last_ping_ > kPingEvery) {
          last_ping_ = Clock::now();
          text = R"({"t":"ping"})";
        } else if (!outbox_.empty() && Up()) {
          text = std::move(outbox_.front());
          outbox_.pop_front();
        }
      }
      if (!text.empty()) SendText(text);
    }
  }

#if defined(_WIN32)
  bool Up() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    return socket_ != nullptr;
  }

  void SendNow(const json& message) { SendText(message.dump()); }

  void SendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(socket_mutex_);  // one send at a time; receiving goes on meanwhile
    if (!socket_) return;
    WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()),
                         DWORD(text.size()));
  }
#else
  bool Up() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    return socket_ != nullptr;
  }

  void SendNow(const json& message) { SendText(message.dump()); }

  void SendText(const std::string& text) {
    std::shared_ptr<net::WebSocket> socket;
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      socket = socket_;
    }
    if (socket) socket->SendText(text);  // one frame at a time (curl_http.h); receiving goes on meanwhile
  }
#endif

  bool Stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
  }

  void SetLink(Link link) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.link == link) return;
    state_.link = link;
    ++state_.version;
  }

  static std::string Str(const json& m, const char* key) {
    auto it = m.find(key);
    return it != m.end() && it->is_string() ? it->get<std::string>() : std::string();
  }
  static int64_t Int(const json& m, const char* key) {
    auto it = m.find(key);
    return it != m.end() && it->is_number() ? it->get<int64_t>() : 0;
  }

  static std::vector<Room> Rooms(const json& list) {
    std::vector<Room> rooms;
    if (!list.is_array()) return rooms;
    for (const json& r : list) {
      if (!r.is_object()) continue;
      Room room;
      room.code = Str(r, "room");
      room.title = Str(r, "title");
      room.host = Str(r, "host");
      room.country = Str(r, "cc");
      room.playing = Str(r, "status") == "playing";
      room.since = Int(r, "since");
      rooms.push_back(std::move(room));
    }
    return rooms;
  }

  // A line of the lobby's own, in the chat's words.
  static std::string SystemText(const json& m) {
    const std::string kind = Str(m, "kind"), by = Str(m, "by"), name = Str(m, "name");
    if (kind == "ban") return by + " baneó a " + name;
    if (kind == "unban") return by + " desbaneó a " + name;
    if (kind == "timeout") return by + " silenció a " + name + " por " + std::to_string(Int(m, "minutes")) + " min";
    if (kind == "notice") return "Aviso de " + by + ": " + Str(m, "text");
    return Str(m, "text");
  }

  void AddLine(ChatLine line) {
    state_.chat.push_back(std::move(line));
    while (state_.chat.size() > kMaxChat) state_.chat.pop_front();
  }

  ChatLine Line(const json& m) {
    ChatLine line;
    line.id = Int(m, "id");
    line.name = Str(m, "name");
    line.country = Str(m, "cc");
    line.text = Str(m, "t") == "sys" ? SystemText(m) : Str(m, "text");
    line.at = Int(m, "at");
    line.staff = m.value("staff", false);
    return line;
  }

  void OnMessage(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object()) return;
    const std::string t = Str(m, "t");
    std::lock_guard<std::mutex> lock(mutex_);
    if (t == "pong") return;
    if (t == "sig") {
      json data = m.value("data", json());
      auto& queue = data.is_object() && data.contains("ice") ? ice_
                    : data.is_object() && data.contains("pf") ? profiles_
                                                               : events_;
      queue.push_back({Event::Kind::kSig, Str(m, "room"), std::move(data)});
      if (queue.size() > 256) queue.pop_front();
      return;
    }
    if (t == "match") {
      events_.push_back({Event::Kind::kMatch, m["room"].is_string() ? m["room"].get<std::string>() : std::string(), {}});
      return;
    }
    if (t == "welcome") {
      state_.online = int(Int(m, "online"));
      state_.rooms = Rooms(m.value("rooms", json::array()));
      const json& notice = m.value("notice", json());
      state_.notice = notice.is_object() ? Str(notice, "text") : std::string();
      state_.chat.clear();
      if (m.contains("history") && m["history"].is_array()) {
        for (const json& line : m["history"]) {
          if (line.is_object()) AddLine(Line(line));
        }
      }
    } else if (t == "me") {
      state_.name = Str(m, "name");
      state_.named = m.value("ok", false);
      state_.name_error = state_.named ? std::string() : Str(m, "error");
      // mk9-lauta (2026-11-11): the name the lobby gave: its profile room (profiles.h), joined while in no room
      if (const std::string room = profiles::RoomOf(state_.name); room != pf_room_) {
        pf_room_ = room;
        REXLOG_INFO("[lobby] the profile asks for {} come to room {}", state_.name, pf_room_);
      }
      if (guest_room_.empty() && !pf_room_.empty()) {
        PushLocked(JoinLocked());
        send_cv_.notify_one();
      }
    } else if (t == "rooms") {
      state_.rooms = Rooms(m.value("rooms", json::array()));
      state_.online = int(Int(m, "online"));
    } else if (t == "online") {
      state_.online = int(Int(m, "n"));
    } else if (t == "chat" || t == "sys") {
      AddLine(Line(m));
    } else if (t == "del") {
      const json& ids = m.value("ids", json::array());
      if (ids.is_array()) {
        for (const json& id : ids) {
          if (!id.is_number()) continue;
          const int64_t gone = id.get<int64_t>();
          state_.chat.erase(std::remove_if(state_.chat.begin(), state_.chat.end(),
                                           [&](const ChatLine& l) { return l.id == gone; }),
                            state_.chat.end());
        }
      }
    } else if (t == "notice") {
      const json& notice = m.value("notice", json());
      state_.notice = notice.is_object() ? Str(notice, "text") : std::string();
    } else if (t == "error") {
      state_.error = Str(m, "text");
    } else {
      return;
    }
    ++state_.version;
  }

  std::mutex mutex_;              // the state, the queues, the flags
  std::condition_variable send_cv_;
  bool running_ = false, stopping_ = false;
  std::string wanted_name_;
  std::string country_;  // this player's, two letters (SetCountry)
  State state_;
  std::deque<std::string> outbox_;
  std::deque<Event> events_, ice_, profiles_;
  std::string guest_room_, guest_id_;  // the room this game joined as its guest ("" none: Join)
  std::string pf_room_;                // this game's profile room (profiles.h; "" until the lobby says the name)
  Clock::time_point last_ping_{};
  std::thread worker_, sender_;

#if defined(_WIN32)
  std::mutex socket_mutex_;       // the WinHTTP handles
  HINTERNET session_ = nullptr, connect_ = nullptr, socket_ = nullptr;
#else
  std::mutex socket_mutex_;       // the WebSocket (curl_http.h)
  std::shared_ptr<net::WebSocket> socket_;
#endif
};

Client& TheClient() {
  static Client client;
  return client;
}

}  // namespace

// "off" (or "none", or empty in a config file): no lobby at all. An empty value on the command line leaves the default
// in place, so tests that must not reach the real lobby say "off".
bool Configured() {
  const std::string& url = REXCVAR_GET(mk9_lobby_url);
  return !url.empty() && url != "off" && url != "none";
}

void Start(const std::string& name) {
  if (Configured()) TheClient().Start(name);
}
void Stop() { TheClient().Stop(); }
State Snapshot() { return TheClient().Snapshot(); }

void SetName(const std::string& name) { TheClient().Send({{"t", "name"}, {"name", name}}); }
void SetCountry(const std::string& country) { TheClient().SetCountry(country); }
void Chat(const std::string& text) { TheClient().Send({{"t", "chat"}, {"text", text}}); }
void Host(const std::string& room, const std::string& title, bool open, bool playing) {
  TheClient().Send({{"t", "host"}, {"room", room}, {"title", title}, {"open", open}, {"mode", "fight"},
                    {"status", playing ? "playing" : "waiting"}});
}
void Unhost() { TheClient().Send({{"t", "unhost"}}); }
void Join(const std::string& room, const std::string& id) { TheClient().Join(room, id); }
void Quick() { TheClient().Send({{"t", "quick"}, {"mode", "fight"}}); }
void SigToHost(const std::string& room, const nlohmann::json& data) {
  TheClient().Send({{"t", "sig"}, {"room", room}, {"to", "host"}, {"data", data}});
}
void SigToGuest(const std::string& room, const std::string& id, const nlohmann::json& data) {
  TheClient().Send({{"t", "sig"}, {"room", room}, {"to", "guest"}, {"id", id}, {"data", data}});
}
bool PollEvent(Event* event) { return TheClient().PollEvent(event); }
bool PollIce(Event* event) { return TheClient().PollIce(event); }
bool PollProfile(Event* event) { return TheClient().PollProfile(event); }
nlohmann::json IceServers() { return TheClient().IceServers(); }
bool ByAddress() { return g_by_address.load(); }

}  // namespace mk9::lobby
