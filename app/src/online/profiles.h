// MK9 online: another player's profile, asked for from the global chat (mk9-lauta, 2026-11-11).
//
// The lobby keeps no profiles: each game answers for its own player. The lobby's relay passes a message to a room's
// host or to one of its guests by an id (docs/online/lobby.md); each game in the lobby is also the guest "pf" of a room
// of its own whose code comes from its name (RoomOf: 8 letters and digits; the rooms' own codes have 6), whenever it is
// not the guest of a real room (lobby.cpp). A game that has a name from the chat asks that room for the profile and
// says where to answer (its own profile room): the record, the most played fighters, the last matches and the picture's
// hash. The picture follows only if the asker lacks it (avatars.h). The lobby only forwards these few KB, as it does a
// room's messages, and keeps nothing.
//
// A game answers each asker at most once every 10 s and 20 asks a minute, and sends its picture to each at most once a
// minute. A silenced player's asks get no answer. A player whose game is a room's guest (in a room it joined, or a
// session's guest) cannot be asked meanwhile: the asker hears nothing and says so after 6 s.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mk9::profiles {

struct Match {
  int64_t at = 0;  // seconds since 1970
  std::string rival, rival_country, mine, theirs;
  int result = 0;  // 1 won, -1 lost, 0 none
};

struct Card {
  std::string name, country, version, avatar;  // avatar: the picture's hash ("" none)
  int played = 0, won = 0, lost = 0;
  std::vector<std::pair<std::string, int>> characters;  // the most played, at most 3
  std::vector<Match> recent;                            // the newest first, at most 8
};

enum class Status { kNone, kAsking, kHave, kNoAnswer };

// The code of a name's profile room ("" for a name with no letters or digits).
std::string RoomOf(const std::string& name);
// Asks for a player's profile (UI thread): its answer comes through Pump.
void Ask(const std::string& name);
// What is known of a name's profile (UI thread).
Status Get(const std::string& name, Card* out);
// Every overlay frame (UI thread): the lobby's profile messages (asks to answer, answers, pictures), the asks timed out.
void Pump();

}  // namespace mk9::profiles
