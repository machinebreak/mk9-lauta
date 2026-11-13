// MK9 online: Discord's Rich Presence over the Discord app's local pipe (discord.h).
//
// The pipe protocol: frames of a little-endian opcode and length, then JSON. A handshake (opcode 0: {v: 1, client_id})
// answered with a READY frame (the account: data.user), then commands (opcode 1): SET_ACTIVITY {pid, activity}. A
// thread of its own connects, sends what changed (Discord takes about five updates every 20 s) and reads what comes
// back.

#include "online/discord.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
// mk9-lauta (2026-11-13): Linux: the Discord app's socket (discord-ipc-N in the runtime folder; Flatpak's and Snap's
// too), the same frames as Windows' pipe
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/i18n.h"

REXCVAR_DEFINE_STRING(mk9_discord_app_id, "1569708793200250910", "MK9 Online",
                      "Discord's Rich Presence: the Discord application's id (developer portal, Application ID) whose "
                      "name shows as what you play (mk9-lauta's own by default); empty = no Rich Presence");
REXCVAR_DEFINE_BOOL(mk9_discord_presence, true, "MK9 Online",
                    "Show in Discord what you are doing (online, in a room, fighting whom), with the Discord app open");
REXCVAR_DEFINE_STRING(mk9_discord_image, "https://raw.githubusercontent.com/machinebreak/mk9-lauta/main/docs/assets/discord_logo.png",
                      "MK9 Online",
                      "Discord's Rich Presence: the picture's address (a PNG Discord fetches; empty: the application's "
                      "art asset named logo)");

namespace mk9::discord {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* kDownload = "https://github.com/machinebreak/mk9-lauta/releases/latest";
constexpr const char* kCommunity = "https://discord.gg/7hbqkDmKHA";

struct Presence {
  std::string details, state;
  int64_t since = 0;
  bool operator==(const Presence& o) const {
    return details == o.details && state == o.state && since == o.since;
  }
};

std::mutex g_mutex;
Presence g_wanted;
std::atomic<bool> g_started{false};
// the account the presence's link heard (its READY)
bool g_have_user = false;
std::string g_user_id, g_user_avatar;

// This game's process id (Discord shows the activity while that process lives).
uint32_t ProcessId() {
#if defined(_WIN32)
  return GetCurrentProcessId();
#else
  return uint32_t(getpid());
#endif
}

#if defined(_WIN32)
class Pipe {
 public:
  ~Pipe() { Close(); }
  bool Open() {
    for (int i = 0; i < 10 && handle_ == INVALID_HANDLE_VALUE; ++i) {
      const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
      handle_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return handle_ != INVALID_HANDLE_VALUE;
  }
  void Close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    in_.clear();
  }
  bool Write(uint32_t op, const json& body) {
    const std::string text = body.dump();
    std::string frame(8 + text.size(), '\0');
    const uint32_t length = uint32_t(text.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, text.data(), text.size());
    DWORD written = 0;
    if (!WriteFile(handle_, frame.data(), DWORD(frame.size()), &written, nullptr) || written != frame.size()) {
      Close();
      return false;
    }
    return true;
  }
  // What came back, read without waiting; each whole frame's JSON to `on` (false: the pipe is gone).
  template <typename OnFrame>
  bool Read(OnFrame&& on) {
    DWORD available = 0;
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
      Close();
      return false;
    }
    while (available) {
      char buffer[4096];
      DWORD read = 0;
      if (!ReadFile(handle_, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read) {
        Close();
        return false;
      }
      in_.append(buffer, read);
      available -= std::min(available, read);
    }
    while (in_.size() >= 8) {
      uint32_t op = 0, length = 0;
      std::memcpy(&op, in_.data(), 4);
      std::memcpy(&length, in_.data() + 4, 4);
      if (length > (1u << 20)) {  // not Discord's
        Close();
        return false;
      }
      if (in_.size() < 8 + size_t(length)) break;
      const json body = json::parse(in_.substr(8, length), nullptr, false);
      in_.erase(0, 8 + size_t(length));
      if (body.is_object()) on(op, body);
    }
    return true;
  }
  bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::string in_;
};
#else
class Pipe {
 public:
  ~Pipe() { Close(); }
  bool Open() {
    if (fd_ >= 0) return true;
    std::vector<std::string> folders;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
      if (const char* v = std::getenv(var); v && *v) folders.push_back(v);
    }
    folders.push_back("/tmp");
    for (const std::string& folder : folders) {
      for (const char* sub : {"", "/app/com.discordapp.Discord", "/snap.discord"}) {
        for (int i = 0; i < 10; ++i) {
          const std::string path = folder + sub + "/discord-ipc-" + std::to_string(i);
          sockaddr_un addr{};
          if (path.size() >= sizeof(addr.sun_path)) continue;
          addr.sun_family = AF_UNIX;
          std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
          const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
          if (fd < 0) return false;
          if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fd_ = fd;
            return true;
          }
          ::close(fd);
        }
      }
    }
    return false;
  }
  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
  }
  bool Write(uint32_t op, const json& body) {
    const std::string text = body.dump();
    std::string frame(8 + text.size(), '\0');
    const uint32_t length = uint32_t(text.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, text.data(), text.size());
    size_t done = 0;
    while (done < frame.size()) {
      const ssize_t n = ::send(fd_, frame.data() + done, frame.size() - done, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        Close();
        return false;
      }
      done += size_t(n);
    }
    return true;
  }
  // What came back, read without waiting; each whole frame's JSON to `on` (false: the socket is gone).
  template <typename OnFrame>
  bool Read(OnFrame&& on) {
    for (;;) {
      char buffer[4096];
      const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        in_.append(buffer, size_t(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
      Close();  // 0: Discord closed it; another error: gone
      return false;
    }
    while (in_.size() >= 8) {
      uint32_t op = 0, length = 0;
      std::memcpy(&op, in_.data(), 4);
      std::memcpy(&length, in_.data() + 4, 4);
      if (length > (1u << 20)) {  // not Discord's
        Close();
        return false;
      }
      if (in_.size() < 8 + size_t(length)) break;
      const json body = json::parse(in_.substr(8, length), nullptr, false);
      in_.erase(0, 8 + size_t(length));
      if (body.is_object()) on(op, body);
    }
    return true;
  }
  bool IsOpen() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
  std::string in_;
};
#endif

// A text field of a frame ("" when missing, null or not a text: the replies' "evt" is null).
std::string Field(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A READY frame's account: data.user's id and avatar.
bool TakeUser(const json& body, std::string* id, std::string* avatar) {
  if (Field(body, "evt") != "READY") return false;
  const auto data = body.find("data");
  if (data == body.end() || !data->is_object()) return false;
  const auto user = data->find("user");
  if (user == data->end() || !user->is_object()) return false;
  *id = Field(*user, "id");
  *avatar = Field(*user, "avatar");
  return !id->empty();
}

void Run(std::string app_id) {
  Pipe pipe;
  Presence sent;
  bool have_sent = false;
  Clock::time_point tried{}, sent_at{};
  int nonce = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!pipe.IsOpen()) {
      if (Clock::now() - tried < std::chrono::seconds(15)) continue;  // the Discord app may start later
      tried = Clock::now();
      if (!pipe.Open()) continue;
      if (!pipe.Write(0, {{"v", 1}, {"client_id", app_id}})) continue;
      have_sent = false;
      REXLOG_INFO("[discord] connected to the Discord app");
    }
    const bool alive = pipe.Read([](uint32_t, const json& body) {
      std::string id, avatar;
      if (TakeUser(body, &id, &avatar)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_have_user = true;
        g_user_id = id;
        g_user_avatar = avatar;
      } else if (Field(body, "evt") == "ERROR") {
        REXLOG_WARN("[discord] the Discord app said: {}", body.dump().substr(0, 300));
      }
    });
    if (!alive) continue;
    Presence wanted;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      wanted = g_wanted;
    }
    if ((have_sent && wanted == sent) || Clock::now() - sent_at < std::chrono::seconds(4)) continue;
    json activity = nullptr;
    if (!wanted.details.empty()) {
      const std::string image = REXCVAR_GET(mk9_discord_image);
      activity = {{"details", wanted.details},
                  {"assets", {{"large_image", image.empty() ? std::string("logo") : image},
                              {"large_text", "MK9 Online by Lauta"}}},
                  {"buttons", json::array({{{"label", i18n::T("Descargar MK9 Online")}, {"url", kDownload}},
                                           {{"label", i18n::T("Comunidad de Discord")}, {"url", kCommunity}}})}};
      if (!wanted.state.empty()) activity["state"] = wanted.state;
      if (wanted.since > 0) activity["timestamps"] = {{"start", wanted.since}};
    }
    if (pipe.Write(1, {{"cmd", "SET_ACTIVITY"},
                       {"args", {{"pid", int(ProcessId())}, {"activity", activity}}},
                       {"nonce", std::to_string(++nonce)}})) {
      sent = wanted;
      have_sent = true;
      sent_at = Clock::now();
    }
  }
}

}  // namespace

