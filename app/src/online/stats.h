// MK9 online: the player's record of online matches (mk9-lauta, 2026-11-10): each match decided in a session (who,
// from where, with which characters, won or lost), in online_stats.jsonl beside the executable, a line a match. The
// online screens' profile shows it; the rival sees the wins and losses.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mk9::stats {

struct Match {
  int64_t at = 0;  // seconds since 1970
  std::string rival, rival_country;
  std::string mine, theirs;  // the characters
  int result = 0;            // 1 won, -1 lost, 0 none (a tag match, a draw)
};

void Add(const Match& match);
// The newest first, n at most.
std::vector<Match> Recent(size_t n);

struct Summary {
  int played = 0, won = 0, lost = 0;
  std::vector<std::pair<std::string, int>> characters;  // the player's own, most played first
};
Summary Totals();

}  // namespace mk9::stats
