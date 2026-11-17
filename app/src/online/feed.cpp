// MK9 online: a session's match for the people watching it (feed.h).

#include "online/feed.h"

#include "mk9_platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>

#include "miniz.h"

#include "online/lobby.h"

namespace mk9::feed {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kEvery = std::chrono::milliseconds(60);  // the messages go out at most this often
constexpr size_t kLeast = 256;     // a spectator's share at least
constexpr size_t kReserve = 16u << 20;  // the host's recording
constexpr auto kSilent = std::chrono::seconds(20);  // a spectator not heard from this long is gone
// mk9-lauta (2026-11-17): a deflated piece's raw bytes at most, and the credit a spectator may save up (its shares)
constexpr size_t kZipPiece = 32u << 10;
constexpr size_t kZipCreditTurns = 8;

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
  bool zip = false;          // mk9-lauta (2026-11-17): it takes the recording deflated
  int64_t credit = 0;        // ... the deflated bytes it may get now (its share each turn, spent by each piece)
};

// The recording's bytes deflated (empty: they would not deflate).
std::string Deflate(const char* data, size_t size) {
  mz_ulong out_size = mz_compressBound(mz_ulong(size));
  std::string out(size_t(out_size), '\0');
  if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &out_size,
                   reinterpret_cast<const unsigned char*>(data), mz_ulong(size), MZ_DEFAULT_LEVEL) != MZ_OK) {
    return {};
  }
  out.resize(size_t(out_size));
  return out;
}

std::mutex g_mutex;
std::string g_recording;          // the host's
std::vector<Watcher> g_watchers;  // the host's
Clock::time_point g_turn{};       // the host's last turn
std::string g_received;           // the spectator's, from offset 0
bool g_ended = false;

}  // namespace

void Append(const void* data, size_t size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // mk9-lauta (2026-11-11): room for about 90 minutes of session at once (the game thread appends: no copy of a
  // growing recording in the middle of a fight)
  if (g_recording.capacity() < kReserve) g_recording.reserve(kReserve);
  g_recording.append(static_cast<const char*>(data), size);
}

bool Heard(const std::string& id, uint64_t have, bool zip) {
  if (id.empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Watcher& w : g_watchers) {
    if (w.id != id) continue;
    w.heard = Clock::now();
    w.zip = zip;
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
  Watcher w;
  w.id = id;
  w.sent = w.sent_before = from;
  w.heard = Clock::now();
  w.zip = zip;
  g_watchers.push_back(w);
  REXLOG_INFO("[feed] a spectator ({}) watches{}: {} spectator(s)", id, zip ? " (deflated)" : "", g_watchers.size());
  return true;
}

int Watchers() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return int(g_watchers.size());
}

void Pump(const std::string& room, bool ended, size_t budget) {
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
  const size_t share = behind ? std::max(kLeast, budget / behind) : 0;
  for (Watcher& w : g_watchers) {
    if (w.sent < g_recording.size() && w.zip) {
      // mk9-lauta (2026-11-17): deflated: a bigger piece of the recording for the same share of the upload (the share
      // each turn adds to its credit; a piece goes once there is credit, and spends what it took deflated)
      w.credit = std::min<int64_t>(w.credit + int64_t(share), int64_t(share * kZipCreditTurns));
      if (w.credit <= 0) continue;
      const size_t n = std::min({g_recording.size() - size_t(w.sent), size_t(w.credit) * 4, kZipPiece});
      const std::string packed = Deflate(g_recording.data() + w.sent, n);
      if (!packed.empty() && packed.size() < n) {
        lobby::SigToGuest(room, w.id,
                          {{"room", "feed"},
                           {"at", w.sent},
                           {"n", n},
                           {"z", Encode(reinterpret_cast<const uint8_t*>(packed.data()), packed.size())}});
        w.credit -= int64_t(packed.size());
        w.sent += n;
        continue;
      }
      // (not smaller deflated: as it is, below)
    }
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

void ReceivedZip(uint64_t at, const std::string& base64, uint64_t n) {
  if (n == 0 || n > kZipPiece) return;
  const std::string packed = Decode(base64);
  std::string bytes(size_t(n), '\0');
  mz_ulong size = mz_ulong(n);
  if (mz_uncompress(reinterpret_cast<unsigned char*>(bytes.data()), &size,
                    reinterpret_cast<const unsigned char*>(packed.data()), mz_ulong(packed.size())) != MZ_OK ||
      size != n) {
    REXLOG_WARN("[feed] a deflated piece at {} did not inflate: the next \"watch\" asks for it again", at);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (at == g_received.size()) {
    g_received += bytes;
  } else if (at < g_received.size() && at + bytes.size() > g_received.size()) {
    g_received += bytes.substr(size_t(g_received.size() - at));
  }
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

bool LoadFile(const std::filesystem::path& path) {
  FILE* f = mk9::platform::OpenFile(path, "rb");
  if (!f) return false;
  std::string bytes;
  char buffer[65536];
  for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) bytes.append(buffer, n);
  std::fclose(f);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_received = std::move(bytes);
  g_ended = true;
  return true;
}

}  // namespace mk9::feed