void SetPresence(const std::string& details, const std::string& state, int64_t since) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_wanted = {details, state, since};
  }
  if (g_started.load(std::memory_order_relaxed)) return;
  const std::string app_id = REXCVAR_GET(mk9_discord_app_id);
  if (app_id.empty() || !REXCVAR_GET(mk9_discord_presence) || g_started.exchange(true)) return;
  std::thread(Run, app_id).detach();
}

bool User(std::string* id, std::string* avatar, int timeout_ms) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_have_user) {
      *id = g_user_id;
      *avatar = g_user_avatar;
      return true;
    }
  }
  // the presence is off or has not reached the app: a link of its own for the READY, then closed
  const std::string app_id = REXCVAR_GET(mk9_discord_app_id);
  if (app_id.empty()) return false;
  Pipe pipe;
  if (!pipe.Open() || !pipe.Write(0, {{"v", 1}, {"client_id", app_id}})) return false;
  bool found = false;
  const auto until = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 200));
  while (!found && pipe.IsOpen() && Clock::now() < until) {
    pipe.Read([&](uint32_t, const json& body) { found = found || TakeUser(body, id, avatar); });
    if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (found) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_have_user = true;
    g_user_id = *id;
    g_user_avatar = *avatar;
  }
  return found;
}

std::string FlagEmoji(const std::string& country) {
  if (country.size() != 2) return {};
  std::string out;
  for (char c : country) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return {};
    const uint32_t cp = 0x1F1E6u + uint32_t(c - 'A');  // REGIONAL INDICATOR SYMBOL LETTER A + n
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

}  // namespace mk9::discord
