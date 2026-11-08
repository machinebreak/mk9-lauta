// MK9 online: the lobby (docs/online/lobby.md): the players' names, the open rooms, the global chat, quick match, and
// the relay the two players of a room talk through until their own path is up (the ICE handshake, the room's chat).
//
// The server is the one the web games share (the VPS, wss://107-172-143-35.sslip.io/lobby?game=mk9pc; its source is
// ps2-web-recomp's scripts/lobby_server.py, which runs locally for tests too): JSON messages over a WebSocket, a lobby
// for each game. Here over WinHTTP's WebSocket (TLS for wss), one thread receiving and one sending; it reconnects by
// itself and says hello again with the name.
//
//   mk9_lobby_url=<ws(s)://host[:port]/lobby?game=mk9pc>   the lobby ("off": none; tests never use the real one)

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mk9::lobby {

struct Room {
  std::string code;       // 4-8 letters and digits
  std::string title, host, country;
  bool playing = false;   // else waiting for a guest
  int64_t since = 0;      // ms since 1970
};

struct ChatLine {
  int64_t id = 0;         // 0: a line of the lobby's own (a ban, a notice)
  std::string name, country, text;
  int64_t at = 0;         // ms since 1970
  bool staff = false;
};

enum class Link { kOff, kConnecting, kOnline, kRetrying };

struct State {
  Link link = Link::kOff;
  std::string name;         // the name the lobby gave
  bool named = false;       // the lobby took the name asked for
  std::string name_error;   // why not: "empty", "reserved", "token"
  int online = 0;           // players in this game's lobby
  std::vector<Room> rooms;  // the open ones, oldest first
  std::deque<ChatLine> chat;
  std::string notice;       // the staff's notice for every page
  std::string error;        // the last refusal: "slow", "links", "muted", "banned"
  uint64_t version = 0;     // bumped on every change
};

// What the room's flow waits for: a message the other player relayed, the answer to a quick search.
struct Event {
  enum class Kind { kSig, kMatch } kind = Kind::kSig;
  std::string room;         // kSig: the room it came through; kMatch: the room found ("" for none)
  nlohmann::json data;      // kSig: what the other player sent
};

// mk9_lobby_url is set.
bool Configured();
// Connects as name (any thread; again: a new name).
void Start(const std::string& name);
void Stop();
State Snapshot();

void SetName(const std::string& name);
void Chat(const std::string& text);
// Hosts the room code (open: in the list; playing: no longer waiting for a guest).
void Host(const std::string& room, const std::string& title, bool open, bool playing);
void Unhost();
// Joins the room code as the guest id (the host's relayed messages come to it).
void Join(const std::string& room, const std::string& id);
void Quick();
// Relays data to the room's host, or to its guest id.
void SigToHost(const std::string& room, const nlohmann::json& data);
void SigToGuest(const std::string& room, const std::string& id, const nlohmann::json& data);
bool PollEvent(Event* event);
// The relayed messages of the ICE handshake (their data has "ice"), apart: ice_link.cpp takes them.
bool PollIce(Event* event);

// The ICE servers for a room's path (GET /ice on the lobby's host: STUN, and TURN with credentials of hours), as the
// WebRTC API takes them: [{urls: [...], username, credential}]. Blocks; empty on failure.
nlohmann::json IceServers();

}  // namespace mk9::lobby
