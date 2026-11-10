// MK9 online: Discord's Rich Presence over the Discord app's local pipe (discord.h).
//
// The pipe protocol: frames of a little-endian opcode and length, then JSON. A handshake (opcode 0: {v: 1, client_id})
// answered with a READY frame, then commands (opcode 1): SET_ACTIVITY {pid, activity}. A thread of its own connects,
// sends what changed (Discord takes about five updates every 20 s) and reads what comes back.

#include "online/discord.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mk9_discord_app_id, "", "MK9 Online",
                      "Discord's Rich Presence: the Discord application's id (developer portal, Application ID) whose "
                      "name shows as what you play; empty = no Rich Presence");
REXCVAR_DEFINE_BOOL(mk9_discord_presence, true, "MK9 Online",
                    "Show in Discord what you are doing (online, in a room, fighting whom), with the Discord app open");

namespace mk9::discord {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

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
  // What came back, read and dropped (false: the pipe is gone).
  bool Drain() {
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
      available -= std::min(available, read);
    }
    return true;
  }
  bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

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
    if (!pipe.Drain()) continue;
    Presence wanted;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      wanted = g_wanted;
    }
    if ((have_sent && wanted == sent) || Clock::now() - sent_at < std::chrono::seconds(4)) continue;
    json activity = nullptr;
    if (!wanted.details.empty()) {
      activity = {{"details", wanted.details}, {"assets", {{"large_image", "logo"}, {"large_text", "mk9-lauta"}}}};
      if (!wanted.state.empty()) activity["state"] = wanted.state;
      if (wanted.since > 0) activity["timestamps"] = {{"start", wanted.since}};
    }
    if (pipe.Write(1, {{"cmd", "SET_ACTIVITY"},
                       {"args", {{"pid", int(GetCurrentProcessId())}, {"activity", activity}}},
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

}  // namespace mk9::discord
