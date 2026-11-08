// MK9 online: the lobby (lobby.h).

#include "online/lobby.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mk9_lobby_url, "wss://107-172-143-35.sslip.io/lobby?game=mk9pc", "MK9 Online",
                      "The online lobby: rooms, the global chat, quick match (ws:// or wss://; \"off\" = none: tests)");

namespace mk9::lobby {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxChat = 200;
constexpr auto kPingEvery = std::chrono::seconds(25);

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

  json IceServers() {
    const Url url = Parse(REXCVAR_GET(mk9_lobby_url));
    if (!url.ok) return json::array();
    HINTERNET session = WinHttpOpen(L"mk9-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return json::array();
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
        }
        WinHttpCloseHandle(request);
      }
      WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return out;
  }

 private:
  void Run() {
    int retry_s = 1;
    while (!Stopping()) {
      SetLink(Link::kConnecting);
      if (Connect()) {
        retry_s = 1;
        SetLink(Link::kOnline);
        std::string name;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          name = wanted_name_;
        }
        SendNow({{"t", "hello"}, {"v", 2}, {"sub", true}, {"name", name}});
        ReceiveLoop();
        CloseSocket();
      }
      if (Stopping()) break;
      SetLink(Link::kRetrying);
      for (int i = 0; i < retry_s * 10 && !Stopping(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      retry_s = std::min(retry_s * 2, 30);
    }
  }

  bool Connect() {
    const std::string text = REXCVAR_GET(mk9_lobby_url);
    const Url url = Parse(text);
    if (!url.ok) {
      static bool warned = false;
      if (!warned) REXLOG_ERROR("[lobby] mk9_lobby_url is not a ws:// or wss:// URL: {}", text);
      warned = true;
      return false;
    }
    HINTERNET session = WinHttpOpen(L"mk9-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 0);  // receiving waits as long as it takes (the pings keep it alive)
    HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     url.secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    HINTERNET socket = nullptr;
    if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0, size = sizeof(status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status == 101) socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    }
    if (request) WinHttpCloseHandle(request);
    if (!socket) {
      const DWORD error = GetLastError();
      static int warned = 0;
      if (warned++ < 3) REXLOG_WARN("[lobby] could not reach {} (WinHTTP error {}): trying again", text, error);
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
    REXLOG_INFO("[lobby] connected to {}", text);
    send_cv_.notify_one();
    return true;
  }

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
      auto& queue = data.is_object() && data.contains("ice") ? ice_ : events_;
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
  State state_;
  std::deque<std::string> outbox_;
  std::deque<Event> events_, ice_;
  Clock::time_point last_ping_{};
  std::thread worker_, sender_;

  std::mutex socket_mutex_;       // the WinHTTP handles
  HINTERNET session_ = nullptr, connect_ = nullptr, socket_ = nullptr;
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
void Chat(const std::string& text) { TheClient().Send({{"t", "chat"}, {"text", text}}); }
void Host(const std::string& room, const std::string& title, bool open, bool playing) {
  TheClient().Send({{"t", "host"}, {"room", room}, {"title", title}, {"open", open}, {"mode", "fight"},
                    {"status", playing ? "playing" : "waiting"}});
}
void Unhost() { TheClient().Send({{"t", "unhost"}}); }
void Join(const std::string& room, const std::string& id) {
  TheClient().Send({{"t", "join"}, {"room", room}, {"id", id}});
}
void Quick() { TheClient().Send({{"t", "quick"}, {"mode", "fight"}}); }
void SigToHost(const std::string& room, const nlohmann::json& data) {
  TheClient().Send({{"t", "sig"}, {"room", room}, {"to", "host"}, {"data", data}});
}
void SigToGuest(const std::string& room, const std::string& id, const nlohmann::json& data) {
  TheClient().Send({{"t", "sig"}, {"room", room}, {"to", "guest"}, {"id", id}, {"data", data}});
}
bool PollEvent(Event* event) { return TheClient().PollEvent(event); }
bool PollIce(Event* event) { return TheClient().PollIce(event); }
nlohmann::json IceServers() { return TheClient().IceServers(); }

}  // namespace mk9::lobby
