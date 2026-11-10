// MK9 online: a session's match for the people watching it (feed.h).

#include "online/feed.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>

#include "online/lobby.h"

namespace mk9::feed {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kEvery = std::chrono::milliseconds(60);  // the messages go out at most this often
constexpr size_t kBudget = 2048;   // recording bytes a turn, the spectators' together (~34 KB/s)
constexpr size_t kLeast = 256;     // a spectator's share at least
constexpr auto kSilent = std::chrono::seconds(20);  // a spectator not heard from this long is gone

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Encode(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t a = data[i], b = i + 1 < size ? data[i + 1] : 0, c = i + 2 < size ? data[i + 2] : 0;
    const uint32_t v = a << 16 | b << 8 | c;
    out += kAlphabet[v >> 18 & 63];
    out += kAlphabet[v >> 12 & 63];
    out += i + 1 < size ? kAlphabet[v >> 6 & 63] : '=';
    out += i + 2 < size ? kAlphabet[v & 63] : '=';
  }
  return out;
}

std::string Decode(const std::string& text) {
  int table[256];
  std::fill(std::begin(table), std::end(table), -1);
  for (int i = 0; i < 64; ++i) table[uint8_t(kAlphabet[i])] = i;
  std::string out;
  uint32_t v = 0;
  int bits = 0;
  for (char ch : text) {
    const int d = table[uint8_t(ch)];
    if (d < 0) continue;  // '=' and anything else
    v = v << 6 | uint32_t(d);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += char(v >> bits & 0xFF);
    }
  }
  return out;
}

struct Watcher {
  std::string id;
  uint64_t sent = 0;
  uint64_t sent_before = 0;  // sent when its last word came: had it all come, what it has now is past this
  Clock::time_point heard{};
  bool told_end = false;
};

std::mutex g_mutex;
std::string g_recording;          // the host's
std::vector<Watcher> g_watchers;  // the host's
Clock::time_point g_turn{};       // the host's last turn
std::string g_received;           // the spectator's, from offset 0
bool g_ended = false;

}  // namespace

void Append(const void* data, size_t size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_recording.append(static_cast<const char*>(data), size);
}

bool Heard(const std::string& id, uint64_t have) {
  if (id.empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Watcher& w : g_watchers) {
    if (w.id != id) continue;
    w.heard = Clock::now();
    // what was sent since its last word may be on the way still; what was sent before it should be there
    if (have < w.sent_before) {
      REXLOG_INFO("[feed] spectator {} has {} of the {} bytes sent before its last word: from there again", id, have,
                  w.sent_before);
      w.sent = have;
      w.told_end = false;
    }
    w.sent_before = w.sent;
    return true;
  }
  if (int(g_watchers.size()) >= kMaxWatchers) return false;
  const uint64_t from = std::min<uint64_t>(have, g_recording.size());
  g_watchers.push_back({id, from, from, Clock::now()});
  REXLOG_INFO("[feed] a spectator ({}) watches: {} spectator(s)", id, g_watchers.size());
  return true;
}

int Watchers() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return int(g_watchers.size());
}

void Pump(const std::string& room, bool ended) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  if (now - g_turn < kEvery) return;
  g_turn = now;
  const size_t before = g_watchers.size();
  g_watchers.erase(std::remove_if(g_watchers.begin(), g_watchers.end(),
                                  [&](const Watcher& w) { return now - w.heard > kSilent; }),
                   g_watchers.end());
  if (g_watchers.size() != before) {
    REXLOG_INFO("[feed] {} spectator(s) stopped asking: {} left", before - g_watchers.size(), g_watchers.size());
  }
  size_t behind = 0;
  for (const Watcher& w : g_watchers) behind += w.sent < g_recording.size() ? 1 : 0;
  const size_t share = behind ? std::max(kLeast, kBudget / behind) : 0;
  for (Watcher& w : g_watchers) {
    if (w.sent < g_recording.size()) {
      const size_t n = std::min(share, g_recording.size() - size_t(w.sent));
      lobby::SigToGuest(room, w.id,
                        {{"room", "feed"},
                         {"at", w.sent},
                         {"data", Encode(reinterpret_cast<const uint8_t*>(g_recording.data()) + w.sent, n)}});
      w.sent += n;
    } else if (ended && !w.told_end) {
      lobby::SigToGuest(room, w.id, {{"room", "feed_end"}});
      w.told_end = true;
    }
  }
}

void Received(uint64_t at, const std::string& base64) {
  const std::string bytes = Decode(base64);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (at == g_received.size()) {
    g_received += bytes;
  } else if (at < g_received.size() && at + bytes.size() > g_received.size()) {
    g_received += bytes.substr(size_t(g_received.size() - at));  // a piece already had in part
  }
  // past what we have: a piece lost on the way (the relay keeps the order): the next "watch" says how much there is,
  // and the host sends it again from there
}

void ReceivedEnd() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ended = true;
}

bool Read(uint64_t at, void* out, size_t size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (at + size > g_received.size()) return false;
  std::memcpy(out, g_received.data() + at, size);
  return true;
}

uint64_t Size() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_received.size();
}

bool Ended() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_ended;
}

}  // namespace mk9::feed
