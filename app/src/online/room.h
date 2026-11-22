// MK9 online: a room of two (docs/online/lobby.md): one player hosts it (a code of six letters and digits, in the
// lobby's list when open, by code only when private), the other joins it (from the list, by code, or quick match: the
// room waiting longest, else a new open one). They talk through the lobby's relay (lobby.h, sig) until the fight's
// own path is up: hello, the room's chat, ready, and the host's start. Then both games start again already joined
// (session.h): the session runs from the first frame, the same on both machines.
//
// mk9-lauta (2026-11-11): the hello, the welcome and the pings carry the player's picture's hash too ("av"); a game
// that lacks it asks "av?" {h} (a spectator with its id) and gets "av" {h, d} (avatars.h).
//
// Messages relayed (data.room): "hi" {name, id, cc} the guest to the host, again every second until welcomed; "welcome"
// {name, cc} or "full" (cc: the player's country, flags.h); "chat" {text}; "ready" {on}; "start" the host's; "bye"; "ping" every 3 s (12 s without a word: the
// other is gone).
//
// mk9-lauta (2026-11-17): a room of four (two against two, each at a PC or two at one: four people at most, four
// machines at most). The host has the room's word: the guests talk to it alone, and it tells every guest the members
// ("members" {cap, list: [{id, name, cc, won, lost, av, mods, pl, ready}]}, the host's first) and passes each guest's
// chat to the others ("chat" {text, name}). Its "start" says every machine's lobby id in the room's order ({machines});
// the session is of that many machines (session.h). A guest's "hi" says the release's session protocol ("proto") and
// what it looks for ("want": 2 a quick one against one, 4 a quick two against two, 0 anything): a room of four takes
// only a release that plays it. Its title starts with "2v2 · " (kFourTitle): the quick matches tell it by that.
//
// mk9-lauta (2026-11-17): a room of King of the Hill (koth): a room of four's machinery with one person at each PC
// (three or four). Its session is of every machine (mk9_net_koth): two fight, the others watch the same fight live;
// the winner stays and the loser goes to the end of the line (netplay.cpp). Its title starts with "KOTH · "
// (kKothTitle); only this release's session protocol (13) joins it ("koth" in the welcome, the members and the start).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mk9::room {

enum class Phase {
  kNone,       // in no room
  kSearching,  // quick match: waiting for the lobby's answer
  kHosting,    // hosting, no guest yet
  kJoining,    // joining, the host has not welcomed us yet
  kTogether,   // both in the room
  kStarting,   // the fight is starting (the game starts again)
  kWatching,   // mk9-lauta (2026-11-10): this game watches the room's session (feed.h)
};

struct Line {
  std::string name, text;
  bool system = false;  // the room's own news
};

// mk9-lauta (2026-11-17): one in a room (a machine: one or two people at it).
struct Member {
  std::string id;  // its lobby id in the room (the host's: empty)
  std::string name, country, avatar, mods;
  int won = -1, lost = -1;
  int streak = 0;  // mk9-lauta (2026-11-17): its winning streak
  int players = 1;
  bool ready = false;
  bool me = false;  // this game's
};

// The title's mark of a room of four (the quick matches tell the rooms by it).
inline constexpr const char* kFourTitle = "2v2 · ";
// mk9-lauta (2026-11-17): ... and of a room of King of the Hill.
inline constexpr const char* kKothTitle = "KOTH · ";

