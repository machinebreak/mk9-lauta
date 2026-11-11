// MK9 online: another player's profile, asked for from the global chat (profiles.h).

#include "online/profiles.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <random>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/logging.h>

#include "online/avatars.h"
#include "online/flags.h"
#include "online/lobby.h"
#include "online/session.h"
#include "online/stats.h"

#if defined(MK9_LAUTA_VERSION)
#define MK9_PROFILES_VERSION MK9_LAUTA_VERSION
#else
#define MK9_PROFILES_VERSION "0.0.0"
#endif

namespace mk9::profiles {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr auto kAskFor = std::chrono::seconds(6);       // no answer after this: the window says so
constexpr auto kFresh = std::chrono::seconds(20);       // a card this new is not asked for again
constexpr auto kAnswerGap = std::chrono::seconds(10);   // each asker's answers at most this often
constexpr auto kPictureGap = std::chrono::seconds(60);  // and the picture
constexpr int kAnswersAMinute = 20;
constexpr int kPicturesAMinute = 6;

// What another game sent, read without trusting it: the wrong type reads as none.
std::string Str(const json& d, const char* key, size_t limit = 64) {
  const auto it = d.find(key);
  if (it == d.end() || !it->is_string()) return {};
  std::string s = it->get<std::string>();
  if (s.size() > limit) s.resize(limit);
  return s;
}
int64_t Int(const json& d, const char* key) {
  const auto it = d.find(key);
  return it != d.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}
std::string StrAt(const json& a, size_t i, size_t limit = 40) {
  if (i >= a.size() || !a[i].is_string()) return {};
  std::string s = a[i].get<std::string>();
  if (s.size() > limit) s.resize(limit);
  return s;
}
int64_t IntAt(const json& a, size_t i) { return i < a.size() && a[i].is_number_integer() ? a[i].get<int64_t>() : 0; }

// The lobby's own key of a name: its letters and digits, lower case.
std::string Key(const std::string& name) {
  std::string key;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) key += c;
  }
  return key;
}

bool RoomCode(const std::string& s) {
  if (s.size() < 4 || s.size() > 8) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

std::string MyName() {
  const lobby::State s = lobby::Snapshot();
  return s.name.empty() ? session::LoadName() : s.name;
}

struct Known {
  Status status = Status::kNone;  // the last ask's
  bool have = false;              // a card came (it stays shown while another is asked for)
  Card card;
  Clock::time_point asked{}, got{};
  std::string nonce;
};
std::unordered_map<std::string, Known> g_known;                          // by Key(name)
std::unordered_map<std::string, Clock::time_point> g_answered, g_pictured;  // by the asker's room
std::deque<Clock::time_point> g_answers, g_pictures;                     // the last minute's

bool Allow(std::deque<Clock::time_point>& last, int per_minute) {
  const auto now = Clock::now();
  while (!last.empty() && now - last.front() > std::chrono::minutes(1)) last.pop_front();
  if (int(last.size()) >= per_minute) return false;
  last.push_back(now);
  return true;
}

bool Due(std::unordered_map<std::string, Clock::time_point>& times, const std::string& who,
         std::chrono::seconds gap) {
  const auto now = Clock::now();
  auto& at = times[who];
  if (at != Clock::time_point{} && now - at < gap) return false;
  at = now;
  return true;
}

json MyCard() {
  const stats::Summary s = stats::Totals();
  json characters = json::array();
  for (size_t i = 0; i < s.characters.size() && i < 3; ++i) {
    characters.push_back(json::array({s.characters[i].first, s.characters[i].second}));
  }
  json recent = json::array();
  for (const stats::Match& m : stats::Recent(8)) {
    recent.push_back(json::array({m.at, m.rival, m.rival_country, m.mine, m.theirs, m.result}));
  }
  return {{"pf", "card"}, {"name", MyName()},  {"cc", flags::Mine()}, {"v", MK9_PROFILES_VERSION},
          {"p", s.played}, {"w", s.won},        {"l", s.lost},         {"av", avatars::MineHash()},
          {"ch", characters}, {"r", recent}};
}

Card ReadCard(const json& d) {
  Card c;
  c.name = Str(d, "name", 24);
  c.country = Str(d, "cc", 2);
  c.version = Str(d, "v", 16);
  c.avatar = avatars::CleanHash(Str(d, "av", 16));
  c.played = int(std::clamp<int64_t>(Int(d, "p"), 0, 1000000));
  c.won = int(std::clamp<int64_t>(Int(d, "w"), 0, 1000000));
  c.lost = int(std::clamp<int64_t>(Int(d, "l"), 0, 1000000));
  if (const auto it = d.find("ch"); it != d.end() && it->is_array()) {
    for (const json& e : *it) {
      if (!e.is_array() || c.characters.size() >= 3) continue;
      const std::string name = StrAt(e, 0, 32);
      if (!name.empty()) c.characters.emplace_back(name, int(std::clamp<int64_t>(IntAt(e, 1), 0, 1000000)));
    }
  }
  if (const auto it = d.find("r"); it != d.end() && it->is_array()) {
    for (const json& e : *it) {
      if (!e.is_array() || c.recent.size() >= 8) continue;
      Match m;
      m.at = IntAt(e, 0);
      m.rival = StrAt(e, 1, 24);
      m.rival_country = StrAt(e, 2, 2);
      m.mine = StrAt(e, 3, 32);
      m.theirs = StrAt(e, 4, 32);
      m.result = int(std::clamp<int64_t>(IntAt(e, 5), -1, 1));
      c.recent.push_back(std::move(m));
    }
  }
  return c;
}

// --- this game, asked ---

void Answer(const json& d) {
  const std::string back = Str(d, "back", 8), from = Str(d, "from", 24);
  REXLOG_INFO("[profiles] {} asks for this player's profile (answer to {})", from.empty() ? "someone" : from, back);
  if (!RoomCode(back) || back == RoomOf(MyName())) return;
  if (!from.empty() && session::IsBlocked(from)) return;
  if (!Due(g_answered, back, kAnswerGap) || !Allow(g_answers, kAnswersAMinute)) return;
  json card = MyCard();
  card["n"] = Str(d, "n", 16);
  lobby::SigToGuest(back, "pf", card);
  REXLOG_INFO("[profiles] the profile went to {} (who asked)", from.empty() ? std::string("someone") : from);
}

void SendPicture(const json& d) {
  const std::string back = Str(d, "back", 8), hash = Str(d, "h", 16), from = Str(d, "from", 24);
  if (!RoomCode(back) || hash.empty() || hash != avatars::MineHash()) return;
  if (!from.empty() && session::IsBlocked(from)) return;
  if (!Due(g_pictured, back, kPictureGap) || !Allow(g_pictures, kPicturesAMinute)) return;
  const std::string data = avatars::MineData();
  if (data.empty()) return;
  lobby::SigToGuest(back, "pf", {{"pf", "av"}, {"h", hash}, {"d", avatars::Base64(data)}});
}

// --- this game's asks, answered ---

void TakeCard(const json& d) {
  Card card = ReadCard(d);
  auto it = g_known.find(Key(card.name));
  if (it == g_known.end() || it->second.status != Status::kAsking || Str(d, "n", 16) != it->second.nonce) return;
  Known& k = it->second;
  k.card = std::move(card);
  k.have = true;
  k.status = Status::kHave;
  k.got = Clock::now();
  if (!k.card.avatar.empty() && avatars::ShowOthers() && !session::IsBlocked(k.card.name) &&
      avatars::Wanted(k.card.avatar)) {
    lobby::SigToGuest(RoomOf(k.card.name), "pf",
                      {{"pf", "av?"}, {"h", k.card.avatar}, {"back", RoomOf(MyName())}, {"from", MyName()}});
  }
}

void TakePicture(const json& d) {
  const std::string hash = avatars::CleanHash(Str(d, "h", 16));
  if (hash.empty()) return;
  // only a picture asked for: one of the cards'
  const bool asked = std::any_of(g_known.begin(), g_known.end(),
                                 [&](const auto& k) { return k.second.have && k.second.card.avatar == hash; });
  if (!asked) return;
  const auto it = d.find("d");
  if (it == d.end() || !it->is_string() || it->get_ref<const std::string&>().size() > (avatars::kMaxBytes + 2) / 3 * 4) {
    return;
  }
  avatars::Store(hash, avatars::Unbase64(it->get<std::string>()));
}

}  // namespace

