// MK9 online: the player's record of online matches (stats.h).

#include "online/stats.h"

#include "mk9_platform.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>
#include <rex/logging.h>

namespace mk9::stats {
namespace {

using json = nlohmann::json;

std::filesystem::path File() {
  return mk9::platform::ExeDir() / "online_stats.jsonl";
}

std::mutex g_mutex;
std::vector<Match> g_matches;  // oldest first
bool g_loaded = false;

// The mutex held.
void LoadLocked() {
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream in(File(), std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const json j = json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    Match m;
    m.at = j.value("at", int64_t(0));
    m.rival = j.value("rival", "");
    m.rival_country = j.value("cc", "");
    m.mine = j.value("mine", "");
    m.theirs = j.value("theirs", "");
    m.result = j.value("result", 0);
    g_matches.push_back(std::move(m));
  }
}

}  // namespace

void Add(const Match& match) {
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadLocked();
  g_matches.push_back(match);
  std::ofstream out(File(), std::ios::binary | std::ios::app);
  out << json{{"at", match.at},           {"rival", match.rival}, {"cc", match.rival_country},
              {"mine", match.mine},       {"theirs", match.theirs}, {"result", match.result}}
             .dump()
      << "\n";
  REXLOG_INFO("[stats] {} vs {} ({} vs {}): {}", match.result > 0 ? "won" : match.result < 0 ? "lost" : "no result",
              match.rival, match.mine, match.theirs, match.result);
}

std::vector<Match> Recent(size_t n) {
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadLocked();
  std::vector<Match> out;
  for (auto it = g_matches.rbegin(); it != g_matches.rend() && out.size() < n; ++it) out.push_back(*it);
  return out;
}

Summary Totals() {
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadLocked();
  Summary s;
  std::map<std::string, int> characters;
  for (const Match& m : g_matches) {
    ++s.played;
    if (m.result > 0) ++s.won;
    if (m.result < 0) ++s.lost;
    if (!m.mine.empty()) ++characters[m.mine];
  }
  s.characters.assign(characters.begin(), characters.end());
  std::stable_sort(s.characters.begin(), s.characters.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  return s;
}

}  // namespace mk9::stats