struct State {
  Phase phase = Phase::kNone;
  std::string code, title;
  bool open = false;          // in the lobby's list
  bool host = false;          // this side hosts it
  bool in_session = false;    // this game is the room's session already
  std::string me, other;      // the names
  std::string other_country;  // the other player's (two letters; "" not said): mk9-lauta, flags.h
  int other_won = -1, other_lost = -1;  // the other player's online record (stats.h; -1: not said)
  int other_streak = 0;                 // mk9-lauta (2026-11-17): ... and its winning streak (0: none, or not said)
  std::string other_avatar;   // mk9-lauta (2026-11-11): the other player's picture's hash (avatars.h; "" none)
  // mk9-lauta (2026-11-11): the content the other's game has beyond the disc ("kratos"; "" none, or an older release):
  // the fight starts only when both have the same (mk9_kratos.h); content_differs says so for the screens
  std::string other_mods;
  bool content_differs = false;
  // mk9-lauta (2026-11-10): spectators. The watching game: the two players (the host's answer), or the room has
  // feed::kMaxWatchers already (watch_full); the host: how many watch.
  bool watching = false, watch_full = false;
  std::string watch_mods;  // the match needs content this game lacks, or lacks content this one has (its "mods")
  std::string p1, p2, p1_country, p2_country, p1_avatar, p2_avatar;
  int watchers = 0;
  bool my_ready = false, other_ready = false;
  // mk9-lauta (2026-11-13): players at each machine (2: two against two with four players, the second at the same
  // PC; mk9_net_local_players in the session); the other's as it said (1 until then, or an older release)
  int my_players = 1, other_players = 1;
  // mk9-lauta (2026-11-17): the room's people at most (2: one against one; 4: two against two), and everyone in it in
  // the room's order, the host first and this player among them (the host's word; the "other" fields above are the
  // first other one's: in a room of two, the rival)
  int capacity = 2;
  bool koth = false;  // mk9-lauta (2026-11-17): a room of King of the Hill (capacity 4, one person at each PC)
  std::vector<Member> members;
  bool can_start = false;  // the host of a room of four: everyone in it is ready (the start is its button)
  std::vector<Line> chat;
  std::string status;         // a line for the screen
  uint64_t version = 0;
};

// capacity (mk9-lauta, 2026-11-17): 2 one against one, 4 two against two; koth: a room of King of the Hill (up to four
// people, one at each PC).
void CreateRoom(bool open, int capacity = 2, bool koth = false);
// want (mk9-lauta, 2026-11-17): what a quick match joins it for (2 or 4; 0: anything, from the list or a code).
void JoinRoom(const std::string& code, int want = 0);
void QuickMatch();
// mk9-lauta (2026-11-17): the host of a room of four starts its fight (everyone in it ready: can_start).
void Start();
void Leave();
void SetReady(bool ready);
// mk9-lauta (2026-11-13): one or two players at this machine (said to the other one).
void SetPlayers(int players);
void Say(const std::string& text);
State Snapshot();
// Every overlay frame (the UI thread): the relayed messages, the keepalives, the start.
void Pump();
// This game is a room's session (netplay.cpp, mk9_net_room): the room as it was, for its chat. id: the guest's id the
// session joined the room with (the lobby keeps one per player; its ICE handshake uses it too). The host's title and
// open (in the lobby's list) are what it announces the room with again (a guest's hello).
// mk9-lauta (2026-11-17): machines: a session of more than two machines' lobby ids in the room's order (the host's
// first, empty): the host welcomes those guests back.
void EnterSession(const std::string& code, bool host, const std::string& me, const std::string& id,
                  const std::string& title, bool open, const std::vector<std::string>& machines = {});
// mk9-lauta (2026-11-10): spectators. Watch: the lobby's screens start the game again watching the room (a playing
// one); EnterWatch: that game (netplay.cpp, mk9_net_role=watch) joins the room with its own id and asks the host for
// the match (feed.h).
void Watch(const std::string& code);
void EnterWatch(const std::string& code, const std::string& me, const std::string& id);

// mk9-lauta (2026-11-21): why a match cannot be watched when its content differs from this game's (State's
// watch_mods, a recording's: netplay's kOtherContent): Kratos on one side only, or the PS Vita costumes.
std::string WatchModsWhy(const std::string& match_mods);
// mk9-lauta (2026-11-21): this game's content beyond the disc as the rooms say it ("kratos", "vita-<set>", joined by
// '+'; "" none): the recordings carry it too.
std::string ContentMods();

}  // namespace mk9::room