std::string RoomOf(const std::string& name) {
  const std::string key = Key(name);
  if (key.empty()) return {};
  uint64_t h = 1469598103934665603ull;  // FNV-1a 64, then MurmurHash3's finalizer: every bit of the name everywhere
  for (unsigned char c : "mk9pf:" + key) {
    h ^= c;
    h *= 1099511628211ull;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::string code;
  for (int i = 0; i < 8; ++i) {
    code += kAlphabet[h & 31];
    h >>= 5;
  }
  return code;
}

void Ask(const std::string& name) {
  const std::string key = Key(name), room = RoomOf(name), back = RoomOf(MyName());
  if (key.empty() || room.empty()) return;
  Known& k = g_known[key];
  const auto now = Clock::now();
  if (k.status == Status::kAsking) return;
  if (k.have && now - k.got < kFresh) return;
  k.asked = now;
  k.nonce = fmt::format("{:08x}", std::random_device{}());
  if (back.empty() || back == room || lobby::Snapshot().link != lobby::Link::kOnline) {
    k.status = Status::kNoAnswer;
    return;
  }
  k.status = Status::kAsking;
  lobby::SigToGuest(room, "pf", {{"pf", "ask"}, {"back", back}, {"from", MyName()}, {"n", k.nonce}});
}

Status Get(const std::string& name, Card* out) {
  const auto it = g_known.find(Key(name));
  if (it == g_known.end()) return Status::kNone;
  if (it->second.have) {
    if (out) *out = it->second.card;
    return Status::kHave;
  }
  return it->second.status;
}

void Pump() {
  lobby::Event event;
  while (lobby::PollProfile(&event)) {
    if (!event.data.is_object()) continue;
    const std::string kind = Str(event.data, "pf", 8);
    if (kind == "ask") {
      Answer(event.data);
    } else if (kind == "av?") {
      SendPicture(event.data);
    } else if (kind == "card") {
      TakeCard(event.data);
    } else if (kind == "av") {
      TakePicture(event.data);
    }
  }
  const auto now = Clock::now();
  for (auto& [key, k] : g_known) {
    if (k.status == Status::kAsking && now - k.asked > kAskFor) k.status = Status::kNoAnswer;
  }
}

}  // namespace mk9::profiles
