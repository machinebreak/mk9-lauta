// MK9 online: a room of two (docs/online/lobby.md): one player hosts it (a code of six letters and digits, in the
// lobby's list when open, by code only when private), the other joins it (from the list, by code, or quick match: the
// room waiting longest, else a new open one). They talk through the lobby's relay (lobby.h, sig) until the fight's
// own path is up: hello, the room's chat, ready, and the host's start. Then both games start again already joined
// (session.h): the session runs from the first frame, the same on both machines.
//
// Messages relayed (data.room): "hi" {name, id, cc} the guest to the host, again every second until welcomed; "welcome"
// {name, cc} or "full" (cc: the player's country, flags.h); "chat" {text}; "ready" {on}; "start" the host's; "bye"; "ping" every 3 s (12 s without a word: the
// other is gone).

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

struct State {
  Phase phase = Phase::kNone;
  std::string code, title;
  bool open = false;          // in the lobby's list
  bool host = false;          // this side hosts it
  bool in_session = false;    // this game is the room's session already
  std::string me, other;      // the names
  std::string other_country;  // the other player's (two letters; "" not said): mk9-lauta, flags.h
  int other_won = -1, other_lost = -1;  // the other player's online record (stats.h; -1: not said)
  // mk9-lauta (2026-11-10): spectators. The watching game: the two players (the host's answer), or the room has
  // feed::kMaxWatchers already (watch_full); the host: how many watch.
  bool watching = false, watch_full = false;
  std::string p1, p2, p1_country, p2_country;
  int watchers = 0;
  bool my_ready = false, other_ready = false;
  std::vector<Line> chat;
  std::string status;         // a line for the screen
  uint64_t version = 0;
};

void CreateRoom(bool open);
void JoinRoom(const std::string& code);
void QuickMatch();
void Leave();
void SetReady(bool ready);
void Say(const std::string& text);
State Snapshot();
// Every overlay frame (the UI thread): the relayed messages, the keepalives, the start.
void Pump();
// This game is a room's session (netplay.cpp, mk9_net_room): the room as it was, for its chat. id: the guest's id the
// session joined the room with (the lobby keeps one per player; its ICE handshake uses it too). The host's title and
// open (in the lobby's list) are what it announces the room with again (a guest's hello).
void EnterSession(const std::string& code, bool host, const std::string& me, const std::string& id,
                  const std::string& title, bool open);
// mk9-lauta (2026-11-10): spectators. Watch: the lobby's screens start the game again watching the room (a playing
// one); EnterWatch: that game (netplay.cpp, mk9_net_role=watch) joins the room with its own id and asks the host for
// the match (feed.h).
void Watch(const std::string& code);
void EnterWatch(const std::string& code, const std::string& me, const std::string& id);

}  // namespace mk9::room
